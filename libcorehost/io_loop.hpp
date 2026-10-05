// ── conpty/io_loop.hpp ────────────────────────────────────
// ConDrv I/O 事件循环。
//
// 功能分解：
// 1. READ_IO 缓冲池：本轮 READ_IO 提交上一条 completion，同时读取下一条
//    消息，因此接收缓冲不能覆盖上一条消息的 completion 数据；挂起请求的
//    缓冲也不能被覆盖，bridge 完成它时还要写回。
// 2. 空闲节流：没有 pending 和 completion 时先让 handler 处理 VT 输入，
//    再短暂等待 server；不能等待 InputAvailableEvent，因为它服务客户端输入。
// 3. 挂起请求：handler 返回 false 时请求留在 bridge 中，循环继续 READ_IO，
//    其他客户端线程的 Console API 照常处理；VT 输入到达时由
//    idle_input_waker 取消 READ_IO，on_idle 完成挂起的读取。
//
// message_router 提供 loop 需要的完整会话操作：ConDrv 消息分派、VT idle
// 输入服务、pending 请求等待和退出条件判断。

#pragma once
#include "connect_completion.hpp"
#include "message_router.hpp"
#include "condrv_io.hpp"
#include "perf_diag.hpp"
#include "utility/log.hpp"
#include "win32/wait.hpp"
#include <array>
#include <mutex>
#include <utility>

namespace corehost::conpty
{

inline constexpr DWORD io_loop_idle_wait_ms = 16;

// READ_IO on the ConDrv server handle is synchronous: it blocks until a
// client sends the next console message. Terminal input and PtySignal
// messages arrive on pipes and never wake it. A client that only waits on its
// input handle (WaitForSingleObject/WaitForMultipleObjects on stdin, as
// Windows OpenSSH does before ReadConsoleInputW) sends nothing, and a client
// whose read is pending sends nothing on that thread either, so the loop
// would sit in READ_IO with keystrokes waiting in vt_in.
//
// This watcher cancels that READ_IO (CancelIoEx on the server handle: it is
// opened for asynchronous I/O, so DeviceIoControl without an OVERLAPPED waits
// on the handle rather than doing synchronous I/O that CancelSynchronousIo
// could reach) when input or a PtySignal is waiting. The vt_in reader thread
// calls nudge() as bytes arrive; a 10 ms tick covers the signal pipe and a
// cancel that missed. The loop treats the abort as "no message" and runs
// on_idle, which turns the bytes into INPUT_RECORDs, completes a pending read
// and sets the event. All console state stays on the loop thread.
class idle_input_waker
{
  public:
    idle_input_waker(win32::handle_view server, win32::handle_view vt_in, win32::handle_view signal,
                     const pipe_bridge &bridge) noexcept
        : _server(server), _vt_in(vt_in), _signal(signal), _bridge(bridge)
    {
        if (!_server.valid())
            return;
        _stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (_stop)
            _thread = ::CreateThread(nullptr, 0, &idle_input_waker::thread_main, this, 0, nullptr);
    }

    idle_input_waker(const idle_input_waker &) = delete;
    idle_input_waker &operator=(const idle_input_waker &) = delete;

    ~idle_input_waker() noexcept
    {
        if (_thread)
        {
            ::SetEvent(_stop);
            ::WaitForSingleObject(_thread, INFINITE);
            ::CloseHandle(_thread);
        }
        if (_stop)
            ::CloseHandle(_stop);
    }

    // Bracket a READ_IO that may block. Holding the lock while flipping the
    // flag means a cancel can only ever hit that READ_IO.
    void enter_read() noexcept
    {
        std::lock_guard guard{_lock};
        _in_read = true;
    }

    void leave_read() noexcept
    {
        std::lock_guard guard{_lock};
        _in_read = false;
    }

    // The vt_in reader owns the pipe from now on (true), or no longer runs
    // (false): ticks then stop asking it for input.
    void use_reader(bool on) noexcept
    {
        std::lock_guard guard{_lock};
        _reader = on;
    }

