#include "PipeServer.h"

#include <sddl.h>

#include <vector>

#include "../CecTest/PipeProtocol.h"

#pragma comment(lib, "advapi32.lib")

namespace cec {
namespace {

// Builds the SECURITY_ATTRIBUTES for the pipe from the SDDL in PipeProtocol.h.
// Caller owns the descriptor and must LocalFree it.
bool BuildSecurity(SECURITY_ATTRIBUTES& attributes,
                   PSECURITY_DESCRIPTOR& descriptor) {
    descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

}  // namespace

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(const wchar_t* name, Handler handler,
                       const std::function<void(const char*)>& log) {
    handler_ = std::move(handler);
    log_ = log;
    stopping_ = 0;

    // Created here, on the caller's thread, and handed to the listener rather
    // than created twice.
    //
    // FILE_FLAG_FIRST_PIPE_INSTANCE fails outright if anyone already holds this
    // name, which is the squatting case the DACL exists to prevent - so it is
    // fatal rather than something to retry into someone else's pipe. An earlier
    // version proved the name with a throwaway instance, closed it, and let the
    // thread create the real one: between those two steps the name belonged to
    // nobody, and a squatter taking it there would have had this service join
    // as an extra instance of *their* pipe, under *their* security descriptor.
    // Keeping the first instance open from the moment it is claimed leaves no
    // such window.
    SECURITY_ATTRIBUTES attributes = {};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!BuildSecurity(attributes, descriptor)) {
        if (log_) log_("pipe: could not build security descriptor");
        return false;
    }
    name_ = name;
    pipe_ = CreateNamedPipeW(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kPipeMaxMessage, kPipeMaxMessage, 0,
        &attributes);
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        pipe_ = nullptr;
        if (log_) log_("pipe: name already in use, refusing to serve");
        return false;
    }

    thread_ = CreateThread(
        nullptr, 0,
        [](LPVOID context) -> DWORD {
            static_cast<PipeServer*>(context)->Run();
            return 0;
        },
        this, 0, nullptr);
    if (!thread_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
        if (log_) log_("pipe: could not start the listener thread");
        return false;
    }
    return true;
}

void PipeServer::Stop() {
    if (!thread_) {
        // Start() can fail after claiming the name; the handle still has to go.
        if (pipe_) {
            CloseHandle(pipe_);
            pipe_ = nullptr;
        }
        return;
    }
    InterlockedExchange(&stopping_, 1);

    // Unblock ConnectNamedPipe by connecting to it once ourselves.
    HANDLE poke = CreateFileW(name_, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);

    WaitForSingleObject(thread_, 3000);
    CloseHandle(thread_);
    thread_ = nullptr;
    if (pipe_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
}

void PipeServer::Run() {
    // ONE instance, created once and reused across connections.
    //
    // Creating a fresh instance per connection - and closing the old one first -
    // leaves a window in which the pipe name does not exist at all. A client
    // connecting in that window gets ERROR_FILE_NOT_FOUND rather than
    // ERROR_PIPE_BUSY, so it fails immediately instead of waiting, and the
    // caller cannot tell that apart from "the service is not running". The tray
    // menu issues four requests back to back when it opens, so it hit this
    // constantly: settings appeared unchecked because the GET silently failed.
    //
    // DisconnectNamedPipe followed by another ConnectNamedPipe on the same
    // handle keeps the instance alive throughout, so the name is always there.
    //
    // The handle itself is the one Start() claimed; Stop() closes it.
    HANDLE pipe = pipe_;
    if (!pipe) return;

    std::vector<char> buffer(kPipeMaxMessage);

    while (!stopping_) {
        // Blocks here while idle, which is the whole point - no polling.
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ||
                               GetLastError() == ERROR_PIPE_CONNECTED;
        if (stopping_) break;

        if (connected) {
            DWORD read = 0;
            if (ReadFile(pipe, buffer.data(),
                         static_cast<DWORD>(buffer.size() - 1), &read, nullptr) &&
                read > 0) {
                std::string request(buffer.data(), read);
                while (!request.empty() &&
                       (request.back() == '\n' || request.back() == '\r')) {
                    request.pop_back();
                }

                std::string response =
                    handler_ ? handler_(request) : std::string("ERR no handler");
                response += '\n';
                DWORD written = 0;
                WriteFile(pipe, response.data(),
                          static_cast<DWORD>(response.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
    }
}

}  // namespace cec
