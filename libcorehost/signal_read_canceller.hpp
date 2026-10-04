// ── conpty/signal_read_canceller.hpp ──────────────────────
// Lets the loop thread block in ReadFile on vt_in while a client request
// (ReadConsole, RawRead, GetConsoleInput) is pending, without missing PtySignal
// messages.
//
// The pending wait used to be a 16 ms WaitForSingleObject on vt_in. An
// anonymous pipe handle is always signaled, so that wait returned at once and
// the loop spun a whole core in PeekNamedPipe for as long as a shell sat at its
// prompt waiting for a key. Pipes have no "data available" wait, so the loop
// now reads vt_in synchronously, which wakes on the first byte. The signal pipe
// still has to be served (resize, close), so while a read is armed this helper
// polls the signal pipe and cancels the read (CancelSynchronousIo) once a
// message or a broken pipe shows up. The helper sleeps while no read is armed.
// All console state stays on the loop thread.

#pragma once
#include <windows.h>
#include <mutex>
#include "win32/handle.hpp"
#include "utility/log.hpp"

namespace corehost::conpty
{

class signal_read_canceller
{
  public:
    signal_read_canceller() noexcept = default;
    signal_read_canceller(const signal_read_canceller &) = delete;
    signal_read_canceller &operator=(const signal_read_canceller &) = delete;

    ~signal_read_canceller() noexcept
    {
        if (_thread)
        {
            ::SetEvent(_stop);
            ::WaitForSingleObject(_thread, INFINITE);
            ::CloseHandle(_thread);
        }
        if (_armed)
            ::CloseHandle(_armed);
        if (_stop)
            ::CloseHandle(_stop);
        if (_loop_thread)
            ::CloseHandle(_loop_thread);
    }

    // Starts the helper on first use. Must be called on the loop thread: that
    // thread's reads are the ones it cancels. Returns false when the helper
    // cannot run; the caller then has to poll instead of blocking.
    [[nodiscard]] bool start(win32::handle_view signal) noexcept
    {
        if (_thread)
            return true;
        if (_failed || !signal.valid())
            return false;

        _signal = signal;
        const HANDLE self = ::GetCurrentProcess();
        if (::DuplicateHandle(self, ::GetCurrentThread(), self, &_loop_thread, THREAD_TERMINATE, FALSE, 0))
        {
            _stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            _armed = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (_stop && _armed)
                _thread = ::CreateThread(nullptr, 0, &signal_read_canceller::thread_main, this, 0, nullptr);
        }
        if (!_thread)
        {
            LOG("signal_read_canceller: could not start, err=%lu", ::GetLastError());
            _failed = true;
        }
        return _thread != nullptr;
    }

    // Bracket the blocking vt_in read. Holding the lock while flipping the
    // flag means a cancel can only ever hit that read.
    void enter_read() noexcept
    {
        std::lock_guard guard{_lock};
        _in_read = true;
        ::SetEvent(_armed);
    }

    void leave_read() noexcept
    {
        std::lock_guard guard{_lock};
        _in_read = false;
    }

  private:
    static constexpr DWORD poll_ms = 10;

    static DWORD WINAPI thread_main(void *self) noexcept
    {
        static_cast<signal_read_canceller *>(self)->run();
        return 0;
    }

    void run() noexcept
    {
        const HANDLE waits[] = {_stop, _armed};
        while (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
        {
            while (::WaitForSingleObject(_stop, poll_ms) == WAIT_TIMEOUT)
            {
                std::lock_guard guard{_lock};
                if (!_in_read)
                    break;
                DWORD avail = 0;
                if (::PeekNamedPipe(_signal.get(), nullptr, 0, nullptr, &avail, nullptr) && avail == 0)
                    continue;
                // A cancel that misses (the loop has not entered ReadFile yet)
                // is retried on the next tick while the signal is still unread.
                if (::CancelSynchronousIo(_loop_thread))
                    LOG3("signal_read_canceller: signal pending, cancelled vt_in read");
            }
        }
    }

    win32::handle_view _signal;
    HANDLE _loop_thread = nullptr;
    HANDLE _stop = nullptr;
    HANDLE _armed = nullptr;
    HANDLE _thread = nullptr;
    std::mutex _lock;
    bool _in_read = false;
    bool _failed = false;
};

} // namespace corehost::conpty