    // Reader thread: bytes or EOF arrived on vt_in.
    void nudge() noexcept
    {
        std::lock_guard guard{_lock};
        // A cancel that lands before the loop is inside DeviceIoControl misses;
        // the next tick retries while the bytes are still queued.
        if (_in_read && ::CancelIoEx(_server.get(), nullptr))
            LOG3("idle_input_waker: vt input arrived, cancelled READ_IO");
    }

    static void nudge_thunk(void *self) noexcept
    {
        static_cast<idle_input_waker *>(self)->nudge();
    }

  private:
    static DWORD WINAPI thread_main(void *self) noexcept
    {
        static_cast<idle_input_waker *>(self)->run();
        return 0;
    }

    void run() noexcept
    {
        while (::WaitForSingleObject(_stop, 10) == WAIT_TIMEOUT)
        {
            const bool signal = signal_waiting();
            std::lock_guard guard{_lock};
            if (!_in_read)
                continue;
            if (!signal && !input_waiting())
                continue;
            if (::CancelIoEx(_server.get(), nullptr))
                LOG3("idle_input_waker: %hs waiting, cancelled READ_IO", signal ? "signal" : "input");
        }
    }

    // Called with _lock held, so use_reader(false) cannot pull the reader away
    // in the middle.
    [[nodiscard]] bool input_waiting() const noexcept
    {
        if (_reader)
            return _bridge.vt_input_queued();
        // No reader thread: peek the pipe. The loop does not read it while it
        // sits in READ_IO.
        DWORD avail = 0;
        return _vt_in.valid() && ::PeekNamedPipe(_vt_in.get(), nullptr, 0, nullptr, &avail, nullptr) && avail != 0;
    }

    // A PtySignal (resize, clear) or a closed signal pipe.
    [[nodiscard]] bool signal_waiting() const noexcept
    {
        if (!_signal.valid())
            return false;
        DWORD avail = 0;
        if (!::PeekNamedPipe(_signal.get(), nullptr, 0, nullptr, &avail, nullptr))
            return true;
        return avail != 0;
    }

    win32::handle_view _server;
    win32::handle_view _vt_in;
    win32::handle_view _signal;
    const pipe_bridge &_bridge;
    HANDLE _stop = nullptr;
    HANDLE _thread = nullptr;
    std::mutex _lock;
    bool _in_read = false;
    bool _reader = false;
};

// Runs the vt_in reader for the loop's lifetime and makes it wake the loop.
// Declared after the waker, so it stops the reader before the waker goes away.
class vt_input_reader_scope
{
  public:
    vt_input_reader_scope(pipe_bridge &bridge, idle_input_waker &waker) noexcept : _bridge(bridge), _waker(waker)
    {
        _running = _bridge.start_vt_input_reader(&idle_input_waker::nudge_thunk, &_waker);
        _waker.use_reader(_running);
    }

    vt_input_reader_scope(const vt_input_reader_scope &) = delete;
    vt_input_reader_scope &operator=(const vt_input_reader_scope &) = delete;

    ~vt_input_reader_scope() noexcept
    {
        if (!_running)
            return;
        _waker.use_reader(false);
        _bridge.stop_vt_input_reader();
    }

