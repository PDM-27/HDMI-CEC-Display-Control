// CecSetup - installer and uninstaller for HDMI-CEC Display Control.
//
// One binary does both. A copy of it is left in the install directory as
// Uninstall.exe, and it decides which face to show from its own filename, so
// the uninstaller can never be a stale build of the installer - and it carries
// the same embedded payload, which is what makes Repair possible at all.
//
// Both exes are embedded as RCDATA, so this is the only file that has to be
// distributed. Everything it touches is listed in Remove() and nowhere else,
// which is the property that keeps uninstall honest.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#include "resource.h"
#include "../CecTest/Version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#pragma comment(linker,                                       \
                "\"/manifestdependency:type='win32' "         \
                "name='Microsoft.Windows.Common-Controls' "   \
                "version='6.0.0.0' "                          \
                "processorArchitecture='*' "                  \
                "publicKeyToken='6595b64144ccf1df' "          \
                "language='*'\"")

namespace {

// From Version.h, which also feeds every exe's VERSIONINFO, so Installed Apps
// and the files themselves cannot disagree about which version this is.
constexpr wchar_t kProductName[] = CEC_WIDEN(CEC_PRODUCT_NAME);
constexpr wchar_t kPublisher[] = CEC_WIDEN(CEC_PUBLISHER);
constexpr wchar_t kVersion[] = CEC_WIDEN(CEC_VERSION_STRING);
constexpr wchar_t kServiceName[] = L"CecService";
constexpr wchar_t kServiceExe[] = L"CecService.exe";
constexpr wchar_t kTrayExe[] = L"CecTray.exe";
constexpr wchar_t kUninstallExe[] = L"Uninstall.exe";
constexpr wchar_t kShortcutName[] = L"HDMI-CEC Display Control.lnk";
constexpr wchar_t kTrayWindowClass[] = L"CecTrayWindow";

constexpr wchar_t kArpKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CecDisplayControl";
constexpr wchar_t kServiceSettingsKey[] = L"SOFTWARE\\CecService";
constexpr wchar_t kTrayUserKey[] = L"Software\\CecTray";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CecTray";

// What a removal is allowed to take out. Named rather than boolean so the call
// sites read as intent.
enum class Scope { Service, App, Everything };

// ---------------------------------------------------------------- paths

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

std::wstring OwnPath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return (n && n < MAX_PATH) ? std::wstring(path, n) : std::wstring();
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Where the app lives. Fixed once at startup rather than recomputed, because
// the answer differs by which face is running and by what the user chose: the
// uninstaller has to work from wherever it was actually installed, not from
// wherever a fresh install would have gone.
std::wstring g_installDir;

std::wstring DefaultInstallDir() {
    const std::wstring base = KnownFolder(FOLDERID_ProgramFiles);
    return base.empty() ? std::wstring() : base + L"\\" + kProductName;
}

const std::wstring& InstallDir() { return g_installDir; }

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Reads back where a previous install put itself, so reinstalling over one
// defaults to the same place instead of silently moving it.
std::wstring RecordedInstallDir() {
    wchar_t value[MAX_PATH] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kArpKey, L"InstallLocation", RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS) {
        return {};
    }
    return value;
}

// ---------------------------------------------------------------- payload

bool WritePayload(int id, const std::wstring& path) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) return false;
    HGLOBAL loaded = LoadResource(nullptr, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(nullptr, found);
    if (!data || !size) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) &&
                    written == size;
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// A file whose process has only just exited can stay locked for a moment
// longer - an antivirus scanner looking at it on close is the usual holder - so
// writing and deleting program files get a short grace period rather than
// failing on the first refusal. Two seconds at most, and only ever spent when a
// file is actually held.
constexpr int kFileAttempts = 10;
constexpr DWORD kFileRetryMs = 200;

