// PipeServer.h - named pipe server for the tray companion app.
//
// One background thread, blocked in ConnectNamedPipe when idle, so this costs
// nothing while nothing is connected. Requests are handled one at a time, which
// is ample for a tray menu and keeps the CEC layer free of concurrent callers
// beyond the mutex that already guards it.
//
// Two of these are started, on different names: the command pipe, whose handler
// may take seconds, and the read-only pipe, whose handler only copies a
// snapshot. See kReadPipeName for why the second exists.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace cec {

class PipeServer {
public:
    // Handles one request line and returns the response line, without the
    // terminating newline. Called on the pipe thread.
    using Handler = std::function<std::string(const std::string& request)>;

    ~PipeServer();

    // Starts the listener thread. Returns false if the pipe could not be
    // created - most importantly if something else already owns the name, which
    // is treated as a hard failure rather than something to work around.
    bool Start(const wchar_t* name, Handler handler,
               const std::function<void(const char*)>& log);
    void Stop();

private:
    void Run();

    Handler handler_;
    std::function<void(const char*)> log_;
    HANDLE thread_ = nullptr;
    // Claimed in Start(), served by Run(), closed in Stop(). Held open for the
    // life of the server so the name never briefly belongs to nobody.
    HANDLE pipe_ = nullptr;
    // Kept because Stop() has to connect to this same name to unblock the
    // listener, and there is now more than one name in play.
    const wchar_t* name_ = nullptr;
    volatile LONG stopping_ = 0;
};

}  // namespace cec