  private:
    pipe_bridge &_bridge;
    idle_input_waker &_waker;
    bool _running = false;
};

// 运行 ConDrv READ_IO/COMPLETE_IO 主循环。server/event 都是非拥有句柄；
// router 持有实际状态机。函数通过 READ_IO piggyback 提交同步 completion，
// 挂起的读取由 bridge 在 VT 输入到达后显式完成，并在 server 断开或 bridge
// 可退出时返回。
inline void run_io_loop_no_setup(win32::handle_view server, win32::handle_view ev, message_router &router,
                                 win32::handle_view vt_in = {})
{
    idle_input_waker waker{server, vt_in, router.bridge.signal_pipe(), router.bridge};
    vt_input_reader_scope reader{router.bridge, waker};

    // server 是 ConDrv \Server 等待/READ_IO 目标；ev 是客户端 input-available
    // 事件，只用于判断 completion 前是否需要先刷 VT 输出。router 持有
    // io_state/pipe_bridge/api_router，是本循环消费消息的唯一入口。
    LOG("run_io_loop_no_setup: enter");

    // Message buffers. READ_IO submits the previous completion in the same
    // call that reads the next message, so the buffer holding that completion
    // (prev_done) must not be read into; a pending read keeps pointing into its
    // buffer (pending_msg) until the bridge completes it, and a second read
    // waits in its own (deferred_msg). Four buffers always leave one free for
    // the next READ_IO.
    std::array<corehost::condrv_io::io_msg, 4> msgs{};
    corehost::condrv_io::io_msg *cur = &msgs[0];

    // nullptr 表示本轮不提交 completion；非空表示上一条消息已经处理完，
    // 其 completion 必须作为下一轮 READ_IO 的输入。
    corehost::condrv_io::io_msg *prev_done = nullptr;

    // The request the bridge holds while it waits for terminal input.
    corehost::condrv_io::io_msg *pending_msg = nullptr;

    // A read from another client thread that arrived while pending_msg was
    // waiting. The bridge holds one waiting read, so this one is dispatched
    // once the first is completed, or dropped because its process left.
    corehost::condrv_io::io_msg *deferred_msg = nullptr;

    const auto next_free = [&]() noexcept {
        for (auto &m : msgs)
            if (&m != prev_done && &m != pending_msg && &m != deferred_msg)
                return &m;
        std::unreachable();
    };

    for (;;)
    {
        if (pending_msg && !router.has_pending())
            pending_msg = nullptr;

        if (deferred_msg && !router.has_pending())
        {
            auto *m = std::exchange(deferred_msg, nullptr);
            LOG2("dispatch deferred read func=%lu id=%08lx:%08lx", m->descriptor.Function,
                 m->descriptor.Identifier.HighPart, m->descriptor.Identifier.LowPart);
            if (router.on_message(*m))
            {
                // prev_done may already carry a completion for the next
                // READ_IO, so this one is submitted on its own.
                if (router.has_buffered_vt_output())
                    router.flush_vt_output();
                corehost::condrv_io::complete_io(server, m->complete);
            }
            else
            {
                pending_msg = m;
            }
            if (router.should_exit())
                break;
        }

        // 只有在没有 pending 请求、也没有 completion 待提交时才允许等待。
        // prev_done 非空时必须立即 READ_IO，把 completion 交回 ConDrv。
        if (!router.has_pending() && prev_done == nullptr)
        {
            // router.on_idle 会主动读取 vt_in；键盘输入不会唤醒 server，所以等待前
            // 必须先服务一次终端输入。
            router.on_idle();
            if (router.should_exit())
                break;
            if (!router.has_pending())
            {
                // 16ms 只在已经主动服务过一次 vt_in 后用于空闲节流；新的
                // ConDrv 消息会唤醒 server，终端输入由下一轮 on_idle 轮询。
                COREHOST_PERF_SCOPE(io_server_idle_wait);
                const auto wait = win32::wait_one(server, io_loop_idle_wait_ms);
                if (wait.abandoned())
                {
                    LOG("run_io_loop_no_setup: server idle wait abandoned");
                    router.flush_vt_output();
                    return;
                }
            }
        }
        else if (router.has_pending())
        {
            // Input that arrived while the loop was busy can complete the
            // pending read right away instead of waiting for the waker.
            router.on_idle();
            if (router.should_exit())
                break;
            if (pending_msg && !router.has_pending())
                pending_msg = nullptr;
        }

        // prev_comp 非空时由 ConDrv 消费；read_io_try 返回 no_message 后也不
        // 能再次提交同一个 completion。
        CD_IO_COMPLETE *prev_comp = prev_done ? &prev_done->complete : nullptr;
        const bool submitting_previous_completion = prev_comp != nullptr;
        LOG2_IF(submitting_previous_completion, "submitting completion id=%08lx:%08lx status=0x%08lx info=%llu",
                prev_done->descriptor.Identifier.HighPart, prev_done->descriptor.Identifier.LowPart,
                prev_done->complete.IoStatus.Status, prev_done->complete.IoStatus.Information);
        if (submitting_previous_completion && router.has_buffered_vt_output() && ev.valid())
        {
            COREHOST_PERF_SCOPE(io_server_wait_0);
            const auto wait = win32::wait_one(ev, 0);
            if (wait.abandoned())
            {
                LOG("run_io_loop_no_setup: input event wait abandoned");
                router.flush_vt_output();
                return;
            }
            if (!wait.signaled())
                router.flush_vt_output();
        }

        // read_io_try 的三个结果含义：
        // disconnected: server 已断开，本循环结束。
        // no_message   : completion 已提交，但暂时没有新消息。
        // message      : cur 中已有一条新的 ConDrv 消息。
        auto read_result = corehost::condrv_io::read_io_result::no_message;
        {
            COREHOST_PERF_SCOPE(io_read_io_try);
            waker.enter_read();
            read_result = corehost::condrv_io::read_io_try(server, prev_comp, *cur);
            waker.leave_read();
        }
        if (read_result == corehost::condrv_io::read_io_result::disconnected)
        {
            router.flush_vt_output();
            LOG("run_io_loop_no_setup: read_io false, exiting");
            break;
        }
        if (read_result == corehost::condrv_io::read_io_result::no_message)
        {
            // no_message 仍可能已经消费 prev_comp，因此必须清空 prev_done。
            prev_done = nullptr;
            LOG2("READ_IO returned no message");

            if (router.has_buffered_vt_output())
                router.flush_vt_output();

            // 0ms 只触发一次非阻塞等待，让 ConDrv 消化 pending 状态。
            {
                COREHOST_PERF_SCOPE(io_server_wait_0);
                const auto wait = win32::wait_one(server, 0);
                if (wait.abandoned())
                {
                    LOG("run_io_loop_no_setup: server pending wait abandoned");
                    router.flush_vt_output();
                    return;
                }
            }
            router.on_idle();
            if (router.should_exit())
                break;
            if (!router.has_pending())
            {
                // 16ms 只在 READ_IO 已确认暂时无消息后让步。
                COREHOST_PERF_SCOPE(io_server_idle_wait);
                const auto wait = win32::wait_one(server, io_loop_idle_wait_ms);
                if (wait.abandoned())
                {
                    LOG("run_io_loop_no_setup: server no-message wait abandoned");
                    router.flush_vt_output();
                    return;
                }
            }
            continue;
        }
        prev_done = nullptr; // completion 已被 read_io 消费
        if (submitting_previous_completion && router.should_flush_vt_output())
            router.flush_vt_output();

        if (cur->descriptor.Function == 0)
        {
            // Function==0 是空 descriptor，不对应可完成的 Console I/O。
            LOG2("empty ConDrv descriptor");
            router.on_idle();
            if (router.should_exit())
                break;
            continue;
        }

        if (cur->descriptor.Function != CONSOLE_IO_CONNECT)
        {
            // The bridge holds one waiting read. A second one (another client
            // thread) waits in its buffer until the first is done; meanwhile
            // other requests, a DISCONNECT that drops the first among them,
            // are served as usual.
            if (router.has_pending() && message_router::may_wait_for_input(*cur))
            {
                if (!deferred_msg)
                {
                    LOG2("read func=%lu deferred until the pending read is done", cur->descriptor.Function);
                    deferred_msg = cur;
                    cur = next_free();
                    continue;
                }
                // A third concurrent read: serve input only until the bridge
                // is free, the way every pending read used to be handled.
                LOG2("read func=%lu waits for the pending reads to finish", cur->descriptor.Function);
                while (router.has_pending() || deferred_msg)
                {
                    while (router.has_pending())
                        router.wait_for_pending_input();
                    pending_msg = nullptr;
                    if (router.should_exit() || !deferred_msg)
                        break;
                    auto *m = std::exchange(deferred_msg, nullptr);
                    if (router.on_message(*m))
                    {
                        if (router.has_buffered_vt_output())
                            router.flush_vt_output();
                        corehost::condrv_io::complete_io(server, m->complete);
                    }
                    else
                    {
                        pending_msg = m;
                    }
                }
                if (router.should_exit())
                    break;
            }

            LOG2("dispatch message func=%lu id=%08lx:%08lx", cur->descriptor.Function,
                 cur->descriptor.Identifier.HighPart, cur->descriptor.Identifier.LowPart);
            // true 表示 router 已经填好 cur->complete，下一轮 READ_IO 提交。
            // false 表示请求挂起，router 会在后续 VT 输入到达时显式完成。
            if (router.on_message(*cur))
            {
                // 应用输出可能包含 CPR/DA/OSC 查询，终端响应只会从 vt_in 回来，
                // 不会唤醒 ConDrv server。此时必须先让终端看见输出并完成本条
                // I/O，再回到 idle 读取响应；否则下一轮 READ_IO 可能阻塞，导致
                // 依赖终端查询响应的程序卡住。
                if (router.has_buffered_vt_output())
                {
                    router.flush_vt_output();
                    corehost::condrv_io::complete_io(server, cur->complete);
                    LOG2("message completed explicitly after VT flush func=%lu", cur->descriptor.Function);
                    prev_done = nullptr;
                    router.on_idle();
                    if (router.should_exit())
                        break;
                }
                else
                {
                    // 普通输出保留 piggyback completion；下一轮 READ_IO 提交
                    // completion 前会 flush 缓冲 VT，避免额外 COMPLETE_IO 往返。
                    if (router.should_exit())
                    {
                        if (router.has_buffered_vt_output())
                            router.flush_vt_output();
                        corehost::condrv_io::complete_io(server, cur->complete);
                        LOG2("message completed explicitly before shutdown func=%lu", cur->descriptor.Function);
                        break;
                    }
                    prev_done = cur;
                }
            }
            else
            {
                // The bridge completes it when terminal input arrives. Until
                // then the loop keeps serving other client threads, e.g. a
                // GetConsoleScreenBufferInfo from PowerShell's main thread
                // while its key reader waits in ReadConsoleInput.
                LOG2("message pending func=%lu", cur->descriptor.Function);
                pending_msg = cur;
                if (router.should_exit())
                    break;
            }
            cur = next_free();
            continue;
        }

        connect_completion connect_result = connect_completion::explicit_complete;
        LOG2("dispatch CONNECT id=%08lx:%08lx", cur->descriptor.Identifier.HighPart,
             cur->descriptor.Identifier.LowPart);
        if (!router.on_connect(*cur, connect_result))
        {
            router.flush_vt_output();
            return;
        }

        // inline_complete 说明 handler 只填了 cur->complete，仍需要下一轮 READ_IO
        // 把 completion 交给 ConDrv。
        if (connect_result == connect_completion::inline_complete)
        {
            prev_done = cur;
        }
        else
        {
            // 首个 CONNECT 可能由 corehost::condrv_io::accept_connection() 通过
            // IOCTL_COMPLETE_IO 显式完成。此时不能再把同一个 completion
            // 作为下一轮 READ_IO 的输入重复提交。
            prev_done = nullptr;
        }
        cur = next_free();
    }

    router.flush_vt_output();
    LOG("run_io_loop_no_setup: exit");
}

} // namespace corehost::conpty