// Writes one program file, moving a copy that is still running out of the way
// if waiting does not free it.
//
// A running exe cannot be overwritten, but it can be renamed, and that is what
// makes this reliable when something the installer could not close still holds
// the file. The usual case is a tray app in another user's session, which no
// window search from this session can reach. The old copy keeps running until
// that user signs out, and is deleted at the next restart.
bool InstallFile(int id, const std::wstring& path) {
    for (int attempt = 0; attempt < kFileAttempts; ++attempt) {
        if (attempt) Sleep(kFileRetryMs);
        if (WritePayload(id, path)) return true;
    }
    const std::wstring aside = path + L".old";
    if (!MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return WritePayload(id, path);
}

// Deletes a program file with the same grace period. Whether it went is judged
// afterwards from what is on disk, so this does not need to report back.
void DeletePatiently(const std::wstring& path) {
    for (int attempt = 0; attempt < kFileAttempts; ++attempt) {
        if (attempt) Sleep(kFileRetryMs);
        if (DeleteFileW(path.c_str())) return;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
    }
}

// ---------------------------------------------------------------- service

// Stops the service and waits until its process has exited. Returns true if it
// is stopped or was never there - the caller's real question is always "is it
// safe to overwrite the binary now", not "did I stop anything".
//
// The process is waited for as well as the status, because the two are not the
// same moment. A service reports STOPPED as the last thing it does and only
// then returns and exits, so for a short while the SCM says stopped while the
// exe is still mapped, and writing or deleting it fails. rebuild-service.ps1
// has always slept half a second for exactly this; here the wait is on the
// thing that actually holds the file, so it lasts as long as needed and no
// longer.
bool StopService(DWORD timeoutMs = 30000) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, kServiceName,
                                     SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        const bool absent = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
        CloseServiceHandle(scm);
        return absent;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    auto query = [&] {
        return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                    reinterpret_cast<BYTE*>(&status),
                                    sizeof(status), &needed) != FALSE;
    };
    query();
    // Opened before the stop, so the id cannot be handed to some unrelated
    // process between the service exiting and this looking for it.
    HANDLE process = status.dwProcessId
                         ? OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId)
                         : nullptr;
    if (status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS ignored = {};
        ControlService(service, SERVICE_CONTROL_STOP, &ignored);
        for (DWORD waited = 0; waited < timeoutMs; waited += 250) {
            if (!query()) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(250);
        }
    }
    const bool stopped = status.dwCurrentState == SERVICE_STOPPED;
    if (process) {
        if (stopped) WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return stopped;
}

// Deletes the registration directly rather than asking CecService.exe to do it.
//
// Removal has to work when the exe is already gone - a half-finished earlier
// attempt, a file the user deleted by hand - and shelling out to a missing
// binary would fail silently and leave a registered service pointing at
// nothing. Installing still goes through the exe, because that is where the
// failure actions and defaults are defined; removal needs none of that.
bool RemoveServiceRegistration() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, kServiceName, DELETE);
    if (!service) {
        const bool absent = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
        CloseServiceHandle(scm);
        return absent;
    }
    const bool ok = DeleteService(service) != FALSE ||
                    GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ok;
}

bool ServiceExists() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    const bool exists = service != nullptr;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return exists;
}

// Runs CecService.exe with a subcommand and waits. The service registers
// itself rather than having that logic duplicated here: its failure actions,
// description and default settings all live in one place, and this stays
// correct when they change.
bool RunServiceCommand(const std::wstring& exe, const wchar_t* verb) {
    std::wstring command = L"\"" + exe + L"\" " + verb;
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

// ---------------------------------------------------------------- tray app

// Asks the tray app to quit, then makes sure of it.
//
// Found as an ordinary top-level window. It used to be message-only and was
// searched for under HWND_MESSAGE, and when the tray became a hidden top-level
// window - so that it hears broadcasts such as TaskbarCreated - that search
// quietly stopped finding it. The installer then tried to overwrite a running
// exe and failed, and the uninstaller said the app was removed while its file
// was still there. FindWindow does see hidden windows, but only in this
// session; a tray in another user's session is InstallFile's problem.
void StopTrayApp() {
    for (int attempt = 0; attempt < 40; ++attempt) {
        HWND window = FindWindowW(kTrayWindowClass, nullptr);
        if (!window) return;

        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (!pid) {
            Sleep(250);
            continue;
        }

        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (!process) {
            Sleep(250);
            continue;
        }
        const DWORD waited = WaitForSingleObject(process, 5000);
        if (waited != WAIT_OBJECT_0) {
            // It holds the file being replaced, so leaving it running is not
            // an option. Nothing it owns is unsaved - settings are written as
            // they change - so this costs nothing.
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
        }
        CloseHandle(process);
        return;
    }
}

// A primary token for the signed-in user, borrowed from the shell, or null.
// The caller closes it.
//
// This installer runs elevated, so its own identity is the administrator's,
// and anything it does on the user's behalf - starting the tray app, finding
// the user's own Start menu - would otherwise happen in the wrong profile.
// Borrowing the shell's token is the documented way back down to the person
// actually at the desk.
HANDLE ShellUserToken() {
    HWND shell = GetShellWindow();
    if (!shell) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    if (!pid) return nullptr;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return nullptr;
    HANDLE token = nullptr;
    HANDLE primary = nullptr;
    if (OpenProcessToken(process, TOKEN_DUPLICATE, &token)) {
        if (!DuplicateTokenEx(token, TOKEN_ALL_ACCESS, nullptr,
                              SecurityImpersonation, TokenPrimary, &primary)) {
            primary = nullptr;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return primary;
}

// Starts the tray app as the signed-in user rather than as the elevated
// account running this installer.
//
// This is not a nicety. The tray writes its autostart entry and its settings
// to HKCU, and launched from here as the administrator all of that would land
// in the wrong profile, and the person actually using the machine would get
// none of it.
bool LaunchTrayAsUser(const std::wstring& exe) {
    HANDLE primary = ShellUserToken();
    if (!primary) return false;
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION info = {};
    const bool started =
        CreateProcessWithTokenW(primary, 0, nullptr, command.data(), 0, nullptr,
                                nullptr, &startup, &info) != FALSE;
    if (started) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
    CloseHandle(primary);
    return started;
}

// ---------------------------------------------------------------- shortcuts

bool WriteShortcut(const std::wstring& link, const std::wstring& target,
                   const std::wstring& description) {
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                reinterpret_cast<void**>(&shell)))) {
        return false;
    }
    shell->SetPath(target.c_str());
    shell->SetDescription(description.c_str());
    shell->SetIconLocation(target.c_str(), 0);

    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile,
                                        reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
        file->Release();
    }
    shell->Release();
    return ok;
}

