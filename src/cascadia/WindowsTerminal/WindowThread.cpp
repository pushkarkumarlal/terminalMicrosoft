// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowThread.h"

WindowThread::WindowThread(winrt::TerminalApp::AppLogic logic,
                           winrt::Microsoft::Terminal::Remoting::WindowRequestedArgs args,
                           winrt::Microsoft::Terminal::Remoting::WindowManager manager,
                           winrt::Microsoft::Terminal::Remoting::Peasant peasant) :
    _peasant{ std::move(peasant) },
    _appLogic{ std::move(logic) },
    _args{ std::move(args) },
    _manager{ std::move(manager) }
{
    // DO NOT start the AppHost here in the ctor, as that will start XAML on the wrong thread!
}

void WindowThread::CreateHost()
{
    const bool coldStart = _warmWindow == nullptr;
    // Start the AppHost HERE, on the actual thread we want XAML to run on
    _host = !coldStart ? std::make_unique<::AppHost>(_appLogic,
                                                     _args,
                                                     _manager,
                                                     _peasant,
                                                     std::move(_warmWindow)) :
                         std::make_unique<::AppHost>(_appLogic,
                                                     _args,
                                                     _manager,
                                                     _peasant);
    _UpdateSettingsRequestedToken = _host->UpdateSettingsRequested([this]() { _UpdateSettingsRequestedHandlers(); });

    if (coldStart)
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }

    // Initialize the xaml content. This must be called AFTER the
    // WindowsXamlManager is initialized.
    _host->Initialize();
}

int WindowThread::RunMessagePump()
{
    // Enter the main window loop.
    const auto exitCode = _messagePump();
    // Here, the main window loop has exited.
    return exitCode;
}

void WindowThread::RundownForExit()
{
    _host->UpdateSettingsRequested(_UpdateSettingsRequestedToken);
    _host->Close();

    // !! LOAD BEARING !!
    //
    // Make sure to finish pumping all the messages for our thread here. We
    // may think we're all done, but we're not quite. XAML needs more time
    // to pump the remaining events through, even at the point we're
    // exiting. So do that now. If you don't, then the last tab to close
    // will never actually destruct the last tab / TermControl / ControlCore
    // / renderer.
    {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::DispatchMessageW(&msg);
        }
    }
}
bool WindowThread::KeepWarm()
{
    if (_host != nullptr)
    {
        return true;
    }
    // If we're refrigerated, then wait on the microwave signal, which will be
    // raised when we get microwaved by another thread to reactivate us. .

    if (_warmWindow != nullptr)
    {
        // DO THE THING
        std::unique_lock lock(_microwave);
        _microwaveGoButton.wait(lock);
        const bool reheated = _host != nullptr;
        if (reheated)
        {
            _UpdateSettingsRequestedToken = _host->UpdateSettingsRequested([this]() { _UpdateSettingsRequestedHandlers(); });
            // Re-initialize the host here, on the window thread
            _host->Initialize();
        }
        return reheated;
    }
    else
    {
        return false;
    }
}

void WindowThread::Refrigerate()
{
    _host->UpdateSettingsRequested(_UpdateSettingsRequestedToken);

    // keep a reference to the HWND and DWXS alive.
    _warmWindow = std::move(_host->Refrigerate());

    // rundown remaining messages before dtoring the app host
    {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::DispatchMessageW(&msg);
        }
    }
    _host = nullptr;
}

void WindowThread::Microwave(
    winrt::Microsoft::Terminal::Remoting::WindowRequestedArgs args,
    winrt::Microsoft::Terminal::Remoting::Peasant peasant)
{
    _peasant = std::move(peasant);
    _args = std::move(args);
    // CreateHost();
    {
        _host = std::make_unique<::AppHost>(_appLogic,
                                            _args,
                                            _manager,
                                            _peasant,
                                            std::move(_warmWindow));
    }

    // raise the signal
    {
        // std::unique_lock lock(_microwave);
        _microwaveGoButton.notify_one();
    }
}

winrt::TerminalApp::TerminalWindow WindowThread::Logic()
{
    return _host->Logic();
}

static bool _messageIsF7Keypress(const MSG& message)
{
    return (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_F7;
}
static bool _messageIsAltKeyup(const MSG& message)
{
    return (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) && message.wParam == VK_MENU;
}
static bool _messageIsAltSpaceKeypress(const MSG& message)
{
    return message.message == WM_SYSKEYDOWN && message.wParam == VK_SPACE;
}

int WindowThread::_messagePump()
{
    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0))
    {
        // GH#638 (Pressing F7 brings up both the history AND a caret browsing message)
        // The Xaml input stack doesn't allow an application to suppress the "caret browsing"
        // dialog experience triggered when you press F7. Official recommendation from the Xaml
        // team is to catch F7 before we hand it off.
        // AppLogic contains an ad-hoc implementation of event bubbling for a runtime classes
        // implementing a custom IF7Listener interface.
        // If the recipient of IF7Listener::OnF7Pressed suggests that the F7 press has, in fact,
        // been handled we can discard the message before we even translate it.
        if (_messageIsF7Keypress(message))
        {
            if (_host->OnDirectKeyEvent(VK_F7, LOBYTE(HIWORD(message.lParam)), true))
            {
                // The application consumed the F7. Don't let Xaml get it.
                continue;
            }
        }

        // GH#6421 - System XAML will never send an Alt KeyUp event. So, similar
        // to how we'll steal the F7 KeyDown above, we'll steal the Alt KeyUp
        // here, and plumb it through.
        if (_messageIsAltKeyup(message))
        {
            // Let's pass <Alt> to the application
            if (_host->OnDirectKeyEvent(VK_MENU, LOBYTE(HIWORD(message.lParam)), false))
            {
                // The application consumed the Alt. Don't let Xaml get it.
                continue;
            }
        }

        // GH#7125 = System XAML will show a system dialog on Alt Space. We want to
        // explicitly prevent that because we handle that ourselves. So similar to
        // above, we steal the event and hand it off to the host.
        if (_messageIsAltSpaceKeypress(message))
        {
            _host->OnDirectKeyEvent(VK_SPACE, LOBYTE(HIWORD(message.lParam)), true);
            continue;
        }

        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return 0;
}

uint64_t WindowThread::PeasantID()
{
    return _peasant.GetID();
}
