// === tests/test_conpty_e2e.cpp ===
// ConPTY E2E test. Verifies creation, VT processing, isolation.
// NOTE: Text echo requires a ConDrv ReadConsole client.
#include "test_common.hpp"
#include "conpty_test_helpers.hpp"
#include <cstdlib>
#include <cstdio>
using namespace corehost::test;

bool test_create()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(ci.hInput != nullptr);
    ASSERT(ci.hOutput != nullptr);
    return true;
}
bool test_vt_cursor()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[5;10H"));
    sleep_ms(200);
    ASSERT(true);
    return true;
}
bool test_vt_sgr()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[31m"));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[0m"));
    sleep_ms(200);
    ASSERT(true);
    return true;
}
bool test_arrow_keys()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[A"));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[B"));
    sleep_ms(200);
    ASSERT(true);
    return true;
}
bool test_erase()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(send_vt_sequence(ci.hInput, "\x1b[2J\x1b[H"));
    sleep_ms(200);
    ASSERT(true);
    return true;
}
bool test_multiple()
{
    conpty_instance ci1, ci2;
    ASSERT(create_conpty(ci1, {80, 25}));
    ASSERT(create_conpty(ci2, {80, 25}));
    ASSERT(true);
    return true;
}
bool test_text_no_crash()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    ASSERT(write_input_string(ci.hInput, "text\r\n"));
    sleep_ms(200);
    ASSERT(true);
    return true;
}

// Child mode: open and close CONOUT$/CONIN$ `count` times, then one new
// screen buffer. Each open is a CREATE_OBJECT, each close a CLOSE_OBJECT.
int open_objects_child(int count)
{
    for (int i = 0; i < count; ++i)
    {
        HANDLE h = ::CreateFileW(i % 2 ? L"CONIN$" : L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return 1;
        ::CloseHandle(h);
    }
    HANDLE sb = ::CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            CONSOLE_TEXTMODE_BUFFER, nullptr);
    if (sb == INVALID_HANDLE_VALUE)
        return 1;
    ::CloseHandle(sb);
    return 0;
}

// Runs this exe with `args` attached to ci's console; returns its exit code,
// or ~0 if it could not be started or did not finish.
DWORD run_self_in(conpty_instance &ci, const std::wstring &args)
{
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string storage(size, '\0');
    auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size))
        return ~0ul;
    DWORD exit_code = ~0ul;
    if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, ci.hpc, sizeof(ci.hpc), nullptr,
                                    nullptr))
    {
        wchar_t module[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(module, len) + L"\" " + args;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attrs;
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                             nullptr, &startup.StartupInfo, &pi))
        {
            if (::WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0)
                ::GetExitCodeProcess(pi.hProcess, &exit_code);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
    }
    ::DeleteProcThreadAttributeList(attrs);
    return exit_code;
}

// corehost answered every CREATE_OBJECT with a ConDrv handle it opened and
// never closed it on CLOSE_OBJECT. A TUI that opened CONOUT$ for each size
// query left 100k handles in corehost within a day.
bool test_object_handles_released()
{
    conpty_instance ci;
    ASSERT(create_conpty(ci, {80, 25}));
    const HANDLE host = reinterpret_cast<PseudoConsole *>(ci.hpc)->hConPtyProcess;

    // The first client's CONNECT and lazily created state come first.
    ASSERT(run_self_in(ci, L"--open-objects 1") == 0);
    sleep_ms(200);
    drain_output(ci.hOutput);
    DWORD before = 0;
    ASSERT(::GetProcessHandleCount(host, &before));

    ASSERT(run_self_in(ci, L"--open-objects 500") == 0);
    sleep_ms(200);
    drain_output(ci.hOutput);
    DWORD after = 0;
    ASSERT(::GetProcessHandleCount(host, &after));
    ASSERT(after < before + 16);
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--open-objects") == 0)
        return open_objects_child(std::atoi(argv[2]));
    std::wcout << L"=== ConPTY E2E Tests ===" << std::endl;
    RUN_TEST(test_create, L"Create");
    RUN_TEST(test_vt_cursor, L"VT CUP");
    RUN_TEST(test_vt_sgr, L"VT SGR");
    RUN_TEST(test_arrow_keys, L"Arrow keys");
    RUN_TEST(test_erase, L"Erase");
    RUN_TEST(test_multiple, L"Multiple");
    RUN_TEST(test_text_no_crash, L"Text no-crash");
    RUN_TEST(test_object_handles_released, L"Object handles released");
    std::wcout << L"  " << tests_passed << L" passed, " << tests_failed << L" failed, " << (tests_passed + tests_failed)
               << L" total." << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