// ---------------------------------------------------------------- location

bool PickFolder(HWND owner, std::wstring& chosen) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose where to install");

    bool picked = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                chosen = path;
                CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

std::wstring StartMenuLink() {
    const std::wstring programs = KnownFolder(FOLDERID_CommonPrograms);
    return programs.empty() ? std::wstring() : programs + L"\\" + kShortcutName;
}

std::wstring DesktopLink() {
    const std::wstring desktop = KnownFolder(FOLDERID_PublicDesktop);
    return desktop.empty() ? std::wstring() : desktop + L"\\" + kShortcutName;
}

void RemoveShortcuts() {
    const std::wstring start = StartMenuLink();
    const std::wstring desktop = DesktopLink();
    if (!start.empty()) DeleteFileW(start.c_str());
    if (!desktop.empty()) DeleteFileW(desktop.c_str());
}

// The tray app makes a Start menu shortcut of its own, in the signed-in user's
// profile, whenever it finds no shared one - which is the case for any copy run
// from somewhere other than an install, a build folder for instance. The
// shared-folder sweep above cannot see it, and the administrator's own Start
// menu is the wrong place to look, so the folder is asked for with the user's
// token. Users not signed in are out of reach, and their stale shortcut would
// only point at a file that is gone.
void RemoveUserShortcut() {
    HANDLE token = ShellUserToken();
    if (!token) return;
    PWSTR programs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, token, &programs))) {
        DeleteFileW((std::wstring(programs) + L"\\" + kShortcutName).c_str());
        CoTaskMemFree(programs);
    }
    CloseHandle(token);
}

// ---------------------------------------------------------------- registry

void WriteArpEntry(const std::wstring& dir) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kArpKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const std::wstring uninstall = L"\"" + dir + L"\\" + kUninstallExe + L"\"";
    text(L"DisplayName", kProductName);
    text(L"DisplayVersion", kVersion);
    text(L"Publisher", kPublisher);
    text(L"InstallLocation", dir);
    text(L"UninstallString", uninstall);
    text(L"DisplayIcon", dir + L"\\" + kTrayExe + L",0");
    // Installed Apps offers this to anyone who never opens the app itself.
    // URLInfoAbout belongs beside it and is deliberately absent until there is a
    // project page to point it at; a tip jar is not an "about" link.
    text(L"HelpLink", L"mailto:business.pdm@proton.me");

    // Modify is meaningless here - there is nothing to configure that the app
    // does not configure itself - but Repair is real, so it is left offered.
    const DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

