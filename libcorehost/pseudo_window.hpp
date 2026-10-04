// ── conpty/pseudo_window.hpp ───────────────────────────
// The window GetConsoleWindow returns, like conhost's ConPTY pseudo window:
// hidden, never shown, owned by the terminal's window once the terminal
// sends PtySignal SetParent.
//
// Clients compare it with their own windows. PSReadLine finds the terminal
// (to read its keyboard layout) by walking up its parent processes, and
// stops at the first one whose main window equals GetConsoleWindow(). With
// NULL that was pwsh itself, so it never reached the terminal and used the
// layout of its own thread: with a Russian default layout, Ctrl+W became
// "Ctrl+ц" and was typed as "ц".

#pragma once
#include <windows.h>

namespace corehost::conpty
{

class pseudo_window
{
  public:
    pseudo_window() noexcept
    {
        _ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!_ready)
            return;
        _thread = ::CreateThread(nullptr, 0, &pseudo_window::thread_main, this, 0, nullptr);
        if (_thread)
            ::WaitForSingleObject(_ready, 5000);
    }

    pseudo_window(const pseudo_window &) = delete;
    pseudo_window &operator=(const pseudo_window &) = delete;

    ~pseudo_window() noexcept
    {
        if (_thread)
        {
            if (_hwnd)
                ::PostMessageW(_hwnd, WM_CLOSE, 0, 0);
            ::WaitForSingleObject(_thread, 5000);
            ::CloseHandle(_thread);
        }
        if (_ready)
            ::CloseHandle(_ready);
    }

    // NULL if the window could not be created.
    HWND hwnd() const noexcept
    {
        return _hwnd;
    }

  private:
    static DWORD WINAPI thread_main(void *self) noexcept
    {
        static_cast<pseudo_window *>(self)->run();
        return 0;
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
    {
        switch (msg)
        {
        case WM_WINDOWPOSCHANGING:
            // Stays hidden even when a client calls ShowWindow on it.
            reinterpret_cast<WINDOWPOS *>(lp)->flags &= ~SWP_SHOWWINDOW;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Its own thread with a message loop: other processes send it messages
    // (ShowWindow, SetWindowText), which must not wait on the I/O loop.
    void run() noexcept
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &pseudo_window::wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"PseudoConsoleWindow";
        ::RegisterClassExW(&wc); // already registered is fine
        _hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0,
                                  0, 0, 0, nullptr, nullptr, instance, nullptr);
        ::SetEvent(_ready);
        if (!_hwnd)
            return;
        MSG m;
        while (::GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }

    HANDLE _ready = nullptr;
    HANDLE _thread = nullptr;
    HWND _hwnd = nullptr;
};

} // namespace corehost::conpty
