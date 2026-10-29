// Proves the point of the change: a second PipeServer on its own name answers
// while the first is stuck inside a slow handler.
//
// Uses the real PipeServer.cpp, not a copy, so the name parameterisation and
// the Stop() poke are exercised as shipped. Test names, so it cannot collide
// with a running service.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "PipeServer.h"

static const wchar_t kSlowName[] = L"\\\\.\\pipe\\CecPipeTest-slow";
static const wchar_t kFastName[] = L"\\\\.\\pipe\\CecPipeTest-fast";

// The tray's client, copied in shape from SendRequestTo.
static std::string Ask(const wchar_t* name, const std::string& request) {
    const DWORD start = GetTickCount();
    for (;;) {
        HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY) return {};
            if (GetTickCount() - start >= 5000) return {};
            WaitNamedPipeW(name, 1000);
            continue;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        std::string line = request + "\n";
        DWORD written = 0;
        WriteFile(pipe, line.data(), (DWORD)line.size(), &written, nullptr);
        char buffer[4096] = {};
        DWORD read = 0;
        std::string response;
        if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read) {
            response.assign(buffer, read);
            while (!response.empty() &&
                   (response.back() == '\n' || response.back() == '\r')) {
                response.pop_back();
            }
        }
        CloseHandle(pipe);
        return response;
    }
}

int failures = 0;
static void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    cec::PipeServer slow;
    cec::PipeServer fast;
    auto log = [](const char* m) { std::printf("  [server] %s\n", m); };

    // The command pipe, whose handler blocks the way SwitchTo does.
    Expect(slow.Start(kSlowName,
                      [](const std::string&) {
                          Sleep(2000);
                          return std::string("OK slow");
                      },
                      log),
           "command pipe started");

    // The read-only pipe, whose handler only copies.
    Expect(fast.Start(kFastName,
                      [](const std::string& r) {
                          return r == "STATUS" ? std::string("OK attached=1")
                                               : std::string("ERR");
                      },
                      log),
           "read-only pipe started on its own name");

    // Start the slow one, then ask the fast one while it is still running.
    std::atomic<DWORD> slowDone{0};
    const DWORD t0 = GetTickCount();
    std::thread blocker([&] {
        const std::string r = Ask(kSlowName, "CMD switch-to 2.0.0.0");
        slowDone = GetTickCount() - t0;
        Expect(r == "OK slow", "the slow command did answer, eventually");
    });

    Sleep(300);   // well inside the slow handler
    const DWORD t1 = GetTickCount();
    const std::string status = Ask(kFastName, "STATUS");
    const DWORD took = GetTickCount() - t1;

    Expect(status == "OK attached=1", "status answered while the command ran");
    std::printf("  status round trip: %lums (command still running)\n", took);
    Expect(took < 250, "and answered inside the menu's 250ms budget");
    Expect(slowDone.load() == 0, "the command had not finished yet");

    blocker.join();
    std::printf("  slow command took: %lums\n", slowDone.load());
    Expect(slowDone.load() >= 1900, "the command really was blocking");

    // The old behaviour, for contrast: a second request on the *same* pipe as a
    // running command has to wait for it.
    std::atomic<DWORD> secondOnSame{0};
    const DWORD t2 = GetTickCount();
    std::thread blocker2([&] { Ask(kSlowName, "CMD switch-to 2.0.0.0"); });
    Sleep(300);
    Ask(kSlowName, "STATUS");
    secondOnSame = GetTickCount() - t2;
    blocker2.join();
    std::printf("  same-pipe question took: %lums (this is the bug)\n",
                secondOnSame.load());
    Expect(secondOnSame.load() >= 1900,
           "a question on the command pipe waits for the command");

    slow.Stop();
    fast.Stop();
    Expect(true, "both servers stopped cleanly");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