// Removes a value from every loaded user hive.
//
// The installer runs elevated, so its own HKCU is the administrator's, not the
// hive of the person who actually uses the app. Sweeping HKEY_USERS is what
// reaches the right one. Hives for users not currently signed in are not
// loaded and cannot be reached this way - a leftover autostart entry there is
// harmless, since it names an exe that no longer exists.
void ForEachUserHive(void (*action)(HKEY subtree)) {
    for (DWORD index = 0;; ++index) {
        wchar_t name[256] = {};
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(HKEY_USERS, index, name, &length, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        // The _Classes subtrees mirror an already-visited hive.
        const std::wstring key = name;
        if (key == L".DEFAULT" || key.find(L"_Classes") != std::wstring::npos) {
            continue;
        }
        HKEY hive = nullptr;
        if (RegOpenKeyExW(HKEY_USERS, key.c_str(), 0, KEY_ALL_ACCESS, &hive) ==
            ERROR_SUCCESS) {
            action(hive);
            RegCloseKey(hive);
        }
    }
}

void ClearUserTraces(HKEY hive) {
    HKEY run = nullptr;
    if (RegOpenKeyExW(hive, kRunKey, 0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, kRunValue);
        RegCloseKey(run);
    }
    RegDeleteTreeW(hive, kTrayUserKey);
}

// ---------------------------------------------------------------- removal

bool DeleteDirectory(const std::wstring& dir) {
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteDirectory(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

// ---------------------------------------------------------------- worker

// The window is told what happened through these rather than the work touching
// controls directly, so every long step runs off the UI thread and the window
// never stops repainting.
constexpr UINT kMsgStatus = WM_APP + 1;   // wParam = percent, lParam = wchar_t*
constexpr UINT kMsgDone = WM_APP + 2;     // wParam = 1 on success

void Report(HWND window, int percent, const wchar_t* text) {
    PostMessageW(window, kMsgStatus, static_cast<WPARAM>(percent),
                 reinterpret_cast<LPARAM>(text));
}

struct InstallRequest {
    HWND window;
    bool desktopShortcut;
};

DWORD WINAPI InstallWorker(LPVOID parameter) {
    const InstallRequest request = *static_cast<InstallRequest*>(parameter);
    delete static_cast<InstallRequest*>(parameter);
    HWND window = request.window;

    const std::wstring dir = InstallDir();
    if (dir.empty()) {
        Report(window, 0, L"Could not find Program Files.");
        PostMessageW(window, kMsgDone, 0, 0);
        return 1;
    }

    // Order matters: nothing may hold a file that is about to be replaced.
    Report(window, 10, L"Closing any running copy...");
    StopTrayApp();
    StopService();

    Report(window, 30, L"Copying files...");
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Report(window, 0, L"Could not create the install folder.");
        PostMessageW(window, kMsgDone, 0, 0);
        return 1;
    }
    if (!InstallFile(IDR_SERVICE_EXE, dir + L"\\" + kServiceExe) ||
        !InstallFile(IDR_TRAY_EXE, dir + L"\\" + kTrayExe)) {
        Report(window, 0, L"Could not write the program files.");
        PostMessageW(window, kMsgDone, 0, 0);
        return 1;
    }
    // Not checked, and that is a decision rather than an omission. The only
    // way this fails is an Uninstall.exe from a previous install being open at
    // the time, and then the copy already there is a working uninstaller for
    // the same product - older, but it removes everything Remove() names.
    // Failing the install over that would be the worse answer.
    CopyFileW(OwnPath().c_str(), (dir + L"\\" + kUninstallExe).c_str(), FALSE);

    Report(window, 55, L"Registering the Windows service...");
    if (!RunServiceCommand(dir + L"\\" + kServiceExe, L"install")) {
        Report(window, 0, L"The service was installed but would not start.");
        PostMessageW(window, kMsgDone, 0, 0);
        return 1;
    }

    Report(window, 80, L"Creating shortcuts...");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring start = StartMenuLink();
    if (!start.empty()) WriteShortcut(start, dir + L"\\" + kTrayExe, kProductName);
    const std::wstring desktop = DesktopLink();
    if (!desktop.empty()) {
        if (request.desktopShortcut) {
            WriteShortcut(desktop, dir + L"\\" + kTrayExe, kProductName);
        } else {
            DeleteFileW(desktop.c_str());
        }
    }
    CoUninitialize();

    WriteArpEntry(dir);

    Report(window, 95, L"Starting...");
    const bool running = LaunchTrayAsUser(dir + L"\\" + kTrayExe);

    Report(window, 100, L"Installed.");
    // The distinction reaches the finish text: claiming the tray icon is
    // running when it is not would send the user hunting for something that
    // is not there.
    PostMessageW(window, kMsgDone, running ? 1 : 2, 0);
    return 0;
}

struct RemoveRequest {
    HWND window;
    Scope scope;
};

DWORD WINAPI RemoveWorker(LPVOID parameter) {
    const RemoveRequest request = *static_cast<RemoveRequest*>(parameter);
    delete static_cast<RemoveRequest*>(parameter);
    HWND window = request.window;
    const std::wstring dir = InstallDir();

    if (request.scope != Scope::App) {
        Report(window, 20, L"Removing the Windows service...");
        // Stop first and wait. Deleting a service that is still stopping leaves
        // it marked for deletion until a reboot, and a reinstall cannot proceed
        // while it is in that state.
        StopService();
        RemoveServiceRegistration();
        DeletePatiently(dir + L"\\" + kServiceExe);
        if (request.scope == Scope::Everything) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, kServiceSettingsKey);
        }
    }

    if (request.scope != Scope::Service) {
        Report(window, 60, L"Removing the app...");
        StopTrayApp();
        DeletePatiently(dir + L"\\" + kTrayExe);
        RemoveShortcuts();
        RemoveUserShortcut();
        ForEachUserHive(ClearUserTraces);
    }

    // Anything left behind would be a folder with nothing in it and an entry
    // in Installed Apps pointing at a product that is no longer there, so
    // partial removals finish the job once they turn out to have been total.
    const bool serviceLeft = FileExists(dir + L"\\" + kServiceExe);
    const bool trayLeft = FileExists(dir + L"\\" + kTrayExe);
    const bool everythingGone =
        request.scope == Scope::Everything || (!serviceLeft && !trayLeft);

    if (everythingGone) {
        Report(window, 85, L"Cleaning up...");
        const std::wstring logs = KnownFolder(FOLDERID_ProgramData);
        if (!logs.empty()) DeleteDirectory(logs + L"\\CecService");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kServiceSettingsKey);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kArpKey);

        // The install folder still holds this very executable, so it cannot
        // delete itself here. A copy in the temp folder waits for this process
        // to exit and then removes the lot.
        wchar_t temp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, temp)) {
            const std::wstring helper = std::wstring(temp) + L"CecCleanup.exe";
            if (CopyFileW(OwnPath().c_str(), helper.c_str(), FALSE)) {
                std::wstring command = L"\"" + helper + L"\" /finish " +
                                       std::to_wstring(GetCurrentProcessId()) +
                                       L" \"" + dir + L"\"";
                STARTUPINFOW startup = {sizeof(startup)};
                PROCESS_INFORMATION info = {};
                if (CreateProcessW(nullptr, command.data(), nullptr, nullptr,
                                   FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &startup, &info)) {
                    CloseHandle(info.hThread);
                    CloseHandle(info.hProcess);
                }
            }
        }
    }

    // Says what is left rather than just that something happened, because the
    // whole point of the separate buttons is that a partial removal leaves the
    // machine in a state worth naming.
    Report(window, 100,
           everythingGone
               ? L"Everything has been removed."
               : request.scope == Scope::Service
                     ? L"The Windows service has been removed. The app is "
                       L"still installed."
                     : L"The app has been removed. The Windows service is "
                       L"still running, so the TV will still turn on and off "
                       L"with the PC.");
    PostMessageW(window, kMsgDone, 1, 0);
    return 0;
}

DWORD WINAPI RepairWorker(LPVOID parameter) {
    // Just the window: a repair puts everything back, so there is no scope to
    // choose, and passing one implied a choice that was never read.
    HWND window = *static_cast<HWND*>(parameter);
    delete static_cast<HWND*>(parameter);
    const std::wstring dir = InstallDir();

    Report(window, 15, L"Closing any running copy...");
    StopTrayApp();
    StopService();

    Report(window, 35, L"Restoring files...");
    CreateDirectoryW(dir.c_str(), nullptr);
    // Checked now, where it used to be ignored: a repair that could not put
    // the files back has not repaired anything, and must not say it has.
    const bool files = InstallFile(IDR_SERVICE_EXE, dir + L"\\" + kServiceExe) &&
                       InstallFile(IDR_TRAY_EXE, dir + L"\\" + kTrayExe);
    CopyFileW(OwnPath().c_str(), (dir + L"\\" + kUninstallExe).c_str(), FALSE);

    Report(window, 60, L"Re-registering the service...");
    const bool ok = RunServiceCommand(dir + L"\\" + kServiceExe, L"install");

    Report(window, 85, L"Restoring shortcuts...");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring start = StartMenuLink();
    if (!start.empty()) WriteShortcut(start, dir + L"\\" + kTrayExe, kProductName);
    CoUninitialize();
    WriteArpEntry(dir);
    LaunchTrayAsUser(dir + L"\\" + kTrayExe);

    Report(window, 100,
           !files ? L"Some files could not be restored because they are in use. "
                    L"Restart the PC and run Repair again."
           : ok   ? L"Repaired. The files are back and the service is "
                    L"running again."
                  : L"The files were restored, but the Windows service would "
                    L"not start. Try restarting the PC.");
    PostMessageW(window, kMsgDone, (files && ok) ? 1 : 0, 0);
    return 0;
}

// ---------------------------------------------------------------- window

// Control ids shared by both faces; only one face exists at a time.
enum ControlId : int {
    kIdTitle = 1000,
    kIdBlurb,
    kIdLocation,
    kIdChange,
    kIdDesktop,
    kIdProgress,
    kIdStatus,
    kIdPrimary,
    kIdCancel,
    kIdRepair,
    kIdRemoveService,
    kIdRemoveApp,
    kIdRemoveAll,
};

HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
UINT g_dpi = 96;
bool g_uninstallMode = false;
bool g_busy = false;
bool g_finished = false;
bool g_trayRunning = false;

int Scaled(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }

HWND Add(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
         int x, int y, int width, int height, int id, HFONT font) {
    HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, Scaled(x), Scaled(y),
        Scaled(width), Scaled(height), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (control) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
    }
    return control;
}

void SetStatus(HWND window, const wchar_t* text) {
    SetWindowTextW(GetDlgItem(window, kIdStatus), text);
}

bool Confirm(HWND window, const wchar_t* question) {
    return MessageBoxW(window, question, kProductName,
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

// Everything the user can click, so work can disable exactly these and leave
// the labels alone - a disabled STATIC greys its text, which made the status
// line look broken at the moment it mattered most.
constexpr int kInteractive[] = {kIdChange,    kIdDesktop,       kIdPrimary,
                                kIdCancel,    kIdRepair,        kIdRemoveService,
                                kIdRemoveApp, kIdRemoveAll};

void EnableInteractive(HWND window, bool enabled) {
    for (int id : kInteractive) {
        if (HWND control = GetDlgItem(window, id)) {
            EnableWindow(control, enabled ? TRUE : FALSE);
        }
    }
}

void BeginWork(HWND window, LPTHREAD_START_ROUTINE worker, LPVOID parameter) {
    g_busy = true;
    EnableInteractive(window, false);
    ShowWindow(GetDlgItem(window, kIdProgress), SW_SHOW);
    HANDLE thread = CreateThread(nullptr, 0, worker, parameter, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        g_busy = false;
        SetStatus(window, L"Could not start. Try again.");
    }
}

void ShowLocation(HWND window) {
    // SS_PATHELLIPSIS shortens in the middle, keeping the drive and the folder
    // name legible, which is what someone checking the path actually looks at.
    SetWindowTextW(GetDlgItem(window, kIdLocation), InstallDir().c_str());
}

void BuildInstaller(HWND window) {
    Add(window, L"STATIC", kProductName, 0, 24, 20, 452, 30, kIdTitle,
        g_titleFont);
    Add(window, L"STATIC",
        L"Installs a background service that turns the TV on and off with the "
        L"PC, and a tray icon for volume and other controls.",
        0, 24, 56, 452, 40, kIdBlurb, nullptr);

    Add(window, L"STATIC", L"Install to:", 0, 24, 106, 70, 20, 0, nullptr);
    Add(window, L"STATIC", L"", SS_PATHELLIPSIS, 96, 106, 274, 20, kIdLocation,
        nullptr);
    Add(window, L"BUTTON", L"Change...", WS_TABSTOP, 386, 102, 90, 26,
        kIdChange, nullptr);
    ShowLocation(window);

    HWND desktop = Add(window, L"BUTTON", L"Create a desktop shortcut",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 24, 140, 452, 24,
                       kIdDesktop, nullptr);
    SendMessageW(desktop, BM_SETCHECK, BST_CHECKED, 0);

    HWND progress = Add(window, PROGRESS_CLASSW, L"", 0, 24, 178, 452, 10,
                        kIdProgress, nullptr);
    ShowWindow(progress, SW_HIDE);
    // Three lines tall: the finish message explains where the icon is and what
    // each mouse button does, and that will not fit on one.
    Add(window, L"STATIC", L"", 0, 24, 194, 452, 56, kIdStatus, nullptr);
    Add(window, L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 286, 258,
        90, 30, kIdPrimary, nullptr);
    Add(window, L"BUTTON", L"Cancel", WS_TABSTOP, 386, 258, 90, 30, kIdCancel,
        nullptr);
}

void BuildUninstaller(HWND window) {
    Add(window, L"STATIC", kProductName, 0, 24, 20, 380, 30, kIdTitle,
        g_titleFont);
    Add(window, L"STATIC", L"Choose what to remove.", 0, 24, 56, 380, 20,
        kIdBlurb, nullptr);

    Add(window, L"BUTTON", L"Repair (put the files and the service back)",
        WS_TABSTOP, 24, 88, 380, 32, kIdRepair, nullptr);
    Add(window, L"BUTTON", L"Remove the service (keep the app)", WS_TABSTOP,
        24, 128, 380, 32, kIdRemoveService, nullptr);
    Add(window, L"BUTTON", L"Remove the app (keep the service)", WS_TABSTOP, 24,
        168, 380, 32, kIdRemoveApp, nullptr);
    Add(window, L"BUTTON", L"Remove everything", WS_TABSTOP, 24, 208, 380, 32,
        kIdRemoveAll, nullptr);

    HWND progress = Add(window, PROGRESS_CLASSW, L"", 0, 24, 256, 380, 10,
                        kIdProgress, nullptr);
    ShowWindow(progress, SW_HIDE);
    Add(window, L"STATIC", L"", 0, 24, 272, 380, 40, kIdStatus, nullptr);
    Add(window, L"BUTTON", L"Close", WS_TABSTOP, 314, 320, 90, 30, kIdCancel,
        nullptr);
}

void OnCommand(HWND window, int id) {
    if (g_busy) return;

    if (id == kIdCancel) {
        DestroyWindow(window);
        return;
    }

    if (id == kIdPrimary) {
        if (g_finished) {
            DestroyWindow(window);
            return;
        }
        auto* request = new InstallRequest{
            window,
            SendMessageW(GetDlgItem(window, kIdDesktop), BM_GETCHECK, 0, 0) ==
                BST_CHECKED};
        BeginWork(window, InstallWorker, request);
        return;
    }

    if (id == kIdChange) {
        std::wstring chosen;
        if (!PickFolder(window, chosen) || chosen.empty()) return;
        // Install into a folder of our own inside whatever was chosen, unless
        // the chosen folder is already one. Writing loose files into a folder
        // picked for other reasons would make the uninstall look reckless, and
        // picking a drive root would be worse still.
        if (_wcsicmp(LeafOf(chosen).c_str(), kProductName) != 0) {
            if (chosen.back() == L'\\') chosen.pop_back();
            chosen += L"\\";
            chosen += kProductName;
        }
        g_installDir = chosen;
        ShowLocation(window);
        return;
    }

    if (id == kIdRepair) {
        BeginWork(window, RepairWorker, new HWND(window));
        return;
    }

    struct { int id; Scope scope; const wchar_t* question; } const choices[] = {
        {kIdRemoveService, Scope::Service,
         L"Remove the Windows service?\n\nThe TV will no longer turn on and off "
         L"with the PC, and the tray icon controls will stop working. The app "
         L"itself stays installed."},
        {kIdRemoveApp, Scope::App,
         L"Remove the app?\n\nThe TV will still turn on and off with the "
         L"PC. The tray icon and its volume controls will go."},
        {kIdRemoveAll, Scope::Everything,
         L"Remove everything?\n\nThe service, the app, the shortcuts and "
         L"the settings will be deleted."},
    };
    for (const auto& choice : choices) {
        if (choice.id != id) continue;
        if (!Confirm(window, choice.question)) return;
        BeginWork(window, RemoveWorker, new RemoveRequest{window, choice.scope});
        return;
    }
}

void OnDone(HWND window, bool success) {
    g_busy = false;
    g_finished = true;
    ShowWindow(GetDlgItem(window, kIdProgress), SW_HIDE);
    // Close comes back whatever happened. It was disabled to stop the window
    // being shut halfway through, and leaving it that way would trap the user
    // in a dialog with no working button.
    EnableWindow(GetDlgItem(window, kIdCancel), TRUE);

    if (g_uninstallMode) {
        // A partial removal is a normal place to stop, but it is also a normal
        // place to carry on from - so the remaining choices come back unless
        // there is nothing left to act on.
        const std::wstring dir = InstallDir();
        const bool anythingLeft =
            ServiceExists() || FileExists(dir + L"\\" + kTrayExe);
        if (anythingLeft) {
            EnableInteractive(window, true);
        }
        SetFocus(GetDlgItem(window, kIdCancel));
        return;
    }

    if (success) {
        // Described in words rather than drawn.
        //
        // This said "Left click \u23FB", the IEC power symbol, on the reasoning
        // that showing the icon beats naming it. It rendered as an empty
        // rectangle: Segoe UI has no glyph at that codepoint and nothing in the
        // fallback chain supplied one either. A character that is not in the
        // font the text is drawn in cannot be relied on to appear, however
        // standard the codepoint is, and a box in an instruction is worse than
        // no picture at all.
        SetStatus(window,
                  g_trayRunning
                      ? L"Installed successfully. The tray icon app is running, "
                        L"it may be in your hidden icons. Left click the blue "
                        L"power icon for volume controls, and right click it "
                        L"for additional controls and settings."
                      : L"Installed. Open HDMI-CEC Display Control from the "
                        L"Start menu to show the tray icon.");
        SetWindowTextW(GetDlgItem(window, kIdPrimary), L"Close");
        EnableWindow(GetDlgItem(window, kIdPrimary), TRUE);
        ShowWindow(GetDlgItem(window, kIdCancel), SW_HIDE);
    } else {
        // Left enabled on purpose: the usual cause is something holding a file,
        // and running it again once is the fix.
        SetWindowTextW(GetDlgItem(window, kIdPrimary), L"Try again");
        g_finished = false;
        EnableWindow(GetDlgItem(window, kIdPrimary), TRUE);
        EnableWindow(GetDlgItem(window, kIdDesktop), TRUE);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            if (g_uninstallMode) {
                BuildUninstaller(window);
            } else {
                BuildInstaller(window);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_ERASEBKGND: {
            RECT client = {};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_COMMAND:
            OnCommand(window, LOWORD(wParam));
            return 0;
        case kMsgStatus:
            SendMessageW(GetDlgItem(window, kIdProgress), PBM_SETPOS,
                         wParam, 0);
            SetStatus(window, reinterpret_cast<const wchar_t*>(lParam));
            return 0;
        case kMsgDone:
            // 1 means installed and running, 2 installed but not launched.
            g_trayRunning = wParam == 1;
            OnDone(window, wParam != 0);
            return 0;
        case WM_CLOSE:
            // Closing mid-way would leave a half-written install behind.
            if (g_busy) return 0;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Waits for the uninstaller to exit, then deletes the folder it was running
// from. Runs from the temp folder as its own copy, with no UI.
int FinishRemoval(int argc, wchar_t** argv) {
    if (argc < 4) return 1;
    const DWORD pid = static_cast<DWORD>(_wtoi(argv[2]));
    const std::wstring dir = argv[3];

    // Generous, because the wait is for a person: the uninstaller stays on
    // screen showing "Done." until they close it, and only then does its folder
    // become deletable. Capped so this can never become a process waiting for
    // ever on a window nobody is going to touch.
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(process, 10 * 60 * 1000);
        CloseHandle(process);
    }

    // The process handle can close a moment before Windows releases the image.
    bool removed = false;
    for (int attempt = 0; attempt < 40 && !removed; ++attempt) {
        removed = DeleteDirectory(dir);
        if (!removed) Sleep(250);
    }
    if (!removed) {
        // Something outside this installer is holding a file - a virus scanner
        // mid-scan being the usual one. Leaving it to the next reboot beats
        // leaving it for ever.
        MoveFileExW((dir + L"\\" + kUninstallExe).c_str(), nullptr,
                    MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    // Nothing can delete a running executable, so this one goes at reboot.
    MoveFileExW(OwnPath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && std::wstring(argv[1]) == L"/finish") {
        const int result = FinishRemoval(argc, argv);
        LocalFree(argv);
        return result;
    }

    // Which face to show is decided by the filename, so the copy left in the
    // install folder is an uninstaller without needing a switch on the command
    // line that a user could stumble into.
    const std::wstring leaf = LeafOf(OwnPath());
    g_uninstallMode = _wcsicmp(leaf.c_str(), kUninstallExe) == 0 ||
                      (argv && argc >= 2 &&
                       std::wstring(argv[1]) == L"/uninstall");
    if (argv) LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    // The folder picker is a COM object owned by this thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // The uninstaller works from where it is, not from where a fresh install
    // would go, so it stays correct when the location was changed at install
    // time. A reinstall defaults to the recorded location for the same reason.
    if (g_uninstallMode) {
        g_installDir = ParentOf(OwnPath());
    } else {
        const std::wstring recorded = RecordedInstallDir();
        g_installDir = recorded.empty() ? DefaultInstallDir() : recorded;
    }

    if (g_uninstallMode && !ServiceExists() &&
        !FileExists(InstallDir() + L"\\" + kTrayExe)) {
        MessageBoxW(nullptr, L"Nothing to remove. It is not installed.",
                    kProductName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"CecSetupWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETUP));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);

    g_dpi = GetDpiForSystem();
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                         FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         VARIABLE_PITCH, L"Segoe UI");
    g_titleFont = CreateFontW(-MulDiv(15, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    const int width = Scaled(g_uninstallMode ? 428 : 500);
    const int height = Scaled(g_uninstallMode ? 366 : 306);
    RECT frame = {0, 0, width, height};
    AdjustWindowRectEx(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int frameWidth = frame.right - frame.left;
    const int frameHeight = frame.bottom - frame.top;

    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, kProductName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, (screenWidth - frameWidth) / 2,
        (screenHeight - frameHeight) / 2, frameWidth, frameHeight, nullptr,
        nullptr, instance, nullptr);
    if (!window) return 1;

    SendMessageW(GetDlgItem(window, kIdProgress), PBM_SETRANGE32, 0, 100);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    CoUninitialize();
    return 0;
}
