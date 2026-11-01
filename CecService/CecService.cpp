// CecService - Windows Service that drives HDMI-CEC on sleep and wake.
//
// Runs as SYSTEM in Session 0, which is the only way to be running before user
// login. Session 0 has no desktop, so this process deliberately has no UI at
// all; the tray app, CecTray, talks to it over a named pipe.
//
// Confirmed 2026-09-04 that NVAPI's AUX calls work from Session 0, so the CEC
// work happens directly here rather than being delegated to a user-session
// helper.

#include <windows.h>
#include <sddl.h>
#include <dbt.h>
#include <powrprof.h>

#pragma comment(lib, "powrprof.lib")

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../CecTest/CecDevice.h"
#include "../CecTest/Edid.h"
#include "../CecTest/PipeProtocol.h"
#include "../CecTest/ShutdownKind.h"
#include "../CecTest/Version.h"
#include "PipeServer.h"

namespace {

const wchar_t* kServiceName = L"CecService";
const wchar_t* kSettingsKey = L"SOFTWARE\\CecService";
const wchar_t* kDisplayName = L"HDMI-CEC Display Power Control";
const wchar_t* kDescription =
    L"Sends HDMI-CEC standby and power-on commands to a TV connected through a "
    L"DisplayPort adapter, so the display follows the PC's sleep and wake.";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stopEvent = nullptr;

// ---------------------------------------------------------------- settings

// Stored under HKLM so the service owns them. The tray app will not write here
// directly - it asks the service over the named pipe, which keeps the pipe from
// becoming a way for a normal user to change privileged state.
struct Settings {
    bool loggingEnabled = true;
    // A full power off should take the TV with it.
    bool standbyOnShutdown = true;
    // A restart never does, and there is no setting for it. There was, off by
    // default, and it had no use: this service turns the television on again
    // as soon as it starts, so all it ever did was blank the set for the
    // minute of the reboot - hiding POST and the BIOS, which is when someone
    // restarting to troubleshoot most wants to see the screen. Removed at the
    // user's request on 2026-10-02.
    //
    // Sleep and wake take the TV with them. Off leaves the set alone on both.
    // A separate setting from the screen one below since 2026-10-02, at the
    // user's request: on a machine with ordinary sleep the two are different
    // events, and turning the screen one off used to leave sleep following
    // regardless, which nothing on the menu said. See SleepFollowed for the
    // Modern Standby case, where they are one event.
    bool standbyOnSleep = true;
    // Windows blanking the display - a power plan timeout, a screen saver, the
    // lock screen - should take the TV with it, the same way sleep does. On by
    // default: on a set attached to a PC, the alternative is a television left
    // showing black for hours.
    bool standbyOnDisplayOff = true;
    // Remote keys the TV forwards to this PC become Windows media keys. On by
    // default: a PC wired to a television is one people sit back from, and this
    // is the difference between a monitor and an appliance.
    bool forwardRemoteKeys = true;
    // Where the tray's volume buttons go. Off means the television, over CEC,
    // which is what a TV driving its own speakers or a soundbar should honour.
    // On means Windows' own volume instead, for the sets that acknowledge a
    // volume command and do nothing with it - measured on an LG OLED, which
    // ACKs <User Control Pressed> and will not even answer <Give Audio Status>.
    // The loudness in the room changes either way; only who is turned down
    // differs.
    bool volumeControlsPc = false;
    // Diagnostic only. Asks the TV what state it is in at the moment of resume,
    // before <Image View On> is sent, which is the only way to tell "standby
    // never took effect" from "standby worked and something turned it back on".
    // Off by default because the query costs up to a second of wake latency.
    bool diagnosePower = false;
    // Diagnostic only. Logs every CEC frame this device receives, which is the
    // only way to see whether a TV asks for the input name at all, and when.
    // Off by default: on a busy bus it is a line per frame.
    bool logCecTraffic = false;
    // How long to hold the suspend open after <Standby> so the TV can finish
    // its 1.5-2.2s transition before the DisplayPort link drops. Tunable
    // without a rebuild because the right value is hardware-dependent.
    DWORD suspendSettleMs = 2500;
};

Settings g_settings;

// What this machine's Windows calls a restart and a power off in event 1074.
// Empty on an English PC and on one that has not shut down since this was
// installed; English is matched regardless. See LearnShutdownWords.
cec::ShutdownWords g_shutdownWords;

// When this process started, as a FILETIME. An event 1074 older than this
// belongs to a previous shutdown, which is the one thing that tells a stale
// answer from a real one. See QueryShutdown.
unsigned long long g_startedAt = 0;

bool ReadFlag(const wchar_t* name, bool fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value != 0;
}

// The CEC address last used, remembered so that a restart asks for the same one
// rather than taking whichever looks free. See AllocateLogicalAddress.
DWORD ReadNumber(const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return value;
}

// The television this port last read as, so that only the set's own answer can
// rename it. See CecDevice::DiscoverPhysicalAddress.
std::string ReadText(const wchar_t* name) {
    wchar_t value[128] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, name, RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::string ascii;
    for (const wchar_t* at = value; *at; ++at) {
        if (*at >= 0x20 && *at <= 0x7E) ascii.push_back(static_cast<char>(*at));
    }
    return ascii;
}

// The same pair for a value that is not ASCII, which the two above quietly
// mangle: ReadText drops anything outside printable ASCII, and WriteText widens
// byte by byte. That is right for what they carry, which is an EDID identity
// key built out of hex digits, and wrong for a word in somebody's own language.
std::wstring ReadWideText(const wchar_t* name) {
    // Roomy on purpose. This carries the learned shutdown words, several to a
    // value, and RegGetValueW does not truncate: a buffer one character short
    // fails the whole read and every word learned so far goes with it. Eight
    // words of a language that likes long ones is still a fraction of this.
    wchar_t value[512] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, name, RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS) {
        return {};
    }
    return value;
}

// The learned words, kept in one value each with a newline between them. A
// newline cannot appear in the field this reads from, and the alternative -
// a numbered value per word - is a worse thing to look at in regedit.
std::vector<std::wstring> SplitWords(const std::wstring& text) {
    std::vector<std::wstring> words;
    size_t at = 0;
    while (at <= text.size() && !text.empty()) {
        const size_t end = text.find(L'\n', at);
        const std::wstring one =
            text.substr(at, end == std::wstring::npos ? end : end - at);
        if (!one.empty()) words.push_back(one);
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    return words;
}

// Enough for four languages each having two words for the same thing, which is
// what German turned out to have. The cap exists only so that a machine which
// somehow produces a fresh word every time cannot grow this without end; a word
// pushed out costs one shutdown to learn again, and that only if somebody goes
// back to a language they have not used in four changes.
constexpr size_t kMaxLearnedWords = 8;

std::wstring JoinWords(const std::vector<std::wstring>& words) {
    std::wstring text;
    for (const std::wstring& one : words) {
        if (!text.empty()) text += L'\n';
        text += one;
    }
    return text;
}

void DeleteValue(const wchar_t* name) {
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSettingsKey, name);
}

void WriteWideText(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void WriteText(const wchar_t* name, const std::string& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    std::wstring wide(value.begin(), value.end());
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(wide.c_str()),
                   static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void WriteNumber(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

bool WriteFlag(const wchar_t* name, bool value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD data = value ? 1 : 0;
    const LONG result =
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void LoadSettings() {
    g_settings.loggingEnabled = ReadFlag(L"LoggingEnabled", true);
    g_settings.standbyOnShutdown = ReadFlag(L"StandbyOnShutdown", true);
    g_settings.standbyOnSleep = ReadFlag(L"StandbyOnSleep", true);
    g_settings.standbyOnDisplayOff = ReadFlag(L"StandbyOnDisplayOff", true);
    g_settings.forwardRemoteKeys = ReadFlag(L"ForwardRemoteKeys", true);
    g_settings.volumeControlsPc = ReadFlag(L"VolumeControlsPc", false);
    g_settings.diagnosePower = ReadFlag(L"DiagnosePower", false);
    g_settings.logCecTraffic = ReadFlag(L"LogCecTraffic", false);

    // Read here with everything else, so that a word learned by one start is in
    // hand at the preshutdown of the next, which re-reads all of this.
    g_shutdownWords.restart = SplitWords(ReadWideText(L"ShutdownWordRestart"));
    g_shutdownWords.powerOff = SplitWords(ReadWideText(L"ShutdownWordPowerOff"));

    DWORD settle = 0;
    DWORD size = sizeof(settle);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"SuspendSettleMs",
                     RRF_RT_REG_DWORD, nullptr, &settle, &size) == ERROR_SUCCESS &&
        settle <= 10000) {
        g_settings.suspendSettleMs = settle;
    }
}

// ---------------------------------------------------------------- logging

std::wstring LogPath() {
    wchar_t programData[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring dir = (length && length < MAX_PATH)
                           ? std::wstring(programData) + L"\\CecService"
                           : std::wstring(L"C:\\CecService");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\service.log";
}

// The log is the only diagnostic a Session 0 service has, so it stays on in
// release builds - but bounded. At roughly 150 bytes per sleep/wake cycle a
// 1 MB cap is several years of normal use, and at most two files ever exist.
constexpr LONGLONG kMaxLogBytes = 1024 * 1024;

void RotateIfLarge(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    const LONGLONG size =
        (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size < kMaxLogBytes) return;
    MoveFileExW(path.c_str(), (path + L".old").c_str(),
                MOVEFILE_REPLACE_EXISTING);
}

std::mutex g_logMutex;

void Log(const char* format, ...) {
    if (!g_settings.loggingEnabled) return;
    std::lock_guard<std::mutex> guard(g_logMutex);
    static const std::wstring path = LogPath();

    // Checking the size on every line meant an extra file-system call per line
    // for a file that grows about 150 bytes per power event. Every 64 lines is
    // still nowhere near the 1 MB cap.
    static unsigned sinceCheck = 0;
    if (sinceCheck++ % 64 == 0) RotateIfLarge(path);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[1200];
    const int n = snprintf(line, sizeof(line),
                           "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", now.wYear,
                           now.wMonth, now.wDay, now.wHour, now.wMinute,
                           now.wSecond, message);
    if (n <= 0) return;

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

// --------------------------------------------------------- CEC frame log

// Names for the opcodes worth recognising on sight. Anything else is printed
// as its number, which is enough to look up and rare enough not to matter.
const char* OpcodeName(uint8_t opcode) {
    switch (opcode) {
        case 0x00: return "Feature Abort";
        case 0x04: return "Image View On";
        case 0x0D: return "Text View On";
        case 0x32: return "Set Menu Language";
        case 0x36: return "Standby";
        case 0x44: return "User Control Pressed";
        case 0x45: return "User Control Released";
        case 0x46: return "Give OSD Name";
        case 0x47: return "Set OSD Name";
        case 0x64: return "Set OSD String";
        case 0x70: return "System Audio Mode Request";
        case 0x71: return "Give Audio Status";
        case 0x72: return "Set System Audio Mode";
        case 0x7A: return "Report Audio Status";
        case 0x7E: return "System Audio Mode Status";
        case 0x80: return "Routing Change";
        case 0x81: return "Routing Information";
        case 0x82: return "Active Source";
        case 0x83: return "Give Physical Address";
        case 0x84: return "Report Physical Address";
        case 0x85: return "Request Active Source";
        case 0x86: return "Set Stream Path";
        case 0x87: return "Device Vendor ID";
        case 0x8C: return "Give Device Vendor ID";
        case 0x8D: return "Menu Request";
        case 0x8E: return "Menu Status";
        case 0x8F: return "Give Device Power Status";
        case 0x90: return "Report Power Status";
        case 0x91: return "Get Menu Language";
        case 0x9D: return "Inactive Source";
        case 0x9E: return "CEC Version";
        case 0x9F: return "Get CEC Version";
        case 0xA0: return "Vendor Command With ID";
        default:   return "";
    }
}

// The operand bytes of a frame, as hex, for the two lines below that print
// them. Truncates rather than overruns: the tail of a long frame is not worth a
// bounds check at every call site.
void FormatOperands(const std::vector<uint8_t>& frame, char* out, size_t size) {
    int at = 0;
    out[0] = '\0';
    for (size_t i = 2; i < frame.size() && at < static_cast<int>(size) - 4; ++i) {
        at += snprintf(out + at, size - at, "%02X ", frame[i]);
    }
}

// One line per frame received, when LogCecTraffic is on. This exists to answer
// a question the code cannot answer by reasoning: whether a given TV ever asks
// <Give OSD Name>, and if so how long after the announce - which decides
// whether an input label can be fixed by replying or only by sending it at a
// better moment.
void LogCecFrame(const std::vector<uint8_t>& frame, bool answered) {
    // One line, whatever the traffic setting says. <Menu Request> is the
    // handshake that decides whether a TV forwards its remote at all, so
    // whether it ever arrives is the first question worth asking when the
    // remote does nothing - and turning frame logging on to find out means
    // reproducing the problem twice.
    if (frame.size() >= 2 && frame[1] == 0x8D) {
        static bool seen = false;
        if (!seen) {
            seen = true;
            Log("TV asked <Menu Request>; answered activated, so it should "
                "forward its remote from here");
        }
    }

    if (!g_settings.logCecTraffic || frame.empty()) return;

    const unsigned src = frame[0] >> 4;
    const unsigned dst = frame[0] & 0x0F;
    if (frame.size() < 2) {
        Log("[cec] %X->%X  poll", src, dst);
        return;
    }

    // The operands, so a name or a status value can be read straight out of the
    // line rather than inferred from the opcode.
    char bytes[64];
    FormatOperands(frame, bytes, sizeof(bytes));

    const char* name = OpcodeName(frame[1]);
    Log("[cec] %X->%X  %02X %-26s %s%s", src, dst, frame[1], name, bytes,
        answered ? " (answered)" : "");
}

// One line per frame sent, when LogCecTraffic is on, with what the adapter
// made of it. The counterpart of LogCecFrame, and added for the same reason it
// was: a failure that names no frame cannot be reasoned about.
void LogCecSend(const std::vector<uint8_t>& frame, const cec::TxResult& tx) {
    if (!g_settings.logCecTraffic || frame.empty()) return;
    const unsigned src = frame[0] >> 4;
    const unsigned dst = frame[0] & 0x0F;
    char bytes[64];
    FormatOperands(frame, bytes, sizeof(bytes));
    const char* verdict = tx.nacked      ? "NOT ACKNOWLEDGED"
                          : tx.lineError ? "line error"
                          : tx.sent      ? "sent"
                                         : "no transmit status";
    if (frame.size() < 2) {
        Log("[cec] tx %X->%X  poll  %s", src, dst, verdict);
        return;
    }
    Log("[cec] tx %X->%X  %02X %-26s %s [%s]", src, dst, frame[1],
        OpcodeName(frame[1]), bytes, verdict);
}

// This PC's CEC address, for marking it in the routing lines below.
std::atomic<uint16_t> g_ownAddress{0};

// Which HDMI port is on screen, as of the last frame anybody noticed, or -1
// when nothing has said yet.
//
// Written from CecDevice's screen path sink, which fires on whichever thread
// drained the bus - the pump when it is running, and the command path when it
// is not. Read by the read-only pipe without any lock at all.
//
// It exists because the snapshot this used to come from is rebuilt only by the
// pump, and the pump is precisely what stands aside while something is
// happening: it yields for kPumpYieldMs after any traffic and skips its tick
// while a command holds the CEC lock, and a switch is seconds of both. So the
// one answer the menu is asked for most, during the one exchange that changes
// it, was the only answer not being kept up to date. Giving the menu its own
// pipe fetched that stale copy faster and changed nothing anybody could see.
std::atomic<int32_t> g_onScreenPath{-1};

// When the tray last reported real keyboard or mouse input, and zero when it
// has reported none since the last <Standby> this service sent. The tray is
// the only part of this that can see input at all: a service in Session 0 has
// no desktop to watch.
std::atomic<DWORD> g_userActiveTick{0};

// When the tray last reported the screen coming on. Its presence is the whole
// point: it says a tray is there to answer the question of who woke it.
std::atomic<DWORD> g_trayScreenOnTick{0};

// Defined with the display state further down, and needed here because a
// routing frame is where this service learns it no longer owns the TV.
bool DisplayOwnsStandby();
void SetDisplayOwnsStandby(bool owned);

// One line per routing frame, always, whatever LogCecTraffic says. Diagnostics
// for two devices on one TV: which input the TV and the other devices say is on
// screen, and whether a TV announces an input changed with its own remote at
// all, which everything planned for that case depends on. Rare frames, so the
// log does not grow by much.
void LogRoutingFrame(const std::vector<uint8_t>& frame, bool) {
    if (frame.size() < 2) return;
    const unsigned src = frame[0] >> 4;
    const unsigned dst = frame[0] & 0x0F;
    char paths[48] = {};
    int at = 0;
    for (size_t i = 2; i + 1 < frame.size() && at < 36; i += 2) {
        const unsigned path = (frame[i] << 8) | frame[i + 1];
        at += snprintf(paths + at, sizeof(paths) - at, " %u.%u.%u.%u%s",
                       path >> 12, (path >> 8) & 0xF, (path >> 4) & 0xF,
                       path & 0xF, path == g_ownAddress.load() ? " (this PC)" : "");
    }
    Log("[route] %X->%X <%s>%s", src, dst, OpcodeName(frame[1]), paths);

    // Somebody else has taken the screen, so this service no longer owns the
    // television's power state. Without this, a PC that turned the TV off, and
    // then watched another device turn it on and take over, would still turn it
    // back to itself the moment its own screen came on.
    //
    // Only from a frame that names a different input. <Standby> names none and
    // is left alone: the set going off changes who is watching nothing, not who
    // turned it off.
    // Exact sizes, not floors. A torn frame carries a stale opcode in front of
    // somebody else's payload, and the line logged just above is where they
    // were caught: a <Routing Change> naming five addresses where the message
    // holds two, and a <Request Active Source>, which has no operands at all,
    // arriving with one. Believing the wrong half of one of those handed the
    // television's power state to a device that had not taken the screen.
    uint16_t path = 0;
    switch (frame[1]) {
        case 0x82:     // <Active Source> [path]
        case 0x81:     // <Routing Information> [path]
        case 0x86:     // <Set Stream Path> [path]
            if (frame.size() != 4) return;
            path = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
            break;
        case 0x80:     // <Routing Change> [from] [to]
            if (frame.size() != 6) return;
            path = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
            break;
        default:
            return;
    }
    // Deliberately no plausibility test on the path here, where the exact sizes
    // above have already thrown the torn frames out. 0.0.0.0 is not an input and
    // would fail that test, but as an <Active Source> it is a television saying
    // its own tuner has the screen - which is precisely the case this needs to
    // hear, since it means the set is no longer showing this PC.
    if (path != g_ownAddress.load() && DisplayOwnsStandby()) {
        SetDisplayOwnsStandby(false);
        Log("another device took the screen; this service no longer owns the TV");
        return;
    }
    // The television itself routing to this PC means it is on, whoever turned
    // it on - most often a person with its remote, after this service had left
    // it off. Hard rule 18: left owned, the next sleep or shutdown would read
    // "already off" and leave the set on all night. Caught in the test of the
    // start after an update: the TV turned on by its remote at the lock screen
    // went on being counted as off. Only the set's own word, since a frame from
    // another device naming this PC is a request, not a fact about the TV.
    if (path == g_ownAddress.load() && (frame[0] >> 4) == 0 &&
        DisplayOwnsStandby()) {
        SetDisplayOwnsStandby(false);
        Log("the TV is on and showing this PC; this service no longer owns it");
    }
}

// Whether this PC sleeps the modern way, where it never really goes down.
//
// It matters because such a machine sends no suspend notification at all when
// it sleeps: closing the lid arrives as the screen going off and nothing else.
// Verified on a Latitude 5430, where the TV followed the lid perfectly, but
// only because the screen-off setting was on. With that setting off, sleep
// would leave the television lit. Nothing here changes behaviour; it is
// reported so the tray can say what the setting actually covers.
bool ModernStandby() {
    static const bool aoac = [] {
        SYSTEM_POWER_CAPABILITIES caps = {};
        if (CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps,
                                   sizeof(caps)) != 0) {
            return false;
        }
        return caps.AoAc != FALSE;
    }();
    return aoac;
}

// Whether the TV should follow this PC to sleep and back.
//
// On a Modern Standby machine sleeping is the screen going off - no suspend
// notification arrives for it - so the screen setting is the one that decides,
// and the tray shows that machine a single item saying so. Asking the sleep
// setting there would let a hidden value overrule the one on the menu for the
// rarer real suspends such a machine still has, such as hibernating.
bool SleepFollowed() {
    return ModernStandby() ? g_settings.standbyOnDisplayOff
                           : g_settings.standbyOnSleep;
}

// ------------------------------------------------------------ device name

// The PC's name as shown in Settings > System > About, which is what the TV
// will display for this input. Read once and cached: Windows requires a reboot
// to apply a rename, so it cannot change while the service is running.
// How far back to look for the shutdown this machine last did. A day, because
// the point is to have learned it, not to have learned it recently: the service
// may be restarted by hand hours after a boot, and the words do not change.
constexpr unsigned int kLearnShutdownMs = 24 * 60 * 60 * 1000;

// Teaches this machine its own words for a restart and a power off.
//
// Event 1074 is the only record readable at the moment the decision has to be
// made, and its wording is the local Windows' own. Kernel-Power event 109
// carries the same fact as a number that no translation touches, and it is
// written about six seconds after this service has already decided and stopped,
// a second after the Event Log service itself has gone, so it cannot even be
// asked for by then. It can be read on the way back up, though.
//
// What it cannot do on the way back up is find its own partner. Going looking
// for the newest word and the newest number and marrying them was tried, and a
// laptop log took it apart: three "Shut down" clicks each wrote a 1074 and then
// hibernated rather than shutting down, so no number ever followed them, while
// two restarts wrote no word at all. The newest of each were then from
// different cycles two and a half minutes apart, and "Ausschalten" was learned
// as a restart - a wrong lesson, stored, and believed for good.
//
// So nothing is looked for. The preshutdown path keeps the word it read while
// this service was itself shutting down, which is the one shutdown it can be
// certain about, and this only asks what became of it. One shutdown teaches one
// word; a machine that has done both ends up knowing both. Nothing is written
// or said when the answer is the one already held, which on an English PC is
// every time.
void LearnShutdownWords() {
    const std::wstring word = ReadWideText(L"ShutdownPendingWord");
    const std::wstring when = ReadWideText(L"ShutdownPendingWhen");
    if (word.empty() || when.empty()) return;

    // Cleared whatever comes of it. A word left by a shutdown that then did not
    // happen has nothing to confirm it, and keeping it would only give it
    // another chance to be paired with somebody else's shutdown later.
    DeleteValue(L"ShutdownPendingWord");
    DeleteValue(L"ShutdownPendingWhen");

    cec::ShutdownKind kind = cec::ShutdownKind::Unknown;
    std::string detail;
    if (!cec::ConfirmShutdownWord(when, kLearnShutdownMs, &kind, &detail)) {
        Log("the last shutdown left a word that could not be confirmed (%s)",
            detail.c_str());
        return;
    }

    std::vector<std::wstring>& known = kind == cec::ShutdownKind::Restart
                                           ? g_shutdownWords.restart
                                           : g_shutdownWords.powerOff;
    // Added rather than replaced. One machine has more than one word for the
    // same thing: the Start menu writes "Ausschalten" on a German PC and
    // shutdown.exe writes "Herunterfahren", and a single slot would have them
    // taking turns while half of that machine's shutdowns went unrecognised.
    if (std::find(known.begin(), known.end(), word) != known.end()) return;
    known.push_back(word);
    if (known.size() > kMaxLearnedWords) known.erase(known.begin());
    WriteWideText(kind == cec::ShutdownKind::Restart ? L"ShutdownWordRestart"
                                                     : L"ShutdownWordPowerOff",
                  JoinWords(known));
    // As UTF-8, not as the printable ASCII everything else in this log is.
    //
    // The whole feature exists for a Windows that is not in English, so a line
    // that goes blank on exactly those machines would be a diagnostic that
    // reports nothing at the only time it is worth reading. The log is written
    // as raw bytes, so these pass straight through and any reader that assumes
    // UTF-8, which Notepad now does, shows the word as it was written.
    std::string readable;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, word.c_str(),
                                          static_cast<int>(word.size()), nullptr,
                                          0, nullptr, nullptr);
    if (bytes > 0) {
        readable.resize(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, word.c_str(),
                            static_cast<int>(word.size()), readable.data(), bytes,
                            nullptr, nullptr);
    }
    Log("this Windows writes '%s' in the log for a %s, which is now recognised",
        readable.c_str(), cec::ShutdownKindName(kind));
}

const std::string& PcName() {
    static const std::string name = [] {
        wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(buffer, &size)) return std::string();
        std::string ascii;
        for (DWORD i = 0; i < size; ++i) {
            if (buffer[i] >= 0x20 && buffer[i] <= 0x7E) {
                ascii += static_cast<char>(buffer[i]);
            }
        }
        return ascii;
    }();
    return name;
}

// ------------------------------------------------------------ CEC session

// Owns the connection to the adapter. Deliberately re-acquires everything on
// demand: the DisplayPort link drops across sleep, taking the CEC tunneling
// enable bit and the logical-address claim with it, and at cold boot this
// service starts before the GPU driver is ready to answer at all.
// How quiet the CEC link has to go before the topology registration is
// refreshed. Anything shorter and the refresh starts landing inside a run of
// keypresses, which is exactly the stall this replaced.
constexpr DWORD kIdleBeforeAnnounceMs = 15000;

// Hard ceiling on registration age, for traffic so continuous it never looks
// idle. Long enough that ordinary use never reaches it.
constexpr DWORD kMaxAnnounceAgeMs = 300000;

// A hold this old did not end cleanly. It has to stay clear of the tray's own
// ceiling of 35s, or a legitimate long press would be declared abandoned while
// the user was still holding the button, and the refresh this guards would land
// in the middle of it.
constexpr DWORD kAbandonedHoldMs = 45000;

// How recently the link must have been used for background upkeep to stand
// aside. Comfortably longer than one press-and-release round trip (~170ms),
// so a run of clicks reads as continuous rather than as gaps between presses.
constexpr DWORD kBusyLinkMs = 1500;

// How often the receive buffer is checked for questions from the TV.
//
// Two rates, because the cost and the benefit are not evenly spread. A TV
// interrogates a device in bursts - at boot, on wake, when it switches to this
// input - and says nothing at all for hours in between. So any received frame,
// or any topology event, drops the interval to kPumpBusyMs for kPumpHotMs, and
// it returns to kPumpIdleMs once things go quiet.
//
// The idle rate is what runs almost all the time, and it is deliberately inside
// the CEC specification's one second maximum response time: a device that
// answers late is, to the TV, a device that did not answer. Two reads a second
// of a single DPCD register is not a meaningful cost on a desktop, and it is
// the price of never being the device that failed to reply.
constexpr DWORD kPumpIdleMs = 500;
constexpr DWORD kPumpBusyMs = 150;
constexpr DWORD kPumpHotMs = 90000;

// A third rate, for when the TV is forwarding its remote to this PC.
//
// The other two are sized for a device that must answer questions within the
// specification's one second. A remote is a different problem: 150ms between
// polls is a button that feels sticky, and the first press of a run always
// waits the full idle interval because it is the thing that reveals a remote is
// in use at all. 60ms is under the threshold where a keypress reads as
// immediate, and it lasts only as long as someone is actually pressing.
constexpr DWORD kPumpRemoteMs = 60;
constexpr DWORD kRemoteHotMs = 12000;

// How recently the link must have been used for the pump to leave it alone,
// and how many questions one tick may answer. Both exist to keep housekeeping
// off the keypress path: whoever answers holds the CEC lock while doing it.
constexpr DWORD kPumpYieldMs = 400;
constexpr int kPumpFramesPerTick = 3;

// Keep-awake.
//
// The tray renews its request every two minutes, and the lease outlasts one
// missed renewal, so a tray that has exited stops the keep-alives within five
// minutes without anything having to notice. The keep-alive itself goes every
// five minutes: well inside the shortest auto power off a TV offers, for two
// frames each time. It waits for the link to have been quiet for a few seconds,
// so it never lands in the middle of someone's clicks.
constexpr DWORD kKeepAwakeLeaseMs = 5 * 60 * 1000;
constexpr DWORD kKeepAliveIntervalMs = 5 * 60 * 1000;
constexpr DWORD kKeepAliveQuietMs = 3000;

// How often opening the volume panel is allowed to re-poll for a soundbar.
// The tray repeats its warm-up while the pointer rests on the icon, and one
// poll per hover is plenty to catch a cable being moved.
constexpr DWORD kAudioProbeIntervalMs = 5000;

// How long a volume question counts as outstanding before it is given up as
// unanswered. The TCL answers in a little over 100ms, so a set that reports
// never gets near this; it only runs out for one that stays silent, and the
// tray stops asking one of those after a few tries. Nothing blocks for this
// long any more - see AskVolume - it only decides when silence is reported.
constexpr DWORD kAudioStatusTimeoutMs = 500;

// How often the pump looks for the answer while a volume question is out. Each
// look is a register read over AUX, not a frame on the CEC bus, so it costs the
// buttons nothing; it only decides how soon an answer that has landed reaches
// the panel.
constexpr DWORD kVolumeAnswerPollMs = 20;

// How recently something must have announced for a power-down path to reuse it
// rather than announce again. This one is for the handover between the
// display-off path and a suspend that took over from it, which is seconds.
constexpr DWORD kFreshAnnounceMs = 15000;

// How long this service's own <Standby> is trusted without asking the TV. A set
// can go on saying "on" for several seconds while it powers down - the TCL has
// taken nearly five to confirm - and on the TCL a second <Standby> in that time
// is the toggle hard rule 1 forbids. The screen-off path waits up to ten
// seconds for the set to confirm, so this matches it. See TvNeedsStandby.
constexpr DWORD kOwnStandbyTrustMs = 10000;

// How long a sleep or shutdown waits for the display-off path to finish
// deciding, when the two begin together. That path stands aside for a
// power-down at the latest once its announce is done, which is about two
// seconds; this is the cap for one that has hung. See LockForPowerDown.
constexpr DWORD kPowerDownLockWaitMs = 5000;

// How long a sleep or shutdown waits for the TV to say whether it is on. The
// TCL answers in about a tenth of a second; this only runs out on a set that
// never answers, and then the decision is the one made before it asked.
constexpr int kPowerCheckMs = 800;

// The same idea for the dim-then-blank sequence, which is far longer: measured
// at fifteen seconds between the two on this machine, exactly on the boundary
// above, and there is no reason to think other configurations are not longer
// still. Safe to be generous because the display-off path verifies and retries
// behind a full announce, so a registration that has in fact gone stale costs
// one ignored frame rather than a television left on. Well inside the 300s this
// service already treats a registration as good for.
constexpr DWORD kDimAnnounceValidMs = 120000;

// How recent an announce must be for the tray's screen-off warning to skip its
// own. The warning comes about ten seconds ahead, well inside the window above,
// so anything this fresh will still count when the screen goes.
constexpr DWORD kPrepareFreshMs = 60000;

// How long a television is given to finish changing input before it is asked
// anything about input again. Measured on the bench: a second click arriving
// inside this window is simply lost, which looks exactly like a refusal.
// How many ignored requests make it worth saying so in the menu.
//
// One. This was two, on the reasoning that a single failure might be a device
// that was briefly busy, and that was wrong twice over.
//
// It was wrong about the evidence. One failure here is not one frame: it is two
// <Set Stream Path> broadcasts spaced by the settle delay, a <Request Active
// Source>, and then a query of the whole bus, over eight to thirteen seconds,
// and it only reaches this line when the device acknowledged a poll during that
// same exchange. A device that is awake, listening, asked twice, and silent for
// thirteen seconds has told us what it is going to do.
//
// It was wrong about the cost of being careful. The menu row is the only notice
// that survives Do Not Disturb, which is on by default for many people, so a
// threshold of two meant somebody's first attempt produced nothing visible
// anywhere: no picture change, no notification, and an unchanged menu. Waiting
// for a second attempt is only cautious if somebody makes one, and the whole
// point is to tell them before they try again.
//
// What makes one safe is that it is not a verdict. Any <Active Source> from the
// device clears it, whether the claim came from here, from another PC, or from
// its own remote, so a device that really was busy un-marks itself the moment it
// next takes the screen. See NoteDeviceFrame.
constexpr uint8_t kSwitchIgnoredSure = 1;

// How long to wait before trying an address move again, and how many times.
//
// A move that could not be attempted - the bus too busy to carry a single probe
// - is worth repeating, but only on a leash. Each attempt drops this device's
// logical address to probe with, puts four frames on the wire, and holds the
// CEC lock for a quarter of a second or more; repeating that every pump tick
// would be a busy loop against the one resource every keypress needs. Three
// seconds apart and five attempts covers a television finishing an announce,
// which is what was actually seen, without ever becoming a spin.
//
// After the last one it stops and waits for a fresh clash. That is not giving
// up on the problem: a clash is detected from frames arriving, so if the other
// device is still there it will be noticed again within seconds.
// How often one known device is asked to confirm which HDMI port it is on.
// A minute, because the question is cheap only when it is answered. See
// GreetPeersNow.
constexpr DWORD kPortRefreshMs = 60000;

constexpr DWORD kAddressRetryGapMs = 3000;
constexpr int kAddressRetryLimit = 5;

constexpr DWORD kInputSettleMs = 3000;

// How long to listen for the device told to take the screen to say that it has.
//
// It says so with <Active Source>, which it broadcasts of its own accord, so
// this is a wait rather than an exchange and it ends the moment the frame
// arrives - on the bench, two to four hundred milliseconds. The ceiling is for
// a device that never answers, and it is generous because the television
// re-syncs the HDMI link on the way, which takes it a second or two.
//
// This was briefly raised to 5000 on the second attempt, to accommodate an
// Apple TV that appeared to claim the screen at 2.5 to 4 seconds. That was a
// misreading and the change is reverted: the <Active Source> frames it was
// measured from were the user pressing the Apple TV's own remote, which is
// indistinguishable here from the device answering a request. There is no
// evidence in any log that that Apple TV responds to <Set Stream Path> at all,
// and a longer wait would buy nothing while costing every click on a genuinely
// empty input two and a half seconds. See item 13 for the test that would
// settle it.
constexpr DWORD kClaimWaitMs = 2500;

// How quiet a television has to be about routing before it is asked to switch,
// and how long to wait for that quiet.
//
// Asking a set that is already changing input is what made a switch need a
// second click. Twice measured: the request landed while the set was completing
// a switch another machine had asked for, the named device claimed the screen
// exactly as it should, and the set carried on to where it had been going. The
// click that worked landed on an idle bus. Waiting first is the whole fix, and
// it costs nothing in the ordinary case because an idle bus is already quiet -
// nor on a set that never announces routing, which has nothing to wait for.
constexpr DWORD kTvRouteQuietMs = 1200;
constexpr DWORD kTvRouteWaitMs = 5000;

// How long this PC stands out of the way after asking for another input.
//
// A television passes a switch request to the bus, and the device named claims
// the screen. Both of those are broadcasts, so every other device sees them -
// including a second copy of this app, whose job on being named is to claim the
// screen. That is correct behaviour and must stay. But while this PC is in the
// middle of handing the screen away, a request naming this PC is the fallout of
// its own handover rather than somebody asking for it, and honouring it undoes
// the switch the person actually chose. Two machines doing that to each other
// oscillated on a four-second cycle for over a minute in one log.
//
// Long enough to outlast a television's own repeats of the request, which one
// log shows spread over about five seconds.
constexpr DWORD kHandoverQuietMs = 6000;

// And how long it lasts once the handover is done rather than in flight.
//
// The window above has to cover the whole exchange, because that is when the
// echo arrives. Once the input asked for has actually claimed the screen, the
// only thing still to come is the television's trailing repeat of a request it
// has already had answered - so the guard drops to this, and a person who picks
// this PC's input on the TV's own remote a moment later is obeyed rather than
// ignored. It is cleared outright when the switch failed, since then nothing
// was handed anywhere and this PC should answer for itself at once.
constexpr DWORD kHandoverTailMs = 1500;

// How often the devices on the bus are checked for still being there, and how
// long one that has stopped answering is kept in the list. A poll is one byte,
// so the check is cheap; the day is because a device switched off overnight is
// exactly the one somebody wants to switch to in the morning, and switching to
// a sleeping device is how it gets woken.
constexpr DWORD kDeviceCheckMs = 10000;
constexpr DWORD kForgetDeviceMs = 24 * 60 * 60 * 1000;

// How long this PC keeps asking where it is after a display arrives, and how
// often inside that.
//
// Once was not enough. Windows' copy of the EDID is not always up to date the
// instant Windows says a display has arrived, and on Intel that copy is the
// only source there is - the sink refuses the I2C write a DDC read begins with,
// so there is nothing else to ask. A log caught the laptop moved from the
// television's port 2 to port 3, read once on the arrival, told 2.0.0.0 again,
// and left announcing an input with nothing on it.
//
// Twelve seconds was not enough either: a log has the laptop moved from port 2
// to port 3, asked four times across twelve seconds, and told 2.0.0.0 every
// time. So the window is a minute, and the reads are cheap enough for that -
// Windows' copy comes out of the registry, not off the wire.
//
// It only runs that long when Windows' copy is what answered. A sink that
// answers for itself is answering about this port at the moment it is asked, so
// one read settles it and the window closes; that is every NVIDIA and AMD
// machine, where an arrival costs a single read.
constexpr DWORD kAddressRecheckMs = 60000;
constexpr DWORD kAddressRecheckEveryMs = 5000;

// The same window and spacing for finding the port again after a display
// arrives while the service has none. See AttachIfDisplayReturned.
// How long another device must keep saying it is on a port held by a PC
// running this app that has stopped answering, before the port moves to it.
// See ClaimPort.
constexpr DWORD kPortContestMs = 5000;

// How long a PC heard from for the first time has to answer the hello this PC
// sends it in return, before it counts as another copy of this app. Its pump
// reads frames about once a second and stands aside during traffic, so this is
// generous; one that misses it is simply asked again at its next hello. See
// NotePeer.
constexpr DWORD kPeerConfirmMs = 5000;

constexpr DWORD kAttachAfterArrivalMs = 60000;
constexpr DWORD kAttachAfterArrivalEveryMs = 5000;

// How far a standby verify may run past its own limit before its answer is
// disbelieved. See WaitWasHeldUp.
constexpr DWORD kStandbyWaitOverrunMs = 5000;

// How recently the tray must have reported a screen coming on for this service
// to leave the decision to it. Comfortably longer than the two notifications
// take to arrive, and short enough that a tray closed since is not waited for.
constexpr DWORD kTrayReportFreshMs = 5000;

// How long to wait for that report before concluding there is no tray running.
// Generously more than a pipe round trip, and short enough not to be noticed
// when the answer is that nobody is there.
constexpr DWORD kTrayReportWaitMs = 1500;

// When to keep checking often. Set by anything that makes the TV likely to
// start asking questions: a wake, an attach, a fresh announce, or simply having
// just heard from it. Compared with a signed difference so the tick counter
// wrapping after 49 days cannot leave the pump stuck at one rate.
std::atomic<DWORD> g_hotUntilTick{0};
std::atomic<DWORD> g_remoteUntilTick{0};
std::atomic<bool> g_pumpPaused{false};

// Until when the tray has asked for the TV to be kept awake. Zero is off; a
// time in the past is a lease that ran out because the tray stopped renewing.
std::atomic<DWORD> g_keepAwakeUntilTick{0};

bool KeepAwakeRequested() {
    const DWORD until = g_keepAwakeUntilTick.load();
    return until && static_cast<LONG>(until - GetTickCount()) > 0;
}

// Which keep-alive signal to send. A hidden setting, KeepAwakeSignal, so that
// each can be tried on a TV without a rebuild; anything out of range means the
// default. Read each time rather than cached, since it is only asked every few
// minutes and a change should apply to the very next one.
cec::CecDevice::KeepAlive KeepAwakeSignal() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"KeepAwakeSignal",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS ||
        value > 3) {
        value = 0;
    }
    return static_cast<cec::CecDevice::KeepAlive>(value);
}

const char* KeepAliveName(cec::CecDevice::KeepAlive signal) {
    switch (signal) {
        case cec::CecDevice::KeepAlive::kF5Key:         return "F5 key";
        case cec::CecDevice::KeepAlive::kUnassignedKey: return "unassigned key 0x77";
        case cec::CecDevice::KeepAlive::kImageViewOn:   return "<Image View On>";
        case cec::CecDevice::KeepAlive::kActiveSource:  return "<Active Source>";
    }
    return "?";
}

void GoHot() { g_hotUntilTick = GetTickCount() + kPumpHotMs; }
void GoRemoteHot() { g_remoteUntilTick = GetTickCount() + kRemoteHotMs; }

bool PumpIsHot() {
    return static_cast<LONG>(g_hotUntilTick.load() - GetTickCount()) > 0;
}

bool RemoteIsActive() {
    return static_cast<LONG>(g_remoteUntilTick.load() - GetTickCount()) > 0;
}

// ---------------------------------------------------------- per-TV settings
//
// One PC can meet more than one television - a desk and a living room, or a
// set replaced by a new one - and some answers belong to the set rather than
// to the PC. Where the volume buttons go is the clear case: the LG acknowledges
// CEC volume and ignores it, so it wants this PC's own volume, while the TCL
// honours it. One global switch meant flipping it by hand at every move. So the
// settings that describe a television are kept per television, keyed by what
// its EDID says it is. Settings that describe the PC, such as turning the TV
// off at shutdown, stay global.
//
// The key is the three fields EDID reserves for identity - manufacturer,
// product code and serial - and deliberately not the physical address, which
// names the HDMI port rather than the set: moving the cable to another input
// must not lose anything.
//
// Glitches are kept out at the edges. The identity is only taken from an EDID
// whose header and checksum are intact, so a torn read cannot invent a set. An
// entry's name is always the same for the same set, and opening an entry that
// exists merely opens it, so a thousand wakes make one entry rather than a
// thousand. Nothing is written unless something changed.
//
// All registry work happens in Sync(), which the pump and the pipe call without
// the CEC lock. The session only reports what it saw, in memory, so neither an
// attach nor a wake ever waits on the registry. Lock order is always the CEC
// lock first and then this one, and nothing here calls back into the session.

const wchar_t* kDisplaysKey = L"SOFTWARE\\CecService\\Displays";

// Far more televisions than one PC will ever plausibly meet, and still small:
// an entry is four values, so a full store is around 20KB. Only counted when a
// new set appears, which is also the only time anything is removed.
constexpr size_t kMaxDisplays = 100;

// Whole days since 1601. Last-seen exists only to choose which set to forget
// when the store is full, and a day is fine enough for that - while writing it
// at most once a day means a PC that sleeps and wakes all day long does not
// rewrite the registry every time.
DWORD Today() {
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    return static_cast<DWORD>(ticks.QuadPart / 864000000000ULL);
}

// Entries this service wrote look exactly like "TCL-2E5A-00000001". Anything
// else under the key came from somewhere else, so it is neither counted nor
// ever deleted. A shorter name stops at its terminator, which fails every test
// below, so this never reads past the end.
bool IsEntryName(const wchar_t* name) {
    for (size_t i = 0; i < 17; ++i) {
        const wchar_t c = name[i];
        if (i == 3 || i == 8) {
            if (c != L'-') return false;
        } else if (i < 3) {
            if (c < L'A' || c > L'Z') return false;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))) {
            return false;
        }
    }
    return name[17] == L'\0';
}

// The identity is built from letters and hex digits only, so widening it byte
// by byte is exact, and it can never contain a backslash that would turn one
// registry key into a path.
std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

class TvStore {
public:
    // What the latest EDID read said is on the other end. Memory only, so it
    // is safe under the CEC lock. An empty key - an EDID that could not be read
    // or did not check out - changes nothing, so the last good identity stands
    // and one bad read on a wake cannot make a set look new.
    void Seen(const cec::DisplayIdentity& identity) {
        if (identity.key.empty()) return;
        std::lock_guard<std::mutex> guard(mutex_);
        seenKey_ = identity.key;
        seenName_ = identity.name;
    }

    // Whether the set just answered a volume question, or refused one.
    //
    // Silence is never passed here: a set that did not answer was usually off
    // or still waking, and remembering that as "cannot" would be wrong the next
    // day. With a soundbar attached it is really the soundbar answering, which
    // is still a fact about this setup. Memory only, because this runs on every
    // poll; Sync() writes it, and only when it changed.
    void NoteReportsVolume(bool reports) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!Active()) return;
        const int value = reports ? 1 : 0;
        if (reportsVolume_ == value) return;
        reportsVolume_ = value;
        reportsDirty_ = true;
    }

    // Brings the registry into line with what was seen. Almost every call
    // finds nothing to do, and costs a lock and a couple of comparisons.
    void Sync() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (seenKey_.empty()) return;
        if (seenKey_ != key_) Load();
        if (reportsDirty_) {
            // Cleared whether or not the write lands. The value holds in memory
            // either way, and retrying a failing write on every tick would do
            // nothing but fill the log.
            reportsDirty_ = false;
            Write(L"ReportsVolume", reportsVolume_ == 1 ? 1 : 0);
        }
        const DWORD today = Today();
        if (today != lastSeenDay_) {
            lastSeenDay_ = today;
            Write(L"LastSeen", today);
        }
    }

    // The setting as it applies to the set on the other end. Returns false,
    // leaving value alone, when no set has been recognised yet, and the global
    // value is the answer.
    //
    // Always on since 2026-10-02. It used to be a menu option, "Remember
    // settings for each TV", and the user asked for it to stop being one: it
    // is how a PC carried between sets keeps each one's volume choice, and
    // turning it off bought nothing but a second way to be confused.
    bool VolumeControlsPc(bool* value) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!Active()) return false;
        *value = volumeControlsPc_;
        return true;
    }

    // Returns false when no entry applies, and the caller writes the global
    // value instead. A failed registry write still counts as applied: the
    // choice holds for as long as the service runs, and the log says why it
    // did not outlive a restart.
    bool SetVolumeControlsPc(bool value) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!Active()) return false;
        volumeControlsPc_ = value;
        Write(L"VolumeControlsPc", value ? 1 : 0);
        return true;
    }

    // Appended to STATUS. tv= is there for diagnosis; tvvol=1 lets the tray be
    // patient with a set known to report its volume, from the first question.
    std::string Describe() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (seenKey_.empty()) return {};
        std::string out = " tv=" + seenKey_;
        if (Active() && reportsVolume_ == 1) out += " tvvol=1";
        return out;
    }

private:
    // An entry is in force only while it belongs to the set actually on the
    // other end. In the gap between a new set being
    // seen and the next Sync(), the old entry is not in force, so nothing
    // learned in that gap can be filed under the wrong television.
    bool Active() const {
        return !key_.empty() && key_ == seenKey_;
    }

    // Opens the entry for the set just seen, creating it if the set is new.
    void Load() {
        key_ = seenKey_;
        path_ = std::wstring(kDisplaysKey) + L"\\" + Widen(key_);
        reportsVolume_ = -1;
        reportsDirty_ = false;
        lastSeenDay_ = 0;
        // A new set starts from the global value. That is also what carries an
        // existing choice across the upgrade that introduced this store.
        volumeControlsPc_ = ReadFlag(L"VolumeControlsPc", false);

        HKEY entry = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path_.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &entry) == ERROR_SUCCESS) {
            DWORD value = 0;
            if (ReadEntryDword(entry, L"VolumeControlsPc", &value)) {
                volumeControlsPc_ = value != 0;
            }
            if (ReadEntryDword(entry, L"ReportsVolume", &value)) {
                reportsVolume_ = value ? 1 : 0;
            }
            if (ReadEntryDword(entry, L"LastSeen", &value)) lastSeenDay_ = value;
            StoreName(entry);
            RegCloseKey(entry);
            Log("TV %s '%s': known; volume buttons change %s", key_.c_str(),
                seenName_.c_str(), volumeControlsPc_ ? "this PC" : "the TV");
            return;
        }

        const size_t kept = MakeRoom();
        // Set before the attempt, so a failure is logged once here rather than
        // again by the last-seen write in Sync().
        lastSeenDay_ = Today();
        HKEY created = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path_.c_str(), 0, nullptr, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &created,
                            nullptr) != ERROR_SUCCESS) {
            Log("TV %s '%s': new, but its settings could not be stored; they "
                "hold until the service restarts", key_.c_str(), seenName_.c_str());
            return;
        }
        StoreName(created);
        const DWORD pc = volumeControlsPc_ ? 1 : 0;
        RegSetValueExW(created, L"VolumeControlsPc", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&pc), sizeof(pc));
        RegSetValueExW(created, L"LastSeen", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&lastSeenDay_),
                       sizeof(lastSeenDay_));
        RegCloseKey(created);
        Log("TV %s '%s': new, %zu of %zu stored; volume buttons change %s",
            key_.c_str(), seenName_.c_str(), kept + 1, kMaxDisplays,
            volumeControlsPc_ ? "this PC" : "the TV");
    }

    // Counts this service's entries and, if there is no room for one more,
    // removes the sets seen least recently. Returns how many are left.
    size_t MakeRoom() {
        HKEY displays = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDisplaysKey, 0, KEY_READ,
                          &displays) != ERROR_SUCCESS) {
            return 0;   // no store yet, so nothing to count
        }
        std::vector<std::pair<DWORD, std::wstring>> entries;
        wchar_t name[256];
        for (DWORD index = 0;; ++index) {
            DWORD length = ARRAYSIZE(name);
            const LONG result = RegEnumKeyExW(displays, index, name, &length,
                                              nullptr, nullptr, nullptr, nullptr);
            // Any error but an over-long name ends the walk, so a registry
            // that misbehaves cannot hold this in a loop.
            if (result == ERROR_MORE_DATA) continue;
            if (result != ERROR_SUCCESS) break;
            if (!IsEntryName(name)) continue;
            // A set with no last-seen day sorts as the oldest, which is where
            // an entry damaged by hand belongs.
            DWORD seen = 0;
            DWORD size = sizeof(seen);
            if (RegGetValueW(displays, name, L"LastSeen", RRF_RT_REG_DWORD,
                             nullptr, &seen, &size) != ERROR_SUCCESS) {
                seen = 0;
            }
            entries.emplace_back(seen, name);
        }
        RegCloseKey(displays);
        if (entries.size() < kMaxDisplays) return entries.size();

        std::sort(entries.begin(), entries.end());
        const size_t excess = entries.size() - kMaxDisplays + 1;
        for (size_t i = 0; i < excess; ++i) {
            const std::wstring path =
                std::wstring(kDisplaysKey) + L"\\" + entries[i].second;
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str());
        }
        Log("TV store full; forgot the %zu set(s) seen least recently", excess);
        return entries.size() - excess;
    }

    // The set's own name, so someone looking through the registry can tell the
    // entries apart. Written only when it differs from what is stored, which
    // is also how an entry made before the name could be read gets one, and a
    // read that found no name leaves a stored one alone.
    void StoreName(HKEY entry) {
        if (seenName_.empty()) return;
        const std::wstring name = Widen(seenName_);
        wchar_t stored[64] = {};
        DWORD size = sizeof(stored);
        if (RegGetValueW(entry, nullptr, L"Name", RRF_RT_REG_SZ, nullptr, stored,
                         &size) == ERROR_SUCCESS &&
            name == stored) {
            return;
        }
        RegSetValueExW(entry, L"Name", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(name.c_str()),
                       static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    }

    static bool ReadEntryDword(HKEY entry, const wchar_t* name, DWORD* out) {
        DWORD size = sizeof(*out);
        return RegGetValueW(entry, nullptr, name, RRF_RT_REG_DWORD, nullptr, out,
                            &size) == ERROR_SUCCESS;
    }

    void Write(const wchar_t* name, DWORD value) {
        HKEY entry = nullptr;
        LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path_.c_str(), 0, nullptr,
                                      0, KEY_SET_VALUE, nullptr, &entry, nullptr);
        if (result == ERROR_SUCCESS) {
            result = RegSetValueExW(entry, name, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&value),
                                    sizeof(value));
            RegCloseKey(entry);
        }
        if (result != ERROR_SUCCESS) {
            Log("TV %s: could not store a setting (error %ld); it holds until "
                "the service restarts", key_.c_str(), result);
        }
    }

    std::mutex mutex_;
    std::string seenKey_;        // the set on the other end, per the last good EDID
    std::string seenName_;
    std::string key_;            // the set whose entry is loaded
    std::wstring path_;
    bool volumeControlsPc_ = false;
    int reportsVolume_ = -1;     // -1 until the set has answered or refused
    bool reportsDirty_ = false;
    DWORD lastSeenDay_ = 0;
};

TvStore g_tvStore;

// Set by the service control handler when Windows reports a display arriving,
// and cleared by the pump once it has read the address again. A flag rather
// than the work itself, because that handler runs on an SCM thread and has to
// return: see RecheckAddressIfDisplayChanged.
std::atomic<bool> g_displayArrived{false};

// Defined with the display-follow state further down. Declared here because the
// standby paths consult them in the middle of their own work.
bool DisplayStillOff();

// Whether a <Standby> has actually reached the wire because the display went
// off - not whether one is being attempted. The difference is the whole point:
// see StandbyAlreadySent().
bool DisplayOwnsStandby();

// Whether the machine is on its way to sleep or off. A display-off standby that
// has not yet sent anything stands aside for one of these, because they have
// deadlines and it does not.
bool PowerDownStarted();

// Recorded inside the standby path rather than by its caller, so that it is
// stored while the CEC lock is still held. Assigning the return value after the
// call would leave a window - short, but exactly the width of the bug this
// ordering exists to prevent - in which a suspend could take the lock, read the
// old value, and send a second <Standby>.
void SetDisplayOwnsStandby(bool owned);

// Defined with the remote-key handling further down. Declared here because the
// observer is installed wherever the CEC device is built, which is above it.
void OnUserControl(uint8_t uiCommand, bool pressed);

// Defined with the remote-key event further down, whose pattern it follows.
// Hands a volume answer to the tray the moment one lands.
void PublishVolume(const std::string& reply);

// Something else on the television's CEC bus.
//
// Kept for the "Switch Input" menu, which is a list of what is actually
// plugged into the television rather than four HDMI numbers. Devices announce
// their address when they appear and their name when asked, so most of this
// arrives without anything being polled.
struct Peer {
    uint8_t address = 0;      // its CEC address, which can change
    uint16_t where = 0xFFFF;  // its physical address, so which HDMI port
    uint8_t type = 0xFF;      // playback device, tuner, audio system
    uint32_t vendor = 0;      // whoever made it, when it has said
    std::string name;         // what it calls itself, if it says
    bool ours = false;        // another copy of this app, which can be asked
    bool present = false;     // answered the last time it was looked for
    DWORD heardTick = 0;      // when it last said or answered anything

    // How many times this device has been asked to take the screen, been awake
    // enough to answer a poll, and done nothing about it. See SwitchTo for the
    // Apple TV measurement behind it, and kSwitchIgnoredSure for why it takes
    // more than one. Cleared the moment the device does claim the screen, which
    // is the only thing that can prove the count wrong, so a device that was
    // merely busy repairs its own entry.
    //
    // Deliberately not persisted. A physical address is a cable position, not
    // an identity, so a stored flag would follow the port rather than the
    // device and would be wrong the first time anything was replugged. The cost
    // of forgetting is one wasted click per service start, and the cost of
    // remembering wrongly is a menu entry that lies.
    uint8_t switchIgnored = 0;
    // How many times another copy of this app has been asked for its full name.
    // See AskForNames.
    uint8_t nameAsks = 0;
    // A port this device has claimed that an absent PC running this app still
    // holds, and when it first said so. See ClaimPort.
    uint16_t contestPort = 0xFFFF;
    DWORD contestTick = 0;
    // The port a hello from this address gave before it was known to be
    // another copy of this app, and when this PC asked it directly to confirm.
    // See NotePeer.
    uint16_t helloPort = 0xFFFF;
    DWORD helloTick = 0;
    // Whether a PC running this app has said hello since the last neighbour
    // round, and how many rounds in a row it has answered a poll without one.
    // See StillSaysHello.
    bool helloHeard = false;
    uint8_t silentRounds = 0;
    // The name was learnt before the device turned up on another input, so it
    // may belong to a different device. See ClaimPort.
    bool nameUnsure = false;
};

// Said after a logical address whenever it is the Free Use one, because that is
// the fallback and the thing to correlate against if a television starts
// behaving oddly. Empty for the ordinary three, so the usual line is unchanged.
const char* FreeUseNote(uint8_t address) {
    return address == cec::kLogicalAddressFreeUse
               ? ", the Free Use address, because the playback ones were taken"
               : "";
}

// How to name a device in the log.
//
// Its HDMI port when that is known, because that is what a person can see, and
// its CEC address when it is not. A device that has answered a poll but never
// said where it is has a port of 0xFFFF, which FormatPhysicalAddress renders as
// "15.15.15.15" - so two log lines in a field report read "15.15.15.15 is not
// answering", which names nothing at all.
std::string DescribePeer(const Peer& one) {
    if (one.where != 0xFFFF) return cec::FormatPhysicalAddress(one.where);
    char text[40];
    snprintf(text, sizeof(text), "the device on CEC address %u", one.address);
    return text;
}

// Whether a device on the bus is the thing playing the sound rather than
// something to watch.
//
// Either half is enough on its own. Logical address 5 is the one CEC sets aside
// for an audio system, so a device that answered a poll there is one even if it
// has never said so; and a set-top box that moved to a spare address still
// reports its type in <Report Physical Address>. The soundbar in the field
// report, a Polk Mini AX, satisfied both.
bool IsAudioSystem(const Peer& one) {
    return one.address == cec::kLogicalAddressAudioSystem || one.type == 5;
}

// How many neighbour rounds in a row a PC running this app may answer polls
// without saying hello before it is taken for an ordinary device. A minute, at
// a round every ten seconds.
constexpr uint8_t kSilentRoundsBeforeStranger = 6;

// Whether a device marked as another copy of this app still behaves like one.
// Asked once per neighbour round, of a device that has just answered a poll.
//
// Every copy says hello every ten seconds and answers this PC's own hello as
// well, so a minute of answering polls with neither is not one. Without this
// the mark was for ever, since nothing turned it back: the Apple TV and a PC
// have both held CEC address 11 on the Vizio's bus, and a device that takes
// over the address of a PC asleep there answers that PC's polls - so the menu
// offered the sleeping PC as awake, and the device that had taken its address
// was nowhere. Counted in rounds this PC actually ran rather than in time, so
// a spell in which this PC was too busy to look is not taken for a spell of
// silence from the other one.
bool StillSaysHello(Peer& one) {
    if (one.helloHeard) {
        one.helloHeard = false;
        one.silentRounds = 0;
        return true;
    }
    return ++one.silentRounds < kSilentRoundsBeforeStranger;
}

// What a device that does not say its name is called instead. The type comes
// from <Report Physical Address> and is the only other thing CEC offers.
const char* DeviceTypeName(uint8_t type) {
    switch (type) {
        case 0: return "TV";
        case 1: return "Recorder";
        case 3: return "Tuner";
        case 4: return "Playback device";
        case 5: return "Audio system";
        case 6: return "Switch";
        case 7: return "Video processor";
        default: return "Device";
    }
}

class CecSession {
public:
    // Finds a CEC-capable port and joins the CEC topology. Safe to call
    // repeatedly; it rebuilds state from scratch each time.
    // Always with the EDID read, and there is no option to skip it.
    //
    // There was one, briefly, for a caller that had just read the address and
    // saw no reason to pay for it twice. It cannot work: AcquireDevice below
    // destroys the device and builds a new one on every attach, because the
    // port may have changed, and it seeds the new one with knownAddress_ so
    // that a sink with an unreadable EDID still has something to announce.
    // Skipping the read therefore announces the address the caller was trying
    // to leave - which is exactly what the log showed, "this PC has moved:
    // 3.0.0.0 now" followed by "attached ... as 2.0.0.0", twice in one minute.
    bool Attach() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!AcquireDevice()) return false;
        if (!device_->Attach()) {
            lastError_ = device_->lastError();
            ReleaseDevice();
            return false;
        }
        knownAddress_ = device_->physicalAddress();
        g_ownAddress = knownAddress_;
        RememberAddress();
        mapWanted_ = true;
        attachUntilTick_ = 0;   // whoever attached, the search is over
        // One frame, and every other copy of this app on the bus knows this PC
        // and where it is. Without it they would have to poll and ask, which
        // took seconds and left a PC that had just woken missing from the menu
        // of whoever looked first.
        device_->SendHereIAm(cec::kLogicalAddressBroadcast);
        if (!device_->identity().key.empty()) {
            WriteText(L"LastTv", device_->identity().key);
        }
        // Said out loud, because until now a failure here was silent and the
        // default address was announced as though it had been read. On a TV
        // with another PC on the port that default names, both then claim the
        // same input.
        if (!device_->addressNote().empty()) {
            Log("address: %s", device_->addressNote().c_str());
        }
        // Memory only; the store files it away later, outside this lock.
        g_tvStore.Seen(device_->identity());
        ForgetPeersIfTvChanged();
        Log("attached to %s as %s (logical address %u%s), shown on TV as '%s'",
            device_->port().Describe().c_str(),
            cec::FormatPhysicalAddress(knownAddress_).c_str(),
            device_->logicalAddress(), FreeUseNote(device_->logicalAddress()),
            device_->osdName().c_str());
        return true;
    }

    // Re-sends the input label a few times over the first few seconds.
    //
    // At cold boot the TV's own CEC stack is often not ready when the service
    // first announces - roughly half of boots left the input showing the TV's
    // "Playback_1" placeholder. The TV accepts a name while it holds only a
    // placeholder and ignores one once it has a real name, so repeating this is
    // safe: it fills in a missed label and is a no-op the rest of the time.
    //
    // Startup only, and its value is not firmly established - one boot that had
    // been 50/50 came up correct with it. On the wake path the same idea was
    // measured and does not work: after a re-announce the TV ignores
    // <Set OSD Name> entirely, so repeating it only wasted time.
    void ReassertOsdName(int attempts, int delayMs, HANDLE cancelEvent) {
        std::string sent = PcName();
        for (int i = 0; i < attempts; ++i) {
            if (cancelEvent &&
                WaitForSingleObject(cancelEvent, delayMs) == WAIT_OBJECT_0) {
                return;
            }
            // Never while the link is busy. This holds the lock for a frame
            // plus a 500ms drain, and a keypress arriving in that window waits
            // the whole time - measured as fast once it finally runs, because
            // the wait happens before any of it. Checked outside the lock so
            // that the check cannot itself queue behind a command in flight.
            // The label is cosmetic; a button that responds is not.
            const DWORD now = GetTickCount();
            if (lastTrafficTick_ && now - lastTrafficTick_ < kBusyLinkMs) continue;

            std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
            if (!device_) return;
            device_->SendOsdName();
            sent = device_->osdName();
            device_->ServiceQueries(500);
        }
        // The name as sent, which is the PC name cut to the fourteen
        // characters a CEC name may have, rather than the PC name itself.
        Log("re-asserted input name '%s'", sent.c_str());
    }

    // Wake path: only finds the port. CecDevice::WakeDisplay() does the rest in
    // an order that puts <Image View On> on the wire before the slower work.
    bool WakeAttach() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        return AcquireDevice();
    }

    // Retries until the GPU driver and display link are actually up. Used at
    // service start, where we may well be running before either is ready.
    // Deliberately NOT holding the lock across the whole loop: this can run
    // for two minutes at cold boot, and a suspend arriving in that window must
    // not be blocked behind it. Each Attach() takes the lock on its own.
    bool AttachWithRetry(int attempts, int delayMs, HANDLE cancelEvent) {
        for (int i = 0; i < attempts; ++i) {
            if (Attach()) {
                looking_ = false;
                return true;
            }
            if (i == 0) {
                Log("not ready yet (%s); retrying", lastError_.c_str());
            }
            if (cancelEvent &&
                WaitForSingleObject(cancelEvent, delayMs) == WAIT_OBJECT_0) {
                looking_ = false;
                return false;
            }
        }
        // Cleared here as well, and this is the state the menu line turns on.
        // Up to this moment the tray says it is connecting, which is the truth
        // at a cold boot: the pipe server is up a good second before the GPU
        // driver will answer, so a menu opened straight after logon found a
        // service that was running and not yet attached, and said no TV had
        // been found - to somebody whose TV was about to work perfectly. Only
        // once this loop has spent its two minutes is that worth saying.
        looking_ = false;
        Log("giving up attaching: %s", lastError_.c_str());
        return false;
    }

    // Sleep must be fast - Windows allows only a short window before it
    // suspends regardless - so this does not re-announce unless it has to.
    // Returns whether a <Standby> frame actually reached the wire. That is a
    // question about the transport, not about the television: a set that hears
    // the command and ignores it still counts, because the retry paths above
    // handle that case, whereas a failed DPCD write means nothing was sent and
    // nobody should believe the TV is off.
    bool SendStandby() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        const DWORD start = GetTickCount();
        cec::TxResult tx;
        if (device_ && device_->SendStandby(&tx)) {
            // Exactly one <Standby>, never a blind retry. The spec defines it
            // as "switch to standby", but the TV used for development maps it
            // onto the remote's power key, which is a TOGGLE - sending it twice
            // 120ms apart reliably turned the TV off and straight back on.
            // Measured: single send ON -> STANDBY, double send ON -> ON.
            // Any future retry here must first confirm the TV is still on.
            Log("<Standby> sent (%lums, 0x3004=0x%02X)", GetTickCount() - start,
                tx.irqFlags);
            lastStandbySentTick_ = GetTickCount();
            // Anything the tray reports from here on is input that happened
            // after the television was told to switch off, which is what tells
            // a person apart from the set's own signal dropping.
            g_userActiveTick = 0;
            return true;
        }
        Log("standby failed (%s, 0x3004=0x%02X); reinitialising",
            device_ ? device_->lastError().c_str() : "not attached", tx.irqFlags);

        // Bounded recovery only. A full Attach() here would read EDID and run
        // the announce, which on a dead link can take several seconds - time
        // the machine does not give us while suspending. Reclaiming the logical
        // address is two DPCD writes and is enough to get a frame out.
        if (device_ && device_->Initialise() && device_->SendStandby()) {
            Log("<Standby> sent after reinitialise (%lums)", GetTickCount() - start);
            lastStandbySentTick_ = GetTickCount();
            return true;
        }
        Log("standby FAILED after %lums", GetTickCount() - start);
        return false;
    }

    // Suspend path. Sends <Standby> once, then deliberately holds the handler
    // open while the TV completes its transition.
    //
    // The TV takes 1.5-2.2s to reach standby, but the DisplayPort link - and
    // with it the adapter's power - goes away as soon as the machine suspends.
    // How quickly that happens after the handler returns varies between runs,
    // which is the leading explanation for standby working on some sleeps and
    // not others with no other variable changing. Blocking here keeps the link
    // alive long enough for the TV to finish.
    //
    // A resend needs "still on" from a question asked once the set has had time
    // to act, which WaitForStandby only reports from 2.5 seconds on - so at the
    // default settle of 2.5 seconds this path sends <Standby> once and never
    // again, as hard rule 1 asks. It had resent since the first version, on an
    // answer from two seconds in: four of eight sleeps on the desktop, the TV
    // already powering down each time. A settle raised by SuspendSettleMs can
    // see a real "on", and then the retry below is the right thing to do.
    void SendStandbyAndSettle(int settleMs) {
        // Waited for, not queued behind. A graphics call can block for as long
        // as it likes and one has been seen to: on Intel, with the screen off,
        // a single AUX call did not return for nearly ten minutes, holding this
        // lock throughout. Windows gives a suspend about two seconds, so a
        // handler that waits for a lock like that achieves nothing and delays
        // the machine going down. Whoever holds it is mid-command on the same
        // television anyway.
        std::unique_lock<std::recursive_timed_mutex> guard(
            mutex_, std::chrono::milliseconds(1500));
        if (!guard.owns_lock()) {
            Log("sleep: the CEC link is busy and did not come free; "
                "the TV is left as it is");
            return;
        }
        const DWORD start = GetTickCount();
        // Nothing to switch off. This used to go through SendStandby, which
        // logged "standby failed (not attached); reinitialising" and then
        // "standby FAILED" - on the Deck, left away from any television for
        // three days, at every one of its three-hourly sleeps, where nothing
        // had failed at all.
        if (!device_) {
            Log("sleep: no TV is attached; nothing sent");
            return;
        }
        // Asked from what the bus has already said, never by asking the bus:
        // this path has a deadline. A set that has said nothing counts as
        // showing this PC, which is the single-device case and how this
        // behaved before.
        if (!device_->OnScreen()) {
            Log("sleep: the TV is showing another input; left alone");
            return;
        }

        // Re-announce before the command, not just at wake. The TV ignores
        // <Standby> from a device it no longer considers a live member of the
        // topology, and our registration goes stale between the wake and the
        // suspend - which can be minutes. Reproduced directly: after sending
        // bare frames for a while the TV refused every <Standby>, directed and
        // broadcast alike, and a single re-announce restored it immediately.
        //
        // This costs about a second, which the suspend budget can afford, and
        // it is the same handshake the wake path already relies on.
        //
        // Skipped when something announced moments ago, which in practice means
        // the display-off path did it and then stood aside for this one. Paying
        // for it twice is a second this path does not have.
        if (!lastAnnounceTick_ ||
            GetTickCount() - lastAnnounceTick_ >= kFreshAnnounceMs) {
            device_->Announce(device_->physicalAddress());
            lastAnnounceTick_ = GetTickCount();
        }

        SendStandby();

        // Poll rather than sleep a fixed time: a fast TV lets the machine
        // suspend sooner, a slow one gets the whole budget. How long a set takes
        // to act on <Standby> is not specified by CEC and differs by model.
        const cec::PowerStatus after = device_->WaitForStandby(settleMs);
        if (after == cec::PowerStatus::Standby ||
            after == cec::PowerStatus::TransitioningToStandby) {
            Log("TV reports %s after %lums", cec::PowerStatusName(after),
                GetTickCount() - start);
            return;
        }

        // A refusal is not a lost frame, so it gets the other route rather than
        // the same one twice. See PressPowerOff. Asked before the "on" test, so
        // a set that refused and then said nothing in time still gets it: the
        // power-off key can only turn a set off, so it is safe on any answer.
        if (device_->Refused(0x36, start)) {
            PressPowerOff(settleMs, start);
            return;
        }

        if (after != cec::PowerStatus::On) {
            Log("TV has not confirmed standby after %lums; sent once, as a check "
                "this early cannot tell a slow set from one that ignored it",
                GetTickCount() - start);
            return;
        }

        // Still on after a hold that outlasts the transition. A plain resend
        // does not help here - measured: with the TV in this state, a second
        // <Standby>, and even a broadcast 4F 36, were both ignored. Only
        // rejoining the topology restores it, so retry that way.
        Log("TV still ON after %lums; re-announcing and retrying",
            GetTickCount() - start);
        device_->Announce(device_->physicalAddress());
        device_->SendStandby();
        const cec::PowerStatus retried = device_->WaitForStandby(settleMs);
        Log("after retry, TV reports %s (%lums)", cec::PowerStatusName(retried),
            GetTickCount() - start);
        if (retried == cec::PowerStatus::On) PressPowerOff(settleMs, start);
    }

    // The second way of turning a television off, for a set that will not have
    // the first.
    //
    // The development TCL answers <Standby> with <Feature Abort>, reason
    // "unrecognized opcode", while answering everything else normally - so no
    // amount of re-announcing, re-addressing or resending was ever going to
    // work, and a good deal of each was tried. What it does accept is remote
    // keys, which is why volume worked throughout, and CEC has a power-off key
    // among them. It is a dedicated off rather than the toggle, so it cannot
    // turn a set back on, and it is sent only after the television has said it
    // is still on.
    //
    // Returns whether the set went off. Called with the lock held.
    bool PressPowerOff(int settleMs, DWORD start) {
        if (!device_) return false;
        Log("the TV will not take <Standby>; pressing the power-off key");
        device_->SendPowerOffKey();
        const cec::PowerStatus after = device_->WaitForStandby(settleMs);
        Log("after the power-off key, TV reports %s (%lums)",
            cec::PowerStatusName(after), GetTickCount() - start);
        return after != cec::PowerStatus::On;
    }

    // Sends <Standby>, confirms the TV actually acted, and resends once if it
    // did not. Checking first is what makes the retry safe - a blind resend
    // would toggle this TV straight back on.
    //
    // Used by both power paths, with different budgets. Preshutdown allows
    // roughly three minutes; suspend allows about two seconds before Windows
    // stops waiting, so its settle and query windows are much tighter and it
    // does not re-verify after the resend.
    //
    // The settle delay does double duty on suspend: it holds the machine
    // briefly so the TV can act on the command before the DisplayPort link
    // drops out from under it.
    // announceFirst is only ever false for a caller that has just announced
    // itself and wants the command to go out the moment it decides to send one.
    // The announce still happens; it has simply been moved earlier.
    // Whether a wait for the TV's answer ran far past its own limit, which means
    // this thread was not running for the overrun and the answer is not about
    // the moment the command went out. Said in the log when it happens.
    //
    // Seen on the Dell, whose screen going off is its Modern Standby sleep: the
    // ten-second verify began at 23:14:31, the machine slept inside it, and it
    // returned at 23:16:33 as the machine woke - with "still ON", on which it
    // re-announced and sent <Standby> a second time, two minutes after the
    // first. On the TCL that is the toggle hard rule 1 forbids. Such an answer
    // is treated as no answer: inconclusive, so nothing is resent.
    //
    // The allowance is one frame that never reports its transmit status
    // (about 3.2 seconds) plus a poll interval, rounded up. The wait checks its
    // deadline after each frame and listen, so a running wait overshoots by at
    // most that much, and anything past it was time spent not running. Were a
    // slow adapter ever to overrun further than that while running, the cost
    // is a resend not made, which is the direction hard rule 1 asks mistakes
    // to fall in.
    bool WaitWasHeldUp(DWORD waitStart, int settleMs) {
        const DWORD took = GetTickCount() - waitStart;
        if (took <= static_cast<DWORD>(settleMs) + kStandbyWaitOverrunMs) {
            return false;
        }
        Log("the wait for the TV's answer took %lums against a %dms limit, most "
            "likely because the machine slept; not resending <Standby>",
            took, settleMs);
        return true;
    }

    bool SendStandbyVerified(int settleMs, bool verifyResend,
                             bool announceFirst = true) {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        const DWORD start = GetTickCount();
        // As for sleep: no television, so nothing failed. See
        // SendStandbyAndSettle.
        if (!device_) {
            Log("no TV is attached; nothing sent");
            return false;
        }

        // Same reason as the suspend path: the TV ignores <Standby> from a
        // device it no longer holds in its topology, and the registration made
        // at wake has gone stale by the time the machine shuts down. Omitting
        // this here while adding it to suspend is exactly why sleep started
        // working and shutdown kept failing.
        if (announceFirst) device_->Announce(device_->physicalAddress());

        const bool sent = SendStandby();

        const DWORD waited = GetTickCount();
        const cec::PowerStatus after = device_->WaitForStandby(settleMs);
        if (after == cec::PowerStatus::Standby ||
            after == cec::PowerStatus::TransitioningToStandby) {
            Log("TV confirmed %s (%lums)", cec::PowerStatusName(after),
                GetTickCount() - start);
            return sent;
        }
        // A set saying it is off is believed whenever it says so, above. Any
        // other answer is only worth acting on if it came inside the window.
        if (WaitWasHeldUp(waited, settleMs)) return sent;
        if (after == cec::PowerStatus::Unknown) {
            // Genuinely inconclusive - some sets go quiet while powering down,
            // and this one has been observed staying on and simply not
            // answering. Do not resend on a guess: on a TV that treats
            // <Standby> as a toggle, guessing wrong turns it back on. This also
            // covers a set that only answered before it had had time to act,
            // which WaitForStandby does not count.
            Log("TV gave no answer late enough to act on after <Standby> "
                "(%lums); leaving it alone", GetTickCount() - start);
            return sent;
        }

        // A set that answered <Feature Abort> has heard the command and will
        // not do it, however often it is asked. Straight to the other route.
        if (device_->Refused(0x36, start)) {
            return PressPowerOff(settleMs, start);
        }

        Log("TV still reports %s after <Standby>; re-announcing and retrying",
            cec::PowerStatusName(after));
        device_->Announce(device_->physicalAddress());
        device_->SendStandby();
        if (!verifyResend) {
            Log("resent <Standby> (%lums)", GetTickCount() - start);
            return sent;
        }
        const DWORD rewaited = GetTickCount();
        const cec::PowerStatus retried = device_->WaitForStandby(settleMs);
        if (retried != cec::PowerStatus::Standby &&
            retried != cec::PowerStatus::TransitioningToStandby &&
            WaitWasHeldUp(rewaited, settleMs)) {
            return sent;   // inconclusive, as for a set that never answered
        }
        Log("TV reports %s (%lums)", cec::PowerStatusName(retried),
            GetTickCount() - start);

        // Two <Standby> messages and the set is still on, so the last rung.
        if (retried == cec::PowerStatus::On) return PressPowerOff(settleMs, start);

        // Deliberately NOT `sent`. The frame reached the wire, but the TV has
        // just said twice that it is still on, and the display-off path reads
        // this as ownership - meaning "the TV is off because of us, so the
        // sleep and shutdown paths should not send <Standby> again". Claiming a
        // set that is demonstrably still on is how a later sleep skips the one
        // command that would have worked.
        return retried != cec::PowerStatus::On;
    }

    // After a restart Windows Update made by itself: leaves the TV as it is,
    // and returns false when the caller should turn it on as after any start.
    //
    // A set that says it is off is treated as though this service had turned
    // it off, which is the case the screen-off path already handles in every
    // way that matters here. A person coming back - a key at the sign-in
    // screen, a wake, the tray seeing the mouse - turns it on, so nobody has
    // to find the TV remote just to use the PC after an update; and a sleep
    // or shutdown before then knows it is off and sends nothing.
    //
    // A set that will not say is turned on, as before this existed. Of the two
    // ways to be wrong, a TV that comes on in the night is the one a person
    // can see and fix, where a TV that will not come on in the morning traps
    // them with a remote they may not have to hand.
    bool LeaveTvForAPerson() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return false;
        const cec::PowerStatus status = device_->QueryPowerStatus(1500);
        if (status == cec::PowerStatus::Standby ||
            status == cec::PowerStatus::TransitioningToStandby) {
            SetDisplayOwnsStandby(true);
            Log("started after a Windows Update restart with the TV off; it "
                "comes on when someone uses the PC");
            return true;
        }
        if (status == cec::PowerStatus::On ||
            status == cec::PowerStatus::TransitioningToOn) {
            Log("started after a Windows Update restart with the TV on; left "
                "as it is");
            return true;
        }
        Log("started after a Windows Update restart, and the TV would not say "
            "whether it is on, so it is turned on as after any start");
        return false;
    }

    // Whether a sleep or shutdown should send <Standby>, asked of the TV itself
    // where it will say. believedOff is what this service's own record says:
    // that it has turned the set off and nothing has turned it on since.
    //
    // Asked because that record can be wrong both ways, and either way the
    // set ends up in the wrong state for the night. It says off while the set
    // is on when somebody turned it on with its own remote and the set said
    // nothing about it - the Fire TV and the Vizio never say which input they
    // show - and then <Standby> was skipped and the set stayed on all night.
    // It says on while the set is off when a power-on was released and its
    // frame never landed, and then a <Standby> went to a set that was off.
    // That second case is only untidy on the TCL, measured on 2026-10-02: a
    // set switched off by its own remote three and eight seconds before a
    // sleep ignored the <Standby> it was then sent. Its toggle is a second
    // <Standby> arriving while it is still on its way down, which is the
    // window trusted without asking below.
    //
    // So the set's own answer decides: off means nothing is sent, on means
    // <Standby> is, whatever the record said. No answer leaves it to the
    // record, which is exactly how this behaved before it asked. Asking sends
    // no command and comes before any <Standby>, so it cannot toggle anything
    // and hard rule 1 is untouched - except in the one window where an answer
    // cannot be trusted: just after this service's own <Standby>, while the
    // set may still say "on" on its way down. That is trusted without asking.
    bool TvNeedsStandby(bool believedOff, const char* phase) {
        // Briefly, because both callers are working to a deadline and a held
        // lock means another path is mid-command on this same television.
        std::unique_lock<std::recursive_timed_mutex> guard(
            mutex_, std::chrono::milliseconds(300));
        if (!guard.owns_lock() || !device_) return !believedOff;
        if (believedOff && lastStandbySentTick_ &&
            GetTickCount() - lastStandbySentTick_ < kOwnStandbyTrustMs) {
            Log("%s: the TV is already off (the display blanked first)", phase);
            return false;
        }
        const cec::PowerStatus status = device_->QueryPowerStatus(kPowerCheckMs);
        if (status == cec::PowerStatus::Standby ||
            status == cec::PowerStatus::TransitioningToStandby) {
            Log("%s: the TV reports %s; nothing sent", phase,
                cec::PowerStatusName(status));
            return false;
        }
        if (status == cec::PowerStatus::On ||
            status == cec::PowerStatus::TransitioningToOn) {
            if (believedOff) {
                Log("%s: the TV reports %s, although this service had it off; "
                    "sending <Standby>", phase, cec::PowerStatusName(status));
            }
            return true;
        }
        if (believedOff) {
            Log("%s: the TV is already off (the display blanked first)", phase);
        }
        return !believedOff;
    }

    // Whether the television says it is on, or on its way there.
    bool TvIsAwake() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return false;
        const cec::PowerStatus status = device_->QueryPowerStatus(1500);
        return status == cec::PowerStatus::On ||
               status == cec::PowerStatus::TransitioningToOn;
    }

    // What the bus has said about which input is on screen, for the log. A
    // command the television refuses looks the same either way afterwards, and
    // this is the difference between "it ignored us" and "we asked while it was
    // showing something else and believed otherwise".
    std::string ScreenNote() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return "is not attached";
        if (!device_->activePathKnown()) {
            return "would not say which input it is showing";
        }
        if (!device_->activePathAnswered()) {
            return "did not answer, but last said " +
                   cec::FormatPhysicalAddress(device_->activePath()) +
                   " is on screen, which is this PC";
        }
        return "says " +
               cec::FormatPhysicalAddress(device_->activePath()) +
               " is on screen, which is this PC";
    }

    // Whether the television is showing this PC, for the paths that decide
    // outside this class. Not attached counts as yes, so that the reason logged
    // is the real one rather than this.
    bool ShowingThisPc() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return true;
        // Shutdown has minutes, so it asks too.
        device_->AskWhoIsOnScreen(700);
        return device_->OnScreen();
    }

    // Whether a power-down path still needs to send <Standby>.
    //
    // Taken under the CEC lock, and that is the entire reason it exists as a
    // function. The flag it reads is set only once a frame has actually gone
    // out, so a suspend asking while the display-off path is mid-announce would
    // get "no" and send its own - correct. But asking *without* the lock could
    // also catch that path a microsecond before it sends, and then both would.
    // Holding the lock means the question is asked only when nobody is between
    // deciding and sending.
    bool StandbyAlreadySent() {
        std::unique_lock<std::recursive_timed_mutex> guard(mutex_, std::defer_lock);
        // Not had means the display-off path is sending <Standby> itself, or
        // something has hung, and "yes, already sent" is the safe answer to
        // both: the alternative is a second <Standby> behind the first, a
        // toggle on the TCL.
        if (!LockForPowerDown(guard)) return true;
        return DisplayOwnsStandby();
    }

    // Takes the CEC lock for a sleep or shutdown that may have begun together
    // with a screen-off, waiting out the display-off path for as long as that
    // path is still deciding.
    //
    // Pressing Sleep turns the screen off and suspends at the same moment, and
    // when the screen-off arrived first both paths stood aside for each other.
    // The display-off path took the lock, asked what was on screen and
    // announced, then saw the sleep and left <Standby> to it, as it should. But
    // the sleep had waited a second for the lock, taken a lock it could not
    // get to mean that the display-off path was sending, and sent nothing. So
    // nothing was sent at all, and the TV stayed on with the PC asleep: twice
    // on 2026-10-19 and 20 on the desktop, where keep-awake meant sleep was
    // always by hand, and 31 times across the saved logs, most often followed
    // directly by the next wake. It is not a guess any more. The display-off
    // path says when it has passed its last look at the power-down and is
    // sending, and until it says so this waits, because it is still deciding
    // and will stand aside.
    //
    // False when the display-off path is sending, or the lock has not come
    // free within kPowerDownLockWaitMs.
    bool LockForPowerDown(std::unique_lock<std::recursive_timed_mutex>& guard) {
        const DWORD start = GetTickCount();
        for (;;) {
            if (guard.try_lock_for(std::chrono::milliseconds(100))) return true;
            if (displayCommitted_.load()) return false;
            if (GetTickCount() - start >= kPowerDownLockWaitMs) return false;
        }
    }

    // Rejoins the topology without touching the TV's power state. Called when
    // Windows dims the display, which on the systems that bother to send it is
    // a second or two of warning before the picture goes - so the announce that
    // the standby would otherwise have to wait for is already done.
    void PreAnnounce() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return;
        device_->Announce(device_->physicalAddress());
        lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
    }

    // The same, when the tray has worked out that the screen is about to go
    // off. Windows' own dim warning cannot be relied on for this: its hidden
    // "Dim display after" setting is left at the old timeout whenever someone
    // shortens the screen timeout in Settings, so it quietly never fires, and
    // without a warning the display-off path spends a second announcing while
    // the TV shows its no-signal screen.
    //
    // A guess, so it is careful to cost nothing when wrong. An announce changes
    // nothing on the TV. It is skipped while the link is in use, and never
    // lands inside a held volume key, and one done lately - most often by a
    // dim just before - is not repeated.
    void PrepareForBlank() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return;
        const DWORD now = GetTickCount();
        if (holding_ && now - holdStartTick_ < kAbandonedHoldMs) return;
        if (lastTrafficTick_ && now - lastTrafficTick_ < kBusyLinkMs) return;
        if (lastAnnounceTick_ && now - lastAnnounceTick_ < kPrepareFreshMs) return;
        Log("screen about to turn off: announcing now so the standby need not wait");
        device_->Announce(device_->physicalAddress());
        lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
    }

    // Display-off path: Windows has blanked the picture but the machine is
    // still running.
    //
    // Every other power command announces first and then sends, because a TV
    // ignores <Standby> from a device it no longer holds in its topology. That
    // announce costs about 1.1 seconds, and here it was landing *after* a
    // separate two-second settle - so the television spent four or five seconds
    // showing its own no-signal screensaver before the command that switches it
    // off ever arrived.
    //
    // The fix is that the announce and the settle are now the same wait.
    // Announcing changes nothing on the TV; it is topology traffic. So it runs
    // first, and the decision to send <Standby> is taken after it against a
    // display state re-read at that moment. Faster and no less careful: the
    // abort window is still there, it is just being used for something.
    //
    // Returns whether the TV was actually commanded. That answer is what
    // decides ownership: the display-on path turns a TV back on only if this
    // service is the one that turned it off, so a set the user switched off by
    // hand is not switched back on by a mouse jiggle.
    bool SendStandbyForDisplayOff() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        const DWORD start = GetTickCount();
        if (!device_ && !Attach()) {
            Log("display off: not attached (%s); leaving the TV alone",
                lastError_.c_str());
            return false;
        }

        // On the Steam Deck the port found earlier stops accepting writes once
        // the picture goes, and stays that way: every display-off <Standby>
        // failed at 0ms with "AUX write failed (ADL -1)", and so did a sleep
        // minutes later on the same device, while a port found afresh with the
        // display already off worked first time. So the two writes that set
        // the adapter up are made here as a check - harmless when they succeed,
        // since they only restate what is already set - and a failure means
        // finding the port again before anything is sent. The fresh port has
        // to announce, so the earlier announce no longer counts as fresh.
        if (!device_->Initialise()) {
            Log("display off: the adapter stopped answering (%s); attaching again",
                device_->lastError().c_str());
            ReleaseDevice();
            lastAnnounceTick_ = 0;
            if (!Attach()) {
                Log("display off: not attached (%s); leaving the TV alone",
                    lastError_.c_str());
                return false;
            }
        }

        // A sleep or shutdown that has already begun owns the television. Seen
        // here when the screen-off and the suspend arrive together and this
        // thread got the lock first; standing aside at once rather than after
        // the announce lets that path have the lock without waiting for it.
        if (PowerDownStarted()) {
            Log("display off: a sleep or shutdown has begun; leaving <Standby> "
                "to it");
            return false;
        }

        // A television showing something else is not this PC's to switch off.
        // Asked rather than assumed: routing frames are broadcast once, when
        // something changes, so a service that started since knows nothing and
        // would otherwise guess. This path has the time - the screen is already
        // off - and the answer is what the television itself acts on, since a
        // set refuses power commands from a device it is not showing.
        device_->AskWhoIsOnScreen(700);
        if (!device_->OnScreen()) {
            Log("display off: the TV is showing %s, not this PC; left alone",
                cec::FormatPhysicalAddress(device_->activePath()).c_str());
            return false;
        }
        Log("display off: the TV %s", ScreenNote().c_str());

        // Skipped only if something announced within the last few seconds -
        // in practice a dim event. Well inside the window the rest of this
        // service already treats a registration as good for, and the whole
        // point is that the announce still happened, just earlier.
        const bool fresh =
            lastAnnounceTick_ &&
            GetTickCount() - lastAnnounceTick_ < kDimAnnounceValidMs;
        if (!fresh) device_->Announce(device_->physicalAddress());
        const DWORD announced = GetTickCount();
        lastAnnounceTick_ = lastTrafficTick_ = announced;

        // Asked here rather than before the announce, because the announce is
        // where the time goes and the answer can change during it.
        if (!DisplayStillOff()) {
            Log("display came back during the announce (%lums); TV left on",
                announced - start);
            return false;
        }

        // A sleep or a shutdown that began during the announce takes over. Both
        // are working to a deadline and this is not, and the announce just done
        // is not wasted: whichever of them runs next finds it fresh and skips
        // its own, so the handover costs nothing.
        if (PowerDownStarted()) {
            Log("power-down started during the announce; leaving <Standby> to it");
            return false;
        }
        // Past the last look at the power-down: from here this path sends, and
        // a sleep or shutdown waiting for the lock is told so rather than left
        // to guess. Cleared on every way out. See LockForPowerDown.
        displayCommitted_ = true;
        struct Uncommit {
            std::atomic<bool>& flag;
            ~Uncommit() { flag = false; }
        } uncommit{displayCommitted_};

        // A set that is already off is left alone. <Standby> is a toggle on the
        // TCL (hard rule 1), so a TV switched off with its own remote shortly
        // before the screen timed out was turned back on - and counted as this
        // service's, so moving the mouse later turned it on again too. Asked
        // here, where there is no deadline; a set that does not answer is sent
        // <Standby> as before, since that is most often one that is on and slow.
        const cec::PowerStatus before = device_->QueryPowerStatus(1500);
        if (before == cec::PowerStatus::Standby ||
            before == cec::PowerStatus::TransitioningToStandby) {
            Log("display off: the TV is already off; left alone");
            return false;
        }

        // Ten seconds, not four. This path has no deadline - the machine is
        // awake and idle - and four was measured too tight: this TV has taken
        // 3531ms to report STANDBY, and a verify that gives up early reads ON,
        // re-announces, and resends, which on a set that treats <Standby> as a
        // toggle turns the television back on. That is exactly what the log
        // showed at 09:50 on 2026-09-07. The cap costs nothing when it is not
        // needed, because WaitForStandby returns as soon as the TV answers.
        const bool sent = SendStandbyVerified(10000, true, false);
        SetDisplayOwnsStandby(sent);

        // Time to the wire, not time to confirmation. The first version of this
        // line stamped after the verify and so reported 3485ms for a command
        // that had actually gone out at 1110ms - flattering the announce it was
        // meant to indict, and hiding that the remaining wait is the TV's own
        // standby transition, which is not ours to shorten.
        // Guarded, because lastStandbySentTick_ is only set when a frame
        // actually goes out. Subtracting from a zero that never happened
        // underflowed to "on the wire at 4292952281ms", which is worse than no
        // number at all: it looks like a measurement.
        // As a difference, so a tick count that wrapped during this does not
        // read as nothing having been sent.
        if (sent && static_cast<LONG>(lastStandbySentTick_ - start) >= 0) {
            Log("display off: announce %lums%s, <Standby> on the wire at %lums",
                announced - start, fresh ? " (skipped, already fresh)" : "",
                lastStandbySentTick_ - start);
        } else {
            Log("display off: announce %lums%s, nothing reached the wire",
                announced - start, fresh ? " (skipped, already fresh)" : "");
        }
        return sent;
    }

    // Wake has more time available, and the link has just come back, so this
    // re-attaches by default. Callers that have only just attached (service
    // start) can skip the redundant second pass.
    // quiet leaves out the failure, for the retries of a caller that has
    // logged the first one and says how it ended: a wake with no television
    // repeats it every two seconds for a minute, and the Deck's log carried
    // 320 identical lines from three days away from one.
    bool SendPowerOn(bool reattach = true, bool quiet = false) {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        const DWORD start = GetTickCount();
        if (reattach && !WakeAttach()) {
            if (!quiet) Log("power-on: attach failed (%s)", lastError_.c_str());
            return false;
        }
        if (!device_) {
            Log("power-on: not attached");
            return false;
        }

        if (g_settings.diagnosePower) {
            // Reclaim the logical address first - the link dropped, so CEC is
            // not usable until this runs - then ask before changing anything.
            device_->Initialise();
            const cec::PowerStatus before = device_->QueryPowerStatus(1200);
            Log("diagnostic: TV reported %s on resume, before <Image View On>",
                cec::PowerStatusName(before));
        }

        uint32_t firstMs = 0;
        const bool woke = device_->WakeDisplay(&firstMs);
        // WakeDisplay re-read the EDID on the way, after <Image View On> was
        // already out, so this is where a set swapped while the PC slept gets
        // noticed. Memory only - see TvStore - so the wake never waits on it.
        g_tvStore.Seen(device_->identity());
        // Here as well as in the attach, because this path never goes through
        // Attach: it builds its device with WakeAttach, which reads nothing, so
        // this is the first point at which the set is known. The peer list
        // belongs to this object, not the device, and outlives every rebuild.
        ForgetPeersIfTvChanged();
        // The failure, read before the address check below, which can
        // re-attach and, if that fails, drop the device: reading it afterwards
        // dereferenced a null pointer and took the service down, on a wake that
        // had failed with the cable moved - the one moment both happen together.
        const std::string error = woke ? std::string() : device_->lastError();
        // And the address that same read produced, which until now went no
        // further than the device itself. See AdoptAddressIfMoved.
        AdoptAddressIfMoved();
        if (woke) {
            Log("<Image View On> on the wire in %ums; announce complete at %lums",
                firstMs, GetTickCount() - start);
            return true;
        }
        // Not "FAILED": every caller tries again or asks the television, and
        // says so when it gives up. The commonest case by far is a frame whose
        // transmit status the adapter never reported in the seconds after a
        // wake, and the set coming on anyway - ten of those on the desktop in
        // four days, every one followed by the TV reporting that it was on.
        if (!quiet) {
            Log("power-on: not confirmed after %lums (%s)",
                GetTickCount() - start, error.c_str());
        }
        lastError_ = error;   // for the caller's closing line
        return false;
    }

    // Display-on path. Retries, which the resume path has never needed.
    //
    // A resume arrives after Windows has brought the whole machine back; this
    // fires the instant Windows re-enables the output, which can be a few
    // seconds after the TV was told to stand by and while both ends of the HDMI
    // link are still settling. Measured on an LG OLED: a blank at 09:55:43, the
    // display back at 09:55:54, and the wake seven seconds behind the standby
    // failing with "adapter never reported transmit status" - leaving the TV
    // off with the PC awake, which is the worst state this feature can produce.
    //
    // Deliberately not holding the lock across the loop: a retry runs for
    // seconds, and a suspend arriving in that window must not queue behind it.
    // Each attempt takes the lock on its own.
    void SendPowerOnWithRetry(HANDLE cancelEvent) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (SendPowerOn(true, attempt > 0)) return;
            // A frame the adapter could not get acknowledged says nothing about
            // whether the television heard it, and this is the moment a set is
            // least able to answer: it is waking, which takes it seconds, and
            // <Image View On> is what woke it. Asking is the rule everywhere
            // else here and it belongs here too - four more rounds of frames
            // and a log line saying the TV may be off, at a television plainly
            // coming on, is how this read before.
            if (TvIsAwake()) {
                Log("power-on: the frames went unacknowledged, but the TV "
                    "reports it is on");
                return;
            }
            if (cancelEvent &&
                WaitForSingleObject(cancelEvent, 1200) == WAIT_OBJECT_0) {
                return;
            }
            // A sleep or shutdown that began meanwhile owns the television
            // now, and a retry would turn it back on behind that path's
            // <Standby>. The patient wake loop has always stood aside for
            // this; the screen-on and start paths that use this one had not.
            if (PowerDownStarted()) return;
        }
        // Through LastError, which takes the lock: this loop deliberately does
        // not hold it, and the string may be being written meanwhile.
        Log("power-on gave up (%s); the TV may be left off with the PC awake",
            LastError().c_str());
    }

    // Why the last attach or power-on went wrong, for a caller's closing line.
    std::string LastError() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        return lastError_;
    }

    // Answers whatever the TV has asked, if nothing else is using the link.
    //
    // try_lock rather than lock, and that is the whole reason this is safe to
    // run on a timer: a keypress must never wait behind housekeeping. A tick
    // that finds the link busy simply does nothing and comes back later, which
    // costs nothing, because a busy link means a command is in flight and the
    // command paths service the receive buffer themselves.
    //
    // Returns the number of questions answered, or -1 if the tick was skipped.
    int PumpQueries() {
        // Stand aside while the link is in active use. try_lock alone is not
        // enough during a held volume button: the lock is free between one
        // press and the next, so the pump could take it and then hold it long
        // enough to delay the following press. A TV does not interrogate a
        // device in the middle of a volume run, so there is nothing to lose by
        // waiting for the gap.
        //
        // Except while a volume question is out. Then its answer is the thing
        // being waited for, and it would otherwise sit unread in the adapter
        // until the buttons went quiet - which during a hold is never. Looking
        // is a register read, not a frame, so it is not what delays a press.
        const bool awaitingAnswer = audioAsked_.load();
        const DWORD sinceTraffic = GetTickCount() - lastTrafficTick_.load();
        if (!awaitingAnswer && lastTrafficTick_ && sinceTraffic < kPumpYieldMs) {
            return -1;
        }

        std::unique_lock<std::recursive_timed_mutex> guard(mutex_, std::try_to_lock);
        if (!guard.owns_lock()) return -1;
        if (!device_) {
            if (audioAsked_.exchange(false)) PublishVolume("OK");
            AttachIfDisplayReturned();
            return 0;
        }
        const int answered = device_->ServicePending(kPumpFramesPerTick);
        PublishAudioNews();
        MoveIfAddressTaken();
        MapTheBusIfWanted();
        GreetPeersNow();
        RefreshSnapshot();
        RejoinBusIfQuiet();
        RecheckAddressIfDisplayChanged();
        WarnIfBusRefusesBroadcasts();
        WarnIfFramesMalformed();
        return answered;
    }

    // Leaves the bus alone on the way to sleep, and takes it up again if the
    // sleep did not happen after all.
    //
    // An adapter usually stays powered while the PC sleeps, and a sleeping PC
    // collects nothing that arrives. A CEC device that has stopped collecting
    // its messages ends up refusing broadcasts, which is not a small thing: it
    // cost an afternoon here, with the desktop unable to turn the television
    // off, the television answering everything else normally, and the cause
    // being a laptop asleep in the corner. So this service does not leave one
    // of those behind.
    void LeaveBusQuiet() {
        // Waiting out a display-off path that is still deciding, as the
        // <Standby> does. With half a second, a sleep that began with a
        // screen-off gave up while that path announced, and the adapter stayed
        // on the bus through the sleep - the very thing this exists to stop.
        std::unique_lock<std::recursive_timed_mutex> guard(mutex_, std::defer_lock);
        if (!LockForPowerDown(guard) || !device_) return;
        // Said before going quiet, so the others drop this PC from their menus
        // at once instead of offering one that cannot answer.
        device_->SendAppMessage(cec::kLogicalAddressBroadcast,
                                cec::CecDevice::kAppBye);
        if (device_->Quiesce()) {
            quiesced_ = true;
            Log("left the CEC bus quiet, so this adapter cannot hold it up "
                "while the PC is asleep");
        }
    }

    // Called from the pump, which runs about once a second. A suspend that was
    // cancelled, or a display that blanked and came back without a sleep,
    // otherwise leaves this deaf until the next wake.
    void RejoinBusIfQuiet() {
        if (!quiesced_ || !device_ || PowerDownStarted()) return;
        // Not while the screen is off. On a Modern Standby machine that is what
        // sleeping looks like, and rejoining then would undo the whole point.
        if (DisplayStillOff()) return;
        quiesced_ = false;
        device_->Initialise();
        Log("rejoined the CEC bus");
    }

    // Takes up an address the device has read for itself, when it is not the
    // one this object has been working from.
    //
    // The two can disagree because the device re-reads the EDID on its own on a
    // wake, so that a television swapped or re-plugged during a sleep is
    // noticed without a restart. That read reached the device and stopped
    // there: knownAddress_ and g_ownAddress kept the old value, and those are
    // what everything above the device compares against.
    //
    // Caught in a log. The Dell was unplugged from a Fire TV where it was
    // 1.0.0.0, carried to the TCL, and plugged into that set's port 2. The
    // device knew it was 2.0.0.0 and answered requests for it correctly, while
    // the service went on believing 1.0.0.0 - which on that television is the
    // desktop. So the menu labelled the desktop "this PC", the desktop's
    // <Active Source> read here as this PC's own claim succeeding, and a
    // screen-off would have taken the television down while the desktop was on
    // it. Nothing but a restart cleared it.
    //
    // Attaching again rather than assigning the field is the point: this PC has
    // to say where it is now, to the television and to every other copy of this
    // app, and Attach is what says it. The address has just been read, so it is
    // not read a second time.
    // Returns false only when this PC has moved and attaching where it now is
    // failed, which is the caller's cue to come back to it. Nothing to do
    // counts as done.
    bool AdoptAddressIfMoved() {
        if (!device_) return false;
        if (!device_->physicalAddress() ||
            device_->physicalAddress() == knownAddress_) {
            return true;
        }
        Log("this PC has moved: %s now, %s before",
            cec::FormatPhysicalAddress(device_->physicalAddress()).c_str(),
            cec::FormatPhysicalAddress(knownAddress_).c_str());
        return Attach();
    }

    // Finds the port again when a display arrives and this service has none.
    //
    // A device is dropped when the adapter stops answering - a cable pulled
    // while the screen goes off is enough - and nothing used to bring it back
    // except a command, a power event or the next screen-off. Measured on the
    // Deck: the cable went back at 00:05:11 and the service attached at
    // 00:05:56, when something happened to ask; meanwhile this PC was missing
    // from the bus and its own menu still showed the Dell asleep.
    //
    // Only after an arrival, so a machine on a plain monitor does not search
    // for a CEC port that is not there. A minute of attempts five seconds apart,
    // since the link may take a while to come up after the cable does; the
    // window is closed by any successful attach, whoever makes it. Not while the
    // startup loop is still looking, which owns the first attach, nor while the
    // screen is off or the machine is going down - the arrival is kept for when
    // the screen returns.
    void AttachIfDisplayReturned() {
        if (looking_ || PowerDownStarted() || DisplayStillOff()) return;
        const DWORD now = GetTickCount();
        if (g_displayArrived.exchange(false)) {
            attachUntilTick_ = now + kAttachAfterArrivalMs;
            if (!attachUntilTick_) attachUntilTick_ = 1;   // never the "off" value
            nextAttachTick_ = now;
        }
        if (!attachUntilTick_) return;
        if (static_cast<LONG>(now - attachUntilTick_) >= 0) {
            attachUntilTick_ = 0;
            Log("a display arrived, but no CEC port answered within a minute "
                "(%s)", lastError_.c_str());
            return;
        }
        if (static_cast<LONG>(now - nextAttachTick_) < 0) return;
        nextAttachTick_ = now + kAttachAfterArrivalEveryMs;
        // Attach closes the window itself on success, and logs the attach.
        // The arrival is handed on to the address recheck, which Windows' copy
        // of the EDID may still need a minute of, exactly as for an arrival
        // with a device in hand.
        if (Attach()) g_displayArrived = true;
    }

    // A display arriving is the one moment this PC's place in the topology can
    // change with nothing here having caused it: the cable has moved to another
    // HDMI port, or to another television altogether. Windows says so, the
    // address is read again, and AdoptAddressIfMoved does the rest.
    //
    // Once per arrival rather than once per tick, and never while the screen is
    // off - on a Modern Standby machine that is what sleeping looks like - or
    // on the way down. The flag stays set until a moment when reading is worth
    // something, which is why it is cleared here and not in the handler.
    void RecheckAddressIfDisplayChanged() {
        // Nothing is read while the screen is off - on a Modern Standby machine
        // that is what sleeping looks like - or on the way down. The flag is
        // not taken until then either, so an arrival during a sleep is answered
        // when the machine comes back rather than thrown away.
        if (!device_ || quiesced_ || PowerDownStarted() || DisplayStillOff()) {
            return;
        }
        const DWORD now = GetTickCount();
        if (g_displayArrived.exchange(false)) {
            // A second arrival inside a window simply starts it again, which is
            // what moving a cable twice in quick succession looks like.
            recheckUntilTick_ = now + kAddressRecheckMs;
            if (!recheckUntilTick_) recheckUntilTick_ = 1;   // never the "off" value
            nextRecheckTick_ = now;
        }
        if (!recheckUntilTick_) return;
        if (static_cast<LONG>(now - recheckUntilTick_) >= 0) {
            CloseAddressRecheck();
            return;
        }
        if (static_cast<LONG>(now - nextRecheckTick_) < 0) return;
        nextRecheckTick_ = now + kAddressRecheckEveryMs;
        device_->DiscoverPhysicalAddress();
        if (device_->physicalAddress() != knownAddress_) {
            // Closed on success only. An attach can fail while the link is
            // still settling a second after the cable went in, and the window
            // is what brings it back round.
            //
            // Except that a failed attach drops the device, and this returns
            // at its first line without one, so the window never came round:
            // the retry lived only in this comment. A failure is handed to the
            // search for a port instead, which retries the same way.
            if (AdoptAddressIfMoved()) {
                recheckUntilTick_ = 0;
            } else if (!device_) {
                recheckUntilTick_ = 0;
                g_displayArrived = true;
            }
            return;
        }
        // The sink answered for itself, so that is the answer: this PC is where
        // it was. Only Windows' copy is worth asking twice.
        if (!device_->addressFromWindows()) CloseAddressRecheck();
    }

    // Stops asking, and says what the last read found.
    //
    // Said out loud because the alternative is a log that cannot tell "Windows
    // still reports the port the cable has left" from "the check never ran",
    // and those want quite different fixes - the first was a full minute of
    // being told 2.0.0.0 about a cable sitting in port 3. The note is carried
    // along because it names which copy of the EDID answered, which is the next
    // question every time. One line per arrival, a handful a day.
    void CloseAddressRecheck() {
        recheckUntilTick_ = 0;
        const std::string note = device_ ? device_->addressNote() : std::string();
        Log("display arrived, and this PC is still %s%s%s",
            cec::FormatPhysicalAddress(knownAddress_).c_str(),
            note.empty() ? "" : "; ", note.c_str());
    }

    // Deliberately NOT moving address when a television refuses <Standby>.
    //
    // It was tried, on 2026-09-25, and it is the wrong answer twice over. A
    // refusal is rarely about the address: this TCL refused with <Feature
    // Abort> "unrecognized opcode" while answering everything else, and went on
    // refusing across three different addresses. And each move writes another
    // device into the set's own table, which is the thing that had gone wrong -
    // the desktop went 4, 8, 4, 11 in half an hour and the television ended up
    // refusing every one of them. What actually cleared it was unplugging the
    // TV at the wall, after which the set re-announced itself and accepted the
    // desktop again immediately.
    //
    // Moving is still right for a proven clash, which is a different thing and
    // is handled in MoveIfAddressTaken: a frame from our own address cannot be
    // ours, so there is no guessing involved.

    // One hello to everyone, every ten seconds, and any other copy of this app
    // answers with its name and port.
    //
    // A single frame on an idle link, which is what lets the menu open with the
    // list already right instead of waiting on the bus while somebody holds a
    // menu open. It also covers the announcement a PC makes when it attaches
    // going astray, which a bus refusing broadcasts does, and it is how a PC
    // that has been unplugged rather than sent to sleep - which says nothing at
    // all - stops being offered: anything not heard from in three rounds is
    // dropped.
    // Drops any neighbour sitting on this PC's own CEC address, and tells the
    // device which addresses other copies of this app are using.
    //
    // A peer at our own address is always wrong and never harmless. It cannot
    // answer a poll, because a device does not acknowledge itself, so it is
    // reported asleep for ever; it cannot be switched to, because the request
    // would be addressed to this machine; and it occupies the menu row of a
    // device that is in fact awake and reachable. It happens when this PC moves
    // onto an address a neighbour already held, which is exactly the fault the
    // avoid mask below exists to prevent - but the entry has to be cleaned up
    // whichever way it got there, and this is the one place that runs often
    // enough to do it.
    //
    // Seen on a Fire TV with two PCs: each held a stale entry on its own
    // address, so each reported the other asleep while both were awake, and
    // neither could switch to the other.
    void TidyPeerAddresses() {
        if (!device_) return;
        const uint8_t mine = device_->logicalAddress();
        uint16_t avoid = 0;
        for (size_t i = peers_.size(); i > 0; --i) {
            const Peer& one = peers_[i - 1];
            if (one.address == mine) {
                Log("dropping the neighbour recorded on CEC address %u, which "
                    "is this PC's own address, so it can never answer", mine);
                peers_.erase(peers_.begin() + static_cast<long>(i - 1));
                continue;
            }
            // Every PC running this app, awake or not. Requiring an answer
            // here undid the whole point of the mask: a sleeping machine
            // answers no poll, which is the original reason addresses have to
            // be stable per machine, so the one case the mask exists for was
            // the one case it excluded. Seen in the field: the Deck woke onto
            // the Dell's 4, and the Dell, moving off it, took the 8 the Deck
            // had slept on, since the Deck was not answering. With this in
            // place the Dell went to 11 instead. The waking onto 4 was a
            // separate fault, every rebuilt device starting on Playback 1; see
            // CecDevice::set_logicalAddress.
            //
            // A machine that has genuinely gone keeps its address reserved
            // until DropDevicesLongGone forgets it a day later. That is the
            // cheaper mistake: there are four addresses and rarely more than
            // two PCs, and the fallback below gives one up rather than leaving
            // this machine with none.
            if (one.ours) avoid |= 1u << one.address;
        }
        device_->set_avoidAddresses(avoid);
    }

    // Asks one device per round for its name, when one is owed.
    //
    // Another copy of this app is owed its full name. A hello carries the name
    // in what is left of a sixteen-byte frame after our vendor ID, version,
    // verb and port, which is seven characters. The television gets the full
    // fourteen, through the standard <Set OSD Name>, so only the menus of
    // other PCs ever saw the short one - and every PC Windows names for itself
    // starts DESKTOP-, so two new users' machines were both listed as
    // "DESKTOP". Seen on the reset Dell, DESKTOP-ABC1234. That answer is
    // believed only from that address and only if it begins with the seven
    // characters the hello already gave, because a torn frame once renamed the
    // Deck "?" in the menu and that is why names from another copy are
    // otherwise ignored (see NoteDeviceFrame). Only a name of exactly seven
    // may have been cut short.
    //
    // And any other device on an input that has never said what it is called.
    // Every device answers <Give OSD Name>, but the only asking was the bus
    // map, which runs when this PC attaches - so a device plugged in while it
    // was running was listed by its type, "HDMI 2 - Playback device", until
    // the next restart. The Google TV plugged in at 18:43 on 2026-10-05 was
    // named only by the restart at 18:52. Its answer is taken as any name is,
    // at two characters or more, and only a device on an input is asked,
    // since nothing else is listed.
    //
    // Up to three tries per device, one per round and only in a round with no
    // other question, each waiting at most 600ms on a device that answers at
    // once - the AskWhere arithmetic, and cheaper.
    void AskForNames() {
        for (Peer& one : peers_) {
            if (!one.present || one.nameAsks >= 3) continue;
            const bool cutShort = one.ours && one.name.size() == 7;
            const bool unnamed = !one.ours &&
                                 (one.name.empty() || one.nameUnsure) &&
                                 one.where != 0xFFFF && one.where != 0;
            if (!cutShort && !unnamed) continue;
            ++one.nameAsks;
            // Kept, because the answer also passes through NoteDeviceFrame on
            // its way here, which takes the name for itself.
            const std::string before = one.name;
            const std::string full = device_->AskName(one.address, 600);
            if (cutShort) {
                if (full.size() > one.name.size() &&
                    full.compare(0, one.name.size(), one.name) == 0) {
                    Log("the PC on CEC address %u is called '%s'", one.address,
                        full.c_str());
                    one.name = full;
                }
            } else if (full.size() >= 2) {
                if (full != before) {
                    Log("the device on %s is called '%s'",
                        cec::FormatPhysicalAddress(one.where).c_str(),
                        full.c_str());
                }
                one.name = full;
                one.nameUnsure = false;
            }
            return;
        }
    }

    // A device answering for an address that another copy of this app held,
    // which is not one: taken from here for what it is. See StillSaysHello.
    void ForgetItWasAPc(Peer& one) {
        Log("CEC address %u has answered for a minute without a hello, so it is "
            "no longer taken for a PC running this app", one.address);
        one.ours = false;
        one.silentRounds = 0;
        // Its name and port were the PC's, so both are asked again: the name
        // in a later round (see AskForNames), the port by the bus map, which
        // asks any device whose type is not known.
        one.name.clear();
        one.nameUnsure = false;
        one.nameAsks = 0;
        one.type = 0xFF;
        mapWanted_ = true;
    }

    void GreetPeersNow() {
        if (!device_) return;
        const DWORD now = GetTickCount();
        if (lastGreetTick_ && now - lastGreetTick_ < kDeviceCheckMs) return;
        lastGreetTick_ = now ? now : 1;

        // Before the polling below, so a stale entry on this PC's own address
        // is never polled and never reported asleep.
        TidyPeerAddresses();

        // A poll is a single byte and the answer is an acknowledgement, so
        // checking everything known costs almost nothing. This is what tells a
        // device that has been switched off from one that is simply quiet, and
        // it is the only way round: most devices say nothing on their way out.
        for (Peer& one : peers_) {
            // An adapter that reported nothing has said nothing about this
            // device, so what was known stands until a poll gets an answer.
            const cec::PollAnswer answer = device_->Poll(one.address);
            if (answer == cec::PollAnswer::Unknown) continue;
            const bool answered = answer == cec::PollAnswer::Present;
            if (answered) {
                one.heardTick = now;
                if (one.ours && !StillSaysHello(one)) ForgetItWasAPc(one);
            } else {
                if (one.present) {
                    Log("%s is not answering; marked asleep",
                        DescribePeer(one).c_str());
                }
                // Asleep rather than silent: what is counted is rounds in
                // which it answered and said nothing.
                one.silentRounds = 0;
            }
            one.present = answered;
        }

        // One <Give Physical Address> per round, and no more, because each one
        // is a frame out and up to 600ms of waiting for the answer.
        //
        // A device that has never said where it is comes first. Without that, a
        // device already on the bus when this service started, which says
        // nothing unprompted, stays unlistable for as long as it never claims
        // the screen.
        //
        // When they have all answered, the question goes to the next device in
        // turn instead of not being asked at all. A device that is moved to a
        // different HDMI port keeps the port it reported before until something
        // contradicts it, and the only thing that normally does is the device
        // announcing itself again - which it does on being plugged in, but
        // which cannot be relied on, and never happens at all for a device
        // moved while this PC was asleep. Round-robin, so the list repairs
        // itself within ten seconds per device rather than staying wrong until
        // a restart, at no more cost than the question that was already being
        // asked. ClaimPort then takes the port off whoever held it before,
        // which is the case this pairs with: a soundbar moved onto the port an
        // Apple TV used to be on.
        Peer* ask = nullptr;
        for (Peer& one : peers_) {
            if (one.present && one.where == 0xFFFF) {
                ask = &one;
                break;
            }
        }
        // The round-robin refresh runs a minute apart, not every round, and the
        // reason is the cost of the question rather than the cost of the answer.
        // AskWhere returns the moment a device replies, but waits out its whole
        // 600ms when one does not - with the CEC lock held, which is the lock a
        // keypress needs. A device that acknowledges polls and ignores
        // <Give Physical Address> is entirely possible, and at every round that
        // would be 600ms of held lock every ten seconds for as long as it was
        // plugged in. Once a minute makes the worst case 600ms a minute, and a
        // port that somebody physically moved still corrects itself within a
        // few minutes, which is far quicker than anyone can walk round a room.
        // The unknown-port question above keeps its every-round cadence: that
        // one is transient, and a device missing from the menu is worse.
        if (!ask && !peers_.empty() &&
            (!lastPortRefreshTick_ ||
             now - lastPortRefreshTick_ >= kPortRefreshMs)) {
            for (size_t tried = 0; tried < peers_.size(); ++tried) {
                Peer& one = peers_[portCursor_ % peers_.size()];
                ++portCursor_;
                if (!one.present || one.ours) continue;   // a hello says theirs
                ask = &one;
                lastPortRefreshTick_ = now ? now : 1;
                break;
            }
        }
        if (ask) {
            const uint16_t where = device_->AskWhere(ask->address, 600);
            if (where != 0xFFFF && cec::PlausiblePhysicalAddress(where) &&
                where != ask->where) {
                Log("the device on CEC address %u is on %s", ask->address,
                    cec::FormatPhysicalAddress(where).c_str());
                ClaimPort(*ask, where);
            }
        } else {
            AskForNames();
        }

        // And one hello for any other copy of this app, which answers with its
        // name and port. A PC that has just woken is known from this within
        // ten seconds even if its own announcement went astray.
        device_->SayHello(cec::kLogicalAddressBroadcast);
        DropDevicesLongGone();
    }

    // Wanted after an attach or an address change, and done here, on the pump,
    // where the link is known to be idle. Doing it inside the attach would put
    // a second onto every wake for something nobody is waiting on.
    void MapTheBusIfWanted() {
        if (!mapWanted_ || !device_) return;
        mapWanted_ = false;
        MapTheBus();
    }

    // Everything a device says about itself goes here. One entry per CEC
    // address, since that is who is speaking; the HDMI port is a field, because
    // it is what the menu shows and what a switch is addressed to.
    //
    // Called from the receive path, under the lock the reader already holds.
    void NoteDeviceFrame(const std::vector<uint8_t>& frame) {
        if (frame.size() < 2 || !device_) return;
        const uint8_t src = static_cast<uint8_t>(frame[0] >> 4);
        // The television is not an input, and a frame from this device's own
        // address is a repeat of something it sent - see the clash detector,
        // which is the one thing that cares about those.
        if (src == cec::kLogicalAddressTv || src == cec::kLogicalAddressBroadcast ||
            src == device_->logicalAddress()) {
            return;
        }

        Peer& one = DeviceAt(src);
        one.heardTick = GetTickCount();
        one.present = true;

        switch (frame[1]) {
            case 0x84:   // <Report Physical Address> [address][device type]
                // Exactly two operands and a shape the HDMI tree can produce.
                // This accepted five bytes or more, and a torn frame holding
                // 0x43 where the high byte belongs became 4.3.0.0, which the
                // menu offered as "HDMI 4 port 3" beside the PC genuinely on
                // HDMI 4 - so one machine appeared twice and neither entry
                // could be switched to.
                //
                // Never for another copy of this app, whose port comes from its
                // hello every ten seconds. On 2026-10-06 the Deck took a torn
                // frame with DESK's address in front of the Chromecast's
                // report, and moved DESK onto the Chromecast's HDMI 2. See
                // NotePeer.
                if (frame.size() == 5 && !one.ours) {
                    const uint16_t where =
                        static_cast<uint16_t>((frame[2] << 8) | frame[3]);
                    if (cec::PlausiblePhysicalAddress(where)) {
                        ClaimPort(one, where);
                        one.type = frame[4];
                    }
                }
                break;
            case 0x82:   // <Active Source> [path]
            case 0x81:   // <Routing Information> [path]
                // A device claiming the screen says where it is, and that is
                // the same answer <Report Physical Address> gives. Taken from
                // here as well because that one is announced when a device
                // joins the bus, and a device already plugged in when this
                // service started may not say it again for hours - which left
                // a Chromecast the other machine could see missing from this
                // one's list entirely, since an entry with no port is not an
                // input anybody can switch to.
                if (frame.size() == 4) {
                    const uint16_t where =
                        static_cast<uint16_t>((frame[2] << 8) | frame[3]);
                    if (cec::PlausiblePhysicalAddress(where)) {
                        // Not for another copy of this app; as above.
                        if (one.where == 0xFFFF && !one.ours) {
                            ClaimPort(one, where);
                        }
                        // It has taken the screen, so whatever it ignored
                        // before, it is not a device that never switches. The
                        // one piece of evidence that can clear the count, and
                        // it arrives whether the request came from here, from
                        // another PC, or from the device's own remote.
                        if (one.where == where) one.switchIgnored = 0;
                    }
                }
                break;
            case 0x47: {  // <Set OSD Name>, which is a device naming itself
                // This message has no fixed length, so a torn frame cannot be
                // told from a real one by size, and the old code cleared the
                // name before reading the new one - so a single stray printable
                // byte replaced a good name with itself. A field log has the
                // Steam Deck going from 'STEAMDE' to '?' that way, and the
                // menu then offered an input called "?".
                //
                // Two defences. A name must have at least two characters to be
                // worth having, and another copy of this app is never renamed
                // from here at all: its hello carries the name, that message is
                // guarded by our own vendor ID and protocol version, and it
                // arrives every ten seconds, so it is both safer and fresher.
                if (one.ours) break;
                std::string name;
                for (size_t i = 2; i < frame.size() && name.size() < 14; ++i) {
                    if (frame[i] >= 0x20 && frame[i] <= 0x7E) {
                        name.push_back(static_cast<char>(frame[i]));
                    }
                }
                if (name.size() >= 2) {
                    one.name = name;
                    one.nameUnsure = false;   // said by the device itself
                }
                break;
            }
            case 0x87:   // <Device Vendor ID> [three bytes of OUI]
                if (frame.size() == 5) {
                    one.vendor = static_cast<uint32_t>(
                        (frame[2] << 16) | (frame[3] << 8) | frame[4]);
                }
                break;
            default:
                break;
        }
    }

    // Throws away everything learnt about the bus when this is a different
    // television from the one it was learnt on.
    //
    // A peer is a device on one television's HDMI inputs, and moving the PC to
    // another set makes every one of them meaningless. Nothing used to say so.
    // ListDevices deliberately keeps a device that has stopped answering, so a
    // sleeping one can still be chosen and woken by switching to it, and
    // DropDevicesLongGone only forgets after a day - so a laptop carried from
    // one television to another went on offering "HDMI 3 - Apple TV" with a
    // sleeping crescent on a set the Apple TV was not plugged into, and would
    // have done for twenty-four hours. Reported from the field.
    //
    // The log was not wrong about it, which is worth knowing: LogTheBus prints
    // only devices that are answering, so it said "other devices on the bus:
    // none" while the stale entry sat in the list the menu draws from.
    //
    // Keyed on the television's identity rather than its physical address,
    // because a cable moved between ports on the same set keeps the same bus
    // and the same neighbours. A set that will not identify itself leaves the
    // key empty, and then nothing can be concluded and nothing is thrown away.
    void ForgetPeersIfTvChanged() {
        if (!device_) return;
        const std::string& key = device_->identity().key;
        if (key.empty() || key == peersForTv_) return;
        if (!peersForTv_.empty() && !peers_.empty()) {
            Log("this is a different television (%s, was %s); forgetting %zu "
                "device(s) learnt from the old one", key.c_str(),
                peersForTv_.c_str(), peers_.size());
        }
        peers_.clear();
        peersForTv_ = key;
        // Recomputed from the now-empty list, so the addresses this PC was
        // avoiding for the old set's neighbours are not avoided on the new one.
        TidyPeerAddresses();
        // Everything the device concluded about the old set, not only the list
        // of its neighbours: where volume goes, whether levels are accepted,
        // what was on screen, what the set said about routing. See
        // ForgetTelevision for what is deliberately kept.
        device_->ForgetTelevision();
        g_onScreenPath.store(-1);
        // This service cannot be the one that turned off a television it has
        // only just met. Left set, the first time the screen came back on this
        // PC would decide it had turned this set off earlier and take it upon
        // itself to turn it back on.
        SetDisplayOwnsStandby(false);
        // Paired with counters inside the device that ForgetTelevision reset.
        // Left alone, "the count has not changed" would be true against a
        // counter that restarted, and the first answer from the new set would
        // be swallowed.
        publishedAudioCount_ = 0;
        // Ask the new bus about a soundbar at the next opportunity rather than
        // waiting out the probe interval.
        lastAudioProbeTick_ = 0;
        // Everything the menu draws is now wrong until the pump next runs, and
        // the pump stands aside while anything is happening. This is cheap and
        // holds no lock the caller does not already hold.
        RefreshSnapshot();
    }

    // Gives a device its HDMI port, and takes that port off anybody else.
    //
    // A physical address is a socket on the television, so exactly one device
    // can be at it. Two entries claiming one port is not a disagreement to be
    // displayed, it is a stale entry, and ListDevices deduplicates them - so the
    // real device simply vanished from the menu. Caught in the field: a Steam
    // Deck listed an Apple TV and a Dell both on 4.0.0.0, and the Apple TV was
    // missing from Switch Input entirely while the Dell showed it correctly.
    //
    // The loser is cleared rather than deleted, because the device is probably
    // still there and only its port is now in doubt. An entry with no port is
    // what the ten-second round asks <Give Physical Address> about, so it comes
    // back by itself within a few seconds and names its real port.
    //
    // A port held by another copy of this app is not taken away. That claim came
    // from a hello carrying our own vendor ID and protocol version, from an
    // address that answered a hello sent to it in return (see NotePeer), so it
    // outranks anything inferred from a bare opcode.
    void ClaimPort(Peer& one, uint16_t where) {
        if (one.where == where) return;
        for (Peer& other : peers_) {
            if (&other == &one || other.where != where) continue;
            // Both sides named by CEC address, never by DescribePeer, which
            // names a device by its port - and the port is the one thing these
            // two have in common. The first version of this line read "the
            // device on CEC address 8 has taken 2.0.0.0 from 2.0.0.0", which
            // says nothing at all about who lost it.
            // Protected only while that PC is answering. A port is a socket
            // on the television and holds one device, so another device that
            // keeps saying it is on the port of a PC that has stopped
            // answering means the PC has gone from it - unplugged, or moved -
            // not that it is asleep there. Caught in the field: the Deck said
            // goodbye at 16:13, its cable was pulled and a Google TV plugged
            // into the same port, and the Google TV's correct answer was
            // refused every ten seconds while the menu went on offering a
            // sleeping Deck on HDMI 2, for what would have been a day.
            //
            // Believed on its second saying, five seconds or more after the
            // first, because one frame is seen twice here when it answers a
            // question. Not because a torn frame cannot repeat: the Deck's log
            // of 2026-10-06 has the same torn hello every three minutes. What
            // keeps a torn frame out of this case is that its payload is a
            // real frame's, and the only device that sends this port in a
            // <Report Physical Address> or an <Active Source> is the one on
            // it - here, a PC that has stopped answering and says nothing.
            if (other.ours && !one.ours) {
                if (other.present) {
                    Log("CEC address %u also claims %s, which CEC address %u "
                        "holds as a PC running this app; ignored", one.address,
                        cec::FormatPhysicalAddress(where).c_str(),
                        other.address);
                    one.contestTick = 0;
                    return;
                }
                const DWORD now = GetTickCount();
                if (one.contestPort != where || !one.contestTick) {
                    one.contestPort = where;
                    one.contestTick = now | 1;
                    Log("CEC address %u says it is on %s, which the PC on CEC "
                        "address %u held before it stopped answering; believed "
                        "if it says so again", one.address,
                        cec::FormatPhysicalAddress(where).c_str(),
                        other.address);
                    return;
                }
                // Signed, since the stored tick may be one ahead of now.
                if (static_cast<LONG>(now - one.contestTick) <
                    static_cast<LONG>(kPortContestMs)) {
                    return;
                }
            }
            Log("%s belongs to CEC address %u now, so CEC address %u has no "
                "port until it says where it is",
                cec::FormatPhysicalAddress(where).c_str(), one.address,
                other.address);
            other.where = 0xFFFF;
        }
        // A device that turns up on another input may be another device. CEC
        // addresses are handed round as devices come and go, and a Roku taking
        // the address a Fire TV stick had left would otherwise be listed under
        // the stick's name on the Roku's port for good. So its name is asked
        // again (see AskForNames), and kept meanwhile: the commoner case is
        // the same device moved, which answers the same, and a soundbar moved
        // between ports should not be listed as "Audio system" while it is
        // asked. Never another copy of this app, whose hello names it.
        if (!one.ours && one.where != 0xFFFF && one.where != where) {
            one.nameUnsure = true;
            one.nameAsks = 0;
        }
        one.where = where;
        one.contestTick = 0;
    }

    // The entry for a CEC address, made if this is the first word from it.
    Peer& DeviceAt(uint8_t address) {
        for (Peer& one : peers_) {
            if (one.address == address) return one;
        }
        Peer made;
        made.address = address;
        peers_.push_back(made);
        return peers_.back();
    }

    // Another copy of this app has said where it is, either because it has just
    // started and announced itself or because it answered a hello. Both carry
    // the name and the port, so nothing further is asked and the list is right
    // within a frame of a PC waking. Called from the receive path, under the
    // lock the pump already holds.
    void NotePeer(uint8_t address, uint16_t where, const std::string& name,
                  bool answered) {
        // The television repeats things, and a repeat of this PC's own hello
        // came back looking like a PC at address 0. Only the playback addresses
        // are ever another copy of this app, and never this one.
        if (!device_ || address == device_->logicalAddress()) return;
        // The four this app can hold. Free Use has to be here as well as in
        // the allocator, or a PC that fell back to it would announce itself,
        // be heard, and be dropped on this line - so the machine that could
        // not get a proper address would also be the one nobody could see.
        if (address != cec::kLogicalAddressPlayback1 &&
            address != cec::kLogicalAddressPlayback2 &&
            address != cec::kLogicalAddressPlayback3 &&
            address != cec::kLogicalAddressFreeUse) {
            return;
        }
        Peer& one = DeviceAt(address);
        const bool first = !one.ours;
        // An address not yet known as a PC running this app is asked directly
        // before it is believed to be one, and only its answer, a Here I Am
        // from that address with the same port, makes it one.
        //
        // The vendor ID and protocol version prove that the payload of a hello
        // came from a copy of this app. They prove nothing about the address
        // in front of it, and the adapter's receive buffer tears frames: a
        // short frame, such as a one-byte poll, can arrive with the previous
        // frame's payload behind its own header. Caught in the field on
        // 2026-10-06. The desktop, DESK, was on CEC address 4 and 1.0.0.0, a
        // Chromecast on 11 and 2.0.0.0, and the Deck received a hello from 11
        // carrying DESK's port and name. The Chromecast's entry became a PC
        // called DESK, the port protection then defended that, the next torn
        // frame went the other way and put address 4 on the Chromecast's
        // 2.0.0.0, and the Deck's menu showed DESK on HDMI 1 and HDMI 2 with
        // no Chromecast - for as long as the service ran, since nothing turns
        // a PC running this app back into a stranger. The same torn hello came
        // every three minutes, so waiting for it to be said again proves
        // nothing; asking proves where it came from, because only the address
        // asked can answer, and the Chromecast does not speak this protocol.
        //
        // The cost to a real PC is the frames of one exchange, well under a
        // second on a quiet bus, and nothing is lost if its answer is late:
        // its next hello, within ten seconds, asks again.
        if (!one.ours) {
            const DWORD now = GetTickCount();
            // Signed, since the stored tick may be one ahead of now.
            const bool asked = one.helloTick && one.helloPort == where &&
                               static_cast<LONG>(now - one.helloTick) <
                                   static_cast<LONG>(kPeerConfirmMs);
            if (!answered || !asked) {
                if (!asked) {
                    one.helloPort = where;
                    one.helloTick = now | 1;
                    device_->SayHello(address);
                }
                return;
            }
        }
        one.helloTick = 0;
        // Said before the entry is overwritten, because this is where a logical
        // address changes hands and the old occupant's details disappear. The
        // Apple TV and a PC have both held address 11 on the same bus, and
        // without this line the swap left no trace at all.
        if (!one.ours && one.where != 0xFFFF && one.where != where) {
            Log("CEC address %u was %s and is now a PC running this app on %s",
                address, DescribePeer(one).c_str(),
                cec::FormatPhysicalAddress(where).c_str());
        }
        one.ours = true;   // before ClaimPort, so its hello outranks the rest
        ClaimPort(one, where);
        // The hello has room for seven characters of the name, so a longer
        // name learnt by asking is kept for as long as it still begins with
        // what the hello says. A different beginning is a renamed PC, or a
        // different one on this address, and is taken as it comes and asked
        // about again. See AskForNames.
        if (one.name.size() < name.size() ||
            one.name.compare(0, name.size(), name) != 0) {
            one.name = name;
            one.nameAsks = 0;
        }
        one.type = 4;   // a PC announces itself as a playback device
        one.ours = true;
        one.present = true;
        one.heardTick = GetTickCount();
        one.helloHeard = true;   // see StillSaysHello
        if (first) {
            Log("another PC running this app is on CEC address %u, %s, called "
                "'%s'", address, cec::FormatPhysicalAddress(where).c_str(),
                name.c_str());
        }
    }

    // One has said goodbye, which it does on its way to sleep. Dropped at once
    // rather than left to be discovered missing, so the menu never offers a PC
    // that has just gone.
    void ForgetPeer(uint8_t address) {
        for (Peer& one : peers_) {
            if (one.address != address || !one.ours) continue;
            Log("the PC on CEC address %u has gone to sleep", address);
            one.present = false;
            return;
        }
    }

    // Who else is on this television's CEC bus, and which of them are other
    // copies of this app.
    //
    // Polling all sixteen addresses is what every real CEC stack does at
    // startup and this one never did, which is why a second PC only ever
    // showed up as confusion. It costs about a second of an idle link, runs
    // from the pump so it can never land between a key press and its release,
    // and is redone when something has changed rather than on a timer.
    //
    // The name and port come from the devices themselves. A device that
    // answers the private hello is another copy of this app, and only those can
    // be asked to take the screen.
    void MapTheBus() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return;

        // Every address polled, and anything that answers asked what it is.
        // Only asked for what is not known already, since a device that has
        // announced itself has told us the same thing for free.
        for (uint8_t address = 1; address <= 14; ++address) {
            if (address == device_->logicalAddress()) continue;
            // Only an acknowledgement is a device, and only a refusal says one
            // is gone. A poll the adapter reported nothing about is neither.
            const cec::PollAnswer answer = device_->Poll(address);
            if (answer != cec::PollAnswer::Present) {
                if (answer == cec::PollAnswer::Absent) {
                    for (Peer& one : peers_) {
                        if (one.address == address) one.present = false;
                    }
                }
                continue;
            }
            Peer& one = DeviceAt(address);
            one.present = true;
            one.heardTick = GetTickCount();
            if (one.where == 0xFFFF || one.type == 0xFF) {
                // Through ClaimPort and the plausibility test, like every other
                // place a port is learnt. This was the one assignment that went
                // straight to the field, so a bus sweep could put two devices
                // on one port again - which is not a visible disagreement but a
                // device vanishing, since ListDevices deduplicates them. It is
                // also the one path that asks an address nothing has been heard
                // from, so it is the most likely to be answered by something
                // that has moved.
                const uint16_t where = device_->AskWhere(address, 1000);
                if (cec::PlausiblePhysicalAddress(where)) ClaimPort(one, where);
            }
            if (one.name.empty()) {
                const std::string name = device_->AskName(address, 1000);
                if (!name.empty()) one.name = name;
            }
            // Asked once, and only because everything else is being asked
            // anyway. Nothing displays it: it goes in the log so that a device
            // which never says its name can be identified from its maker if it
            // ever comes to that.
            if (!one.vendor) device_->AskVendor(address, 800);
            if (!one.ours && device_->AskIfPeer(address, 1200)) {
                one.ours = true;
                one.helloHeard = true;   // its answer is this round's hello
            }
        }

        DropDevicesLongGone();
        LogTheBus();
    }

    // One line naming everything on the bus, after a sweep.
    void LogTheBus() {
        std::string summary;
        for (const Peer& one : peers_) {
            if (!one.present) continue;
            char entry[96];
            snprintf(entry, sizeof(entry), " %s=%s[%s]%s",
                     cec::FormatPhysicalAddress(one.where).c_str(),
                     one.name.empty() ? "?" : one.name.c_str(),
                     DeviceTypeName(one.type), one.ours ? "(this app)" : "");
            summary += entry;
            if (one.vendor) {
                char maker[24];
                snprintf(maker, sizeof(maker), "(vendor %06X)", one.vendor);
                summary += maker;
            }
        }
        Log("other devices on the bus:%s",
            summary.empty() ? " none" : summary.c_str());
    }

    // A device that has not been heard from for a day is gone rather than
    // asleep, so it stops being offered. Anything shorter would drop a console
    // that is off overnight, which is exactly what someone wants to switch to
    // in the morning - switching to a sleeping device is how it gets woken.
    void DropDevicesLongGone() {
        for (size_t i = peers_.size(); i > 0; --i) {
            const Peer& one = peers_[i - 1];
            if (one.present || GetTickCount() - one.heardTick < kForgetDeviceMs) {
                continue;
            }
            Log("%s has not been heard from in a day; forgotten",
                DescribePeer(one).c_str());
            peers_.erase(peers_.begin() + static_cast<long>(i - 1));
        }
    }

    // What the "Switch Input" menu shows: everything on an HDMI input of this
    // television, by port. Called under the CEC lock, for the snapshot.
    //
    // Answered from the list the ten-second check keeps current, so the menu
    // opens at once. Devices with no port are left out - the television itself,
    // and a soundbar that sits on the HDMI-ARC connection rather than an input
    // - because there is nothing to switch to.
    std::vector<Peer> ListDevices() {
        std::vector<Peer> shown;
        if (!device_) return shown;

        // This PC first, since it is always on an input and never announces
        // itself to itself.
        Peer self;
        self.address = device_->logicalAddress();
        self.where = device_->physicalAddress();
        self.type = 4;
        self.name = device_->osdName();
        self.present = true;
        shown.push_back(self);

        for (const Peer& one : peers_) {
            if (one.where == 0xFFFF || one.where == 0) continue;
            if (one.where == self.where) continue;   // a stale entry for us
            // One line per port. A device that restarts can come back on a
            // different CEC address, and both entries would otherwise be shown
            // until the older aged out. Seen in a log, from the laptop and the
            // desktop trading logical addresses while a cable was moved about:
            // "2.0.0.0=DELL-54 2.0.0.0=DELL-54", which this is what keeps out
            // of the menu.
            //
            // Compared by subtraction, the way every other tick comparison here
            // is. GetTickCount wraps after 49.7 days, and a plain > across that
            // moment picks the older of the two - which on this line is the
            // device that has gone rather than the one that is there.
            bool already = false;
            for (Peer& listed : shown) {
                if (listed.where != one.where) continue;
                already = true;
                if (static_cast<LONG>(one.heardTick - listed.heardTick) > 0) {
                    listed = one;
                }
            }
            if (!already) shown.push_back(one);
        }

        // By HDMI port, so every PC shows the same order. Insertion order put
        // whichever machine was asked first at the top, and the two disagreed.
        for (size_t i = 1; i < shown.size(); ++i) {
            for (size_t j = i; j > 0 && shown[j].where < shown[j - 1].where; --j) {
                std::swap(shown[j], shown[j - 1]);
            }
        }
        return shown;
    }

    // Puts a given HDMI port on screen, by whatever route works.
    //
    // The standard one first, which is how a television switches input at all:
    // it passes the request to the bus and the device named claims the screen
    // itself. That reaches anything with CEC turned on, including another PC
    // running this app, which answers it like a Chromecast does.
    //
    // Returns the pipe's answer, since the person who chose a menu item is
    // owed one either way.
    std::string SwitchTo(uint16_t port) {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return "ERR not attached to a CEC port";

        // This PC is a special case with no message involved: it claims the
        // screen for itself, which is what every wake does.
        if (port == device_->physicalAddress()) {
            Log("switching the TV to this PC");
            // Wanting the screen is the opposite of handing it over, so
            // anything asking this PC to take it is welcome from here.
            handoverUntilTick_ = 0;
            // And it turns the television on, which ends any <Standby> this
            // service was still holding (hard rule 18).
            SetDisplayOwnsStandby(false);
            device_->WakeAndClaimInput(true);
            // Brought up to date here, under the lock this already holds, so
            // the answer is current the instant this returns rather than on the
            // pump's next tick. The tray asks the moment a command finishes.
            RefreshSnapshot();
            GoHot();
            return "OK";
        }

        // A television that is off cannot show anything, and a request sent to
        // one goes nowhere. Turning it on first is what a person would do.
        if (device_->QueryPowerStatus(1200) == cec::PowerStatus::Standby) {
            Log("switching input: the TV is off, turning it on first");
            // It turns the television on, which ends any <Standby> this
            // service was holding (hard rule 18). The one turn-on path left
            // without it: a switch asked for from the command line while this
            // service had the set off, after which the next screen-off would
            // have read "already off" and left the set showing that input.
            SetDisplayOwnsStandby(false);
            device_->SendImageViewOn();
            Sleep(kInputSettleMs);
        }

        const std::string where = cec::FormatPhysicalAddress(port);
        const DWORD start = GetTickCount();

        // One request, and then listening.
        //
        // This used to send the request, wait a flat 1.2 seconds whatever
        // happened, ask the whole bus who was on screen, and if the answer was
        // not yet the right one, send the request again. Three things were
        // wrong with that. The fixed wait threw away the fact that the answer
        // usually arrives in a few hundred milliseconds. The question it then
        // asked - <Request Active Source> - makes every device on the bus
        // re-announce, which is noise at the exact moment the bus is busy. And
        // the second request, sent while the first was still being acted on,
        // is what a second copy of this app answered by claiming the screen
        // back, so the two of them pulled the television between them.
        //
        // Now the request goes once and the confirmation is simply heard: the
        // device that takes the screen broadcasts <Active Source> because the
        // standard says it must, and that frame was arriving anyway.

        // Armed before the waiting starts, not just before the request: a set
        // routing to this PC while this is waiting is the same echo, and
        // claiming the screen then would undo the switch being asked for.
        handoverUntilTick_ = GetTickCount() + kHandoverQuietMs;

        for (int attempt = 0; attempt < 2; ++attempt) {
            const DWORD since = GetTickCount() - lastInputRequestTick_;
            if (lastInputRequestTick_ && since < kInputSettleMs) {
                // A set busy changing input loses what arrives meanwhile, so
                // one request is seen through before the next begins.
                Sleep(kInputSettleMs - since);
            }
            // And not while the set is mid-route, whoever started it. See
            // kTvRouteQuietMs - this is the difference between the click that
            // worked and the one that did not.
            device_->WaitForTvRouteQuiet(kTvRouteQuietMs, kTvRouteWaitMs);

            const DWORD asked = GetTickCount();
            lastInputRequestTick_ = asked;
            handoverUntilTick_ = asked + kHandoverQuietMs;
            device_->SendSetStreamPath(port);
            if (!device_->AwaitActivePath(port, kClaimWaitMs)) {
                // Nothing claimed it. One more request, because a set mid-resync
                // loses what arrives during it and the rate limit above spaces
                // the two far enough apart for that to be over.
                continue;
            }
            // The input says it has the screen, which is all CEC asks of it.
            // Whether the picture moved is the television's to say, and this
            // app has no way to see the picture - so the set's own word is the
            // best evidence there is, and its silence is not proof either way.
            handoverUntilTick_ = GetTickCount() + kHandoverTailMs;
            if (device_->TvNamedPath(port, asked) ||
                !device_->TvAnnouncesRouting()) {
                // Either the set said so, or it is one of the sets that never
                // says anything about routing and simply follows the claim.
                // See TvAnnouncesRouting.
                Log("the TV is now showing %s (%lums)", where.c_str(),
                    GetTickCount() - start);
            } else {
                Log("%s claimed the screen in %lums, but this TV usually says "
                    "which input it is showing and did not", where.c_str(),
                    GetTickCount() - start);
            }
            RefreshSnapshot();
            GoHot();
            return "OK";
        }

        // Neither request was answered by a claim. Now the bus is worth asking,
        // once: a device that took the screen without announcing it is not
        // following the standard, but it is on screen, and asking is the only
        // way to find that out. <Request Active Source> makes every device
        // re-announce, which is why it waits until there is nothing else left
        // to learn.
        device_->AskWhoIsOnScreen(1000);
        // activePath() only, never what was merely asked for: the television
        // echoes the request, and reading the echo as an answer reported a
        // switch that had not happened and moved the dot to an input nothing
        // had claimed.
        if (device_->activePathKnown() && device_->activePath() == port) {
            Log("the TV is showing %s, which it did not announce (%lums)",
                where.c_str(), GetTickCount() - start);
            handoverUntilTick_ = GetTickCount() + kHandoverTailMs;
            RefreshSnapshot();
            GoHot();
            return "OK";
        }

        // Nothing took the screen, so nothing was handed over and this PC has
        // no reason to stay out of the way.
        handoverUntilTick_ = 0;

        // Another copy of this app can be asked directly, which answers yes or
        // no rather than leaving this to be inferred. Only reached when the
        // standard route did not take, so it costs nothing in the normal case.
        for (const Peer& one : peers_) {
            if (one.where != port || !one.ours) continue;
            // Not a PC that is asleep. It left the bus on its way down, so it
            // cannot hear the question, and asking costs three tries of a
            // second and a half each with this lock held - the volume buttons
            // waiting behind a question with no possible answer.
            if (!one.present) {
                Log("the PC on %s is asleep, so it cannot take the screen",
                    where.c_str());
                return "ERR that PC is asleep";
            }
            if (device_->AskPeerToTakeScreen(one.address, 1500)) {
                Log("the PC on %s took the screen when asked directly",
                    where.c_str());
                return "OK";
            }
            Log("the PC on %s did not take the screen; its own screen is "
                "probably off", where.c_str());
            return "ERR that PC has no picture to show";
        }

        // Two different failures, and telling them apart is the whole point.
        //
        // An empty port, a device with CEC switched off, and a sleeping PC all
        // look the same from here: silence. But a device that answered a poll
        // during this very exchange is none of those, and saying "nothing there
        // answered" about it is simply false - it is there, it is listening, and
        // it threw the request away.
        //
        // Measured on an Apple TV, 2026-10-01, with a Steam Deck on the same
        // bus as the control. Two requests to 3.0.0.0 got nothing while polls
        // to its logical address were acknowledged before, during and after;
        // the identical request to 2.0.0.0 was answered in 609ms; and the
        // Apple TV then broadcast <Active Source> 3.0.0.0 of its own accord
        // the moment its remote was pressed. So it transmits, we hear it, the
        // address is right, and it ignores <Set Stream Path> by design. tvOS
        // implements a narrow CEC subset and this is inside it. Nothing this
        // app can send will switch to it, which makes the message the only
        // thing left to get right.
        for (Peer& one : peers_) {
            if (one.where != port || !one.present) continue;
            if (one.switchIgnored < 0xFF) ++one.switchIgnored;
            Log("%s answered a poll but ignored the request to take the "
                "screen (%u times); some devices only take it from their own "
                "remote", where.c_str(), one.switchIgnored);
            RefreshSnapshot();
            return "ERR that device ignores switch requests";
        }
        Log("the TV did not switch to %s; nothing there answered", where.c_str());
        return "ERR nothing on that input answered";
    }

    // The other side of the same exchange. Refused when this PC has no picture
    // to show, since taking the screen then means handing the viewer a blank
    // input and taking the television away from whatever they were watching.
    bool TakeScreenForPeer() {
        if (!device_ || DisplayStillOff() || PowerDownStarted()) {
            Log("asked to put this PC on screen; refused, it has no picture to "
                "show");
            return false;
        }
        // Not while this PC is handing the screen to something else. The
        // request is then the echo of its own handover coming back round the
        // bus, and taking the screen would undo what the person chose. See
        // kHandoverQuietMs.
        if (handoverUntilTick_ &&
            static_cast<LONG>(handoverUntilTick_ - GetTickCount()) > 0) {
            Log("asked to put this PC on screen; ignored, this PC is in the "
                "middle of handing the screen to another input");
            return false;
        }
        Log("asked to put this PC on screen; claiming it");
        SetDisplayOwnsStandby(false);   // it turns the TV on: hard rule 18
        const bool ok = device_->WakeAndClaimInput();
        RefreshSnapshot();
        GoHot();
        return ok;
    }

    // Says out loud when the bus is refusing broadcasts, once per spell.
    //
    // Nothing here can fix it: the refusal comes from another device that has
    // stopped collecting its messages, usually an adapter left powered by a PC
    // that is asleep. But the symptoms point everywhere except at the cause -
    // the television answers questions, volume works, and only power commands
    // fail, because <Report Physical Address> and <Active Source> are the
    // broadcasts being lost and a set will not take power commands from a
    // device it cannot place.
    void WarnIfBusRefusesBroadcasts() {
        if (!device_) return;
        // Tried again while it lasts, rather than waiting for the next routine
        // refresh to happen along. The first time this was seen, the bus came
        // back after ninety seconds of the television refusing everything, and
        // the only reason it came back at all was a topology refresh falling
        // due. An announce is three frames; every fifteen seconds while a bus
        // is broken is nothing, and it stops as soon as one is accepted.
        if (device_->broadcastsRefused() >= 6 && !DisplayStillOff() &&
            (!lastAnnounceTick_ || GetTickCount() - lastAnnounceTick_ >= 15000)) {
            device_->Announce(device_->physicalAddress());
            lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
        }
        const bool refusing = device_->broadcastsRefused() >= 6;
        if (refusing == warnedBusRefusing_) return;
        warnedBusRefusing_ = refusing;
        if (refusing) {
            Log("the CEC bus is refusing broadcasts, so the TV cannot learn "
                "where this PC is and will refuse power commands; another "
                "device on the bus is not collecting its messages, most often "
                "an adapter left plugged into a PC that is asleep");
        } else {
            Log("the CEC bus is taking broadcasts again");
        }
    }

    // Says out loud when the adapter hands over a frame that cannot exist -
    // one whose operand count its opcode does not allow.
    //
    // Nothing here can fix it either, and it is ignored where it is noticed, in
    // NoteRouting. But it means the receive buffer was read across two frames,
    // and if that ever becomes common it is worth knowing rather than
    // discovering from a television that switched somewhere nobody asked for.
    // Said once per ten, so a bad spell is a line or two and not a flood.
    void WarnIfFramesMalformed() {
        if (!device_) return;
        // A counter that has gone backwards is a new device, not a repaired
        // one: AcquireDevice builds a fresh CecDevice and its counters start at
        // zero. Comparing against the old high-water mark then suppressed these
        // warnings entirely until the new device had caught up with a number it
        // knew nothing about - so the one diagnostic for a flaky adapter went
        // quiet exactly when the adapter had just been rebuilt because it was
        // flaky.
        const uint32_t seen = device_->malformedFrames();
        if (seen < warnedMalformed_) warnedMalformed_ = 0;
        if (device_->receiveOverflows() < warnedOverflows_) warnedOverflows_ = 0;
        if (seen >= warnedMalformed_ + 10) {
            warnedMalformed_ = seen;
            Log("%u frames so far have arrived with an operand count their "
                "opcode does not allow, and were ignored; the adapter's receive "
                "buffer is being read across two frames", seen);
        }
        const uint32_t over = device_->receiveOverflows();
        if (over >= warnedOverflows_ + 10) {
            warnedOverflows_ = over;
            Log("the adapter has reported a receive overflow %u times, so "
                "frames are arriving faster than they are being collected",
                over);
        }
    }

    // Kept across restarts, and asked for first next time. A television
    // remembers what it learnt about each address: when the desktop restarted
    // while the laptop was asleep it took the laptop's address, announced its
    // own HDMI port, and the TV went on believing that address was the laptop's
    // input - so it ignored every <Standby> from it, as a set does from an
    // input it is not showing. Volume kept working throughout, which is what
    // made it look like the television sulking rather than a name clash.
    void RememberAddress() {
        if (!device_) return;
        // Free Use is a fallback, never a preference. Remembering it would make
        // one crowded moment permanent: this PC would ask for 14 first at every
        // start from then on, keep it whenever it was free, and so stay on the
        // least well supported address on the bus long after the device that
        // crowded it out had gone. The playback addresses are still tried first
        // on the next start, and if the bus really is full it lands back on 14
        // a second later, which is the right way round.
        if (device_->logicalAddress() == cec::kLogicalAddressFreeUse) return;
        WriteNumber(L"LogicalAddress", device_->logicalAddress());
    }

    // Moves this device to a free CEC address when something else turns out to
    // be using the one it holds.
    //
    // Two PCs on one television reach this easily: each picks a free address
    // when it attaches, and a machine that is asleep answers no poll, so its
    // address looks free and the other takes it. What follows is hard to read
    // from the outside, because most things still work - the desktop answered
    // volume and power queries perfectly while every <Standby> was ignored and
    // every wake came back "CEC frame was not acknowledged", which reads as a
    // sulking television rather than two devices wearing one name.
    //
    // Called from the pump, which only runs on an idle link, so this never
    // lands between a keypress and its release. Re-announcing afterwards is
    // what tells the television the new address, and without it the set keeps
    // addressing the old one.
    void MoveIfAddressTaken() {
        if (!device_ || !device_->addressClash()) return;
        // Checked before the flag is cleared, so waiting does not consume the
        // clash that is waiting to be acted on.
        if (addressRetryTick_ &&
            static_cast<LONG>(addressRetryTick_ - GetTickCount()) > 0) {
            return;
        }
        device_->clear_addressClash();

        // Our own address is offered first. Without a preference the list
        // always starts at Playback 1, so a PC sitting on 8 would move to 4 the
        // moment 4 looked free - churn nobody asked for, and with two PCs both
        // reaching for the same address, churn that feeds itself. Asking about
        // our own first also makes the commonest outcome here the first thing
        // tested: by the time a clash is acted on, the other device has usually
        // moved already and this address is free again.
        const uint8_t before = device_->logicalAddress();
        const bool allocated = device_->AllocateLogicalAddress(before);
        const uint8_t after = device_->logicalAddress();

        // The answer first, never the address alone. Both of the interesting
        // outcomes can leave the address looking unchanged or changed for the
        // wrong reason: keeping an address that turned out to be free is a
        // success that reads as "did not move", and failing to restore the
        // address mask after picking a new one is a failure that reads as "did
        // move". The field log showed the first of those as an empty reason
        // printed after "is shared with another device:", because lastError_
        // had been cleared by a call that had succeeded.
        if (!allocated) {
            // One line per run, not one per attempt, or a bus that cannot be
            // probed buries the log in the minute it matters most.
            if (!addressRetries_) {
                Log("CEC address %u is shared with another device: %s; commands "
                    "may be ignored", before, device_->lastError().c_str());
            }
            // Worth repeating only when nothing was actually asked - a bus too
            // busy to carry a probe, or a register write that did not land.
            // "Every address answered" is a fact about the bus that another
            // attempt cannot change. See kAddressRetryGapMs for the leash.
            if (!device_->probedTheBus() &&
                ++addressRetries_ <= kAddressRetryLimit) {
                device_->set_addressClash();
                addressRetryTick_ = (GetTickCount() + kAddressRetryGapMs) | 1;
                return;
            }
            if (addressRetries_ > kAddressRetryLimit) {
                Log("gave up moving off CEC address %u after %d tries; waiting "
                    "for it to be noticed again", before, kAddressRetryLimit);
            }
            addressRetries_ = 0;
            addressRetryTick_ = 0;
            return;
        }

        addressRetries_ = 0;
        addressRetryTick_ = 0;
        if (after == before) {
            Log("CEC address %u is free again; keeping it", before);
            return;
        }
        Log("CEC address %u was taken by another device; moved to %u%s", before,
            after, FreeUseNote(after));
        RememberAddress();
        // This PC has just taken an address, and whoever is recorded on it is
        // now unreachable by definition. Done here rather than left to the next
        // round, because the ten seconds in between is long enough to see a
        // neighbour wrongly reported asleep.
        TidyPeerAddresses();
        mapWanted_ = true;
        device_->Announce(device_->physicalAddress());
        lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
    }

    // Passes on whatever volume news has arrived since last time, or silence
    // once a question has gone unanswered for too long. Called with the lock
    // held, straight after the receive buffer was read.
    //
    // Any news settles the question that is out. A report is the level as it
    // is now, whoever sent it and why, and a soundbar that volunteers one is
    // passed on exactly as an answer would be.
    //
    // Anything other than a real 0-100 level goes to the tray as no level at
    // all, and the panel then shows nothing rather than a guess. Two kinds of
    // nothing, because the tray treats them differently: "OK refused" is a
    // Feature Abort or the 0x7F that means "I decline to say", which a set
    // will keep saying; plain "OK" is silence, which may just have been slow.
    void PublishAudioNews() {
        const cec::CecDevice::AudioNews& news = device_->audioNews();
        if (news.count && news.count != publishedAudioCount_) {
            publishedAudioCount_ = news.count;
            audioAsked_ = false;
            const unsigned level = news.status & 0x7F;
            // Carried on the answer as well as on the status line, because the
            // status line is only asked for on the half-minute watch and when
            // the pointer reaches the tray icon - so the slider went on
            // offering a drag that could not work for several seconds after the
            // set had already said no. An open panel polls the level about once
            // a second, and a refusal bumps the news count, so this is the
            // first thing out of the service after the abort arrives.
            const char* nolevel = news.levelRefused ? " nolevel=1" : "";
            if (news.refused || level > 100) {
                // Only a refusal is remembered per TV. Silence says nothing
                // lasting.
                g_tvStore.NoteReportsVolume(false);
                PublishVolume(std::string("OK refused") + nolevel);
                return;
            }
            g_tvStore.NoteReportsVolume(true);
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "OK vol=%u mute=%d%s", level,
                     (news.status & 0x80) ? 1 : 0, nolevel);
            PublishVolume(buffer);
            return;
        }
        if (audioAsked_ && GetTickCount() - audioAskTick_ >= kAudioStatusTimeoutMs) {
            audioAsked_ = false;
            PublishVolume("OK");
        }
    }

    // Polls for a soundbar or receiver and records where volume should go.
    //
    // Losing one is noticed immediately, because a press it does not
    // acknowledge falls back to the TV and clears the assumption. Gaining one
    // has no such signal: nothing about sending volume to the TV fails when a
    // soundbar quietly reappears, so the only way to find out is to ask.
    void ProbeAudioSystem() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return;
        const bool had = device_->hasAudioSystem();
        device_->DetectAudioSystem();
        lastAudioProbeTick_ = GetTickCount();
        if (device_->hasAudioSystem() != had) {
            Log("volume now goes to the %s",
                device_->hasAudioSystem() ? "audio system" : "TV");
        }
    }

    // Refreshes the topology registration when the link has gone quiet, rather
    // than on a fixed clock. Announcing costs a measured 1.1s, so this is the
    // difference between a responsive button and a visibly slow one.
    //
    // The TV drops this device from its topology after a spell of silence, so
    // the announce cannot be skipped entirely. But every frame sent is traffic
    // on that same link, and a run of volume presses keeps the registration
    // alive by itself. Keying the refresh off the age of the last announce put
    // that 1.1s stall into the middle of continuous use every fifteen seconds;
    // keying it off the age of the last command moves it into the gap where
    // nobody is pressing anything.
    //
    // kMaxAnnounceAgeMs is the backstop for what idle time cannot see: a button
    // held down for minutes is continuous traffic, so it would never look idle
    // and would otherwise never refresh at all.
    // Returns whether it actually announced, which tv-on needs: a claim sent
    // before a refresh is a claim a television may have ignored.
    bool EnsureAnnounced() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return false;

        const DWORD now = GetTickCount();

        // Never mid-hold. This is the one place a stall does real damage: it
        // lands between <User Control Pressed> and its release, and the TV
        // starts auto-repeating a key it believes is stuck down.
        //
        // The time limit is the safety net. If a release is ever lost, a
        // latched flag would block every future refresh, so a hold that has run
        // past the tray's own ceiling is treated as over.
        if (holding_ && now - holdStartTick_ < kAbandonedHoldMs) return false;
        holding_ = false;

        const bool linkIdle =
            !lastTrafficTick_ || now - lastTrafficTick_ >= kIdleBeforeAnnounceMs;
        const bool announceStale =
            !lastAnnounceTick_ || now - lastAnnounceTick_ >= kMaxAnnounceAgeMs;
        if (!linkIdle && !announceStale) return false;

        device_->Announce(device_->physicalAddress());

        // Re-announcing is exactly what prompts a TV to ask who this is, so
        // listen closely for a while afterwards.
        GoHot();

        ProbeAudioSystem();

        // Announcing is itself traffic, so it counts as such - otherwise
        // repeated warm-ups from a hovering pointer would each find an idle
        // link and announce all over again. Timed from completion, not entry.
        lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
        return true;
    }

    // Sends <Give Audio Status> and returns without waiting for the answer.
    //
    // The read this replaced held the CEC lock for the whole question and
    // answer, about 200ms, and a button release arriving meanwhile waited for
    // all of it - long enough for the TV to repeat a key it thought was still
    // down. Here the lock is held only while the question is on the wire. The
    // answer is picked up by the pump, which looks every 20ms while one is due,
    // and passed to the tray the moment it lands. A release can still wait for
    // a frame that is already on the wire, because nothing on a CEC bus can
    // interrupt one, but that is the same wait as for the panel's own re-sent
    // press rather than a new kind of delay.
    //
    // Always a real question. An earlier readout answered from a remembered
    // report instead, and every other poll never reached the television.
    //
    // One question outstanding at a time. A second would put another frame on
    // a bus the first answer is about to need, for news that arrives in the
    // same order either way.
    std::string AskVolume() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_) return "ERR not attached to a CEC port";
        if (audioAsked_ && GetTickCount() - audioAskTick_ < kAudioStatusTimeoutMs) {
            return "OK";
        }
        if (!device_->AskAudioStatus()) return "ERR " + device_->lastError();
        audioAskTick_ = GetTickCount();
        audioAsked_ = true;
        return "OK";
    }

    bool AwaitingVolumeAnswer() const { return audioAsked_.load(); }

    // A fresh keep-awake lease: the first keep-alive goes straight away, which
    // also puts a line in the log to confirm it is working, and a reason for
    // sending nothing is logged again even if it is the same as last time.
    void StartKeepAlive() {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        lastKeepAliveTick_ = 0;
        keepAwakeSkip_ = nullptr;
    }

    // Sends the TV a keep-alive, if now is a moment it is safe to. Called from
    // the pump while the tray's lease is live.
    //
    // Three things have to hold. This PC must be the input on screen, so a TV
    // someone has switched to another device is left to it. The TV must report
    // that it is on, so nothing lands on a set in standby - and <Image View On>,
    // one of the signals, would switch it on. And the link must have been quiet
    // for a few seconds. try_lock, as the pump always does: a command holding
    // the link means this simply comes back on a later tick.
    void KeepTvAwake() {
        const DWORD now = GetTickCount();
        const DWORD last = lastKeepAliveTick_.load();
        if (last && now - last < kKeepAliveIntervalMs) return;
        const DWORD traffic = lastTrafficTick_.load();
        if (traffic && now - traffic < kKeepAliveQuietMs) return;

        std::unique_lock<std::recursive_timed_mutex> guard(mutex_, std::try_to_lock);
        if (!guard.owns_lock() || !device_) return;
        // Whatever happens next, the next look is a full interval away, so a TV
        // that is off or on another input is not asked about on every tick.
        lastKeepAliveTick_ = now ? now : 1;

        const char* skip = nullptr;
        if (!device_->OnScreen()) {
            skip = "another input is on screen";
        } else {
            const cec::PowerStatus power = device_->QueryPowerStatus(1000);
            lastTrafficTick_ = GetTickCount();
            if (power != cec::PowerStatus::On) skip = "the TV is not reporting that it is on";
        }
        if (skip) {
            // Once per reason, not every five minutes for as long as it holds.
            if (keepAwakeSkip_ != skip) Log("keep-awake: nothing sent, %s", skip);
            keepAwakeSkip_ = skip;
            return;
        }
        keepAwakeSkip_ = nullptr;

        const cec::CecDevice::KeepAlive signal = KeepAwakeSignal();
        const bool sent = device_->SendKeepAlive(signal);
        lastTrafficTick_ = GetTickCount();
        Log("keep-awake: %s %s%s%s", KeepAliveName(signal), sent ? "sent" : "FAILED",
            sent ? "" : ": ", sent ? "" : device_->lastError().c_str());
    }

    // What the tray asks for when its menu opens, answered from a copy rather
    // than from the device itself.
    //
    // The menu used to ask two questions that both took the CEC lock, and the
    // lock is held for as long as whatever is using the link takes: about a
    // second for an announce, several for a verified standby, longer for a
    // sweep of the bus. So a right click could sit there for a second or more
    // with nothing drawn, which is why the menu sometimes appeared wherever
    // the pointer had got to. Nothing it shows is worth waiting for, so it is
    // refreshed by the pump - which holds the lock anyway - and read from
    // under a lock of its own that is never held for more than a copy.
    void RefreshSnapshot() {
        std::string line =
            looking_ ? "OK attached=0 looking=1" : "OK attached=0 looking=0";
        if (device_) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer),
                     "OK attached=1 la=%u pa=%s name=%s audio=%s nolevel=%d",
                     device_->logicalAddress(),
                     cec::FormatPhysicalAddress(knownAddress_).c_str(),
                     device_->osdName().c_str(),
                     // For diagnosis: when volume does nothing, the first
                     // question is who was supposed to act on it.
                     device_->hasAudioSystem() ? "system" : "tv",
                     // Whoever owns volume has refused an absolute level, so
                     // the slider in the panel has nothing to offer and goes.
                     device_->audioNews().levelRefused ? 1 : 0);
            line = buffer;
        }
        std::vector<Peer> devices = ListDevices();
        // screenPath, not activePath: on a set that answers a switch request by
        // finishing the switch it was already making, the device's claim is true
        // about the bus and stale about the picture. See screenPath().
        const uint16_t onScreen =
            (device_ && device_->screenPathKnown()) ? device_->screenPath() : 0;

        std::lock_guard<std::mutex> guard(snapshotMutex_);
        statusSnapshot_ = std::move(line);
        deviceSnapshot_ = std::move(devices);
        onScreenSnapshot_ = onScreen;
    }

    // One line of state for the tray app. Deliberately does not touch the
    // hardware: the tray asks for this whenever its menu opens, and waking the
    // adapter for a menu draw would be a poor trade.
    std::string DescribeStatus() {
        std::lock_guard<std::mutex> guard(snapshotMutex_);
        return statusSnapshot_;
    }

    std::vector<Peer> DevicesForMenu() {
        std::lock_guard<std::mutex> guard(snapshotMutex_);
        return deviceSnapshot_;
    }

    // Which HDMI port the television says is on screen, or 0 when it has never
    // said. For the dot beside the live input in the menu.
    uint16_t WhatIsOnScreen() {
        std::lock_guard<std::mutex> guard(snapshotMutex_);
        return onScreenSnapshot_;
    }

    // Named commands only - the pipe never carries raw CEC frames, so this
    // function is the complete list of what a non-admin caller can make the
    // hardware do. Re-attaches first because the tray may be the first thing to
    // touch CEC in hours, by which time the TV has dropped our registration.
    std::string RunCommand(const std::string& name, const std::string& argument) {
        std::lock_guard<std::recursive_timed_mutex> guard(mutex_);
        if (!device_ && !Attach()) return "ERR not attached to a CEC port";

        // Re-announce only when the registration may have gone stale. Doing it
        // before every command added ~1.1s to each keypress, which made the
        // volume buttons feel broken and - worse - delayed the matching
        // <User Control Released> long enough for the TV to start auto-repeating.
        //
        // Except for tv-on, where it happens afterwards. A stale registration
        // costs about 1.1 seconds to refresh, and putting that in front of
        // <Image View On> is a second of black screen for somebody who has just
        // pressed the shortcut that exists to be pressed without looking. The
        // announce is not skipped, only moved: the claim goes out, the
        // registration is refreshed behind it, and the claim is repeated if the
        // television did not take the first one.
        if (name != "tv-on") EnsureAnnounced();

        // Warm-up only: the tray sends this when the pointer reaches the tray
        // icon, so the work runs while the user is still travelling towards a
        // button rather than costing them a wait on the first click.
        //
        // This is also where a soundbar that has come back is found. Opening
        // the panel is the natural moment to re-check: the poll is a single
        // frame, it happens before any button is pressed, and it makes the
        // answer deterministic rather than depending on whether the link
        // happened to be idle long enough for the topology refresh. Probing on
        // every press instead would double the latency of every press to catch
        // something that changes when someone moves a cable.
        //
        // It deliberately returns before the traffic stamp below. The poll is a
        // frame on the wire, but counting it would let a resting pointer keep
        // the link looking busy and suppress the topology refresh entirely.
        if (name == "prepare") {
            const DWORD since = GetTickCount() - lastAudioProbeTick_;
            if (!lastAudioProbeTick_ || since >= kAudioProbeIntervalMs) {
                ProbeAudioSystem();
            }
            return "OK";
        }

        // Stamped before the dispatch so that every exit path below - including
        // the error ones - counts as traffic on the link.
        lastTrafficTick_ = GetTickCount();
        if (name.compare(0, 6, "press-") == 0) {
            if (!holding_) holdStartTick_ = lastTrafficTick_;
            holding_ = true;
        } else if (name == "release") {
            holding_ = false;
        }

        bool ok = false;
        bool announceAfterClaim = false;
        if (name == "tv-on") {
            // Deliberate, so it gets the version that survives a TV in standby.
            //
            // A person pressed the hotkey and is waiting for a picture, so this
            // claims the screen before spending anything on upkeep: the
            // announce EnsureAnnounced may have wanted has been moved to after
            // the claim, which is the order the wake path has always used.
            SetDisplayOwnsStandby(false);   // it turns the TV on: hard rule 18
            ok = device_->WakeAndClaimInput(true);
            announceAfterClaim = true;
            // Wanting the screen is the opposite of handing it over.
            handoverUntilTick_ = 0;
        } else if (name == "tv-standby") {
            // <Standby> is a toggle on the TCL (hard rule 1), so turning off a
            // set that is already off turned it back on. The menu seldom met
            // that; a script sending tv-off whenever a stream ends meets it all
            // the time. So the set is asked first, and sent <Standby> only if
            // it is on or turning on - or cannot say, unless this service sent
            // it <Standby> moments ago and it is still going off, which is when
            // a set stops answering.
            const cec::PowerStatus status = device_->QueryPowerStatus(2000);
            const bool sentLately = lastStandbySentTick_ &&
                                    GetTickCount() - lastStandbySentTick_ < 15000;
            if (status == cec::PowerStatus::Standby ||
                status == cec::PowerStatus::TransitioningToStandby ||
                (status == cec::PowerStatus::Unknown && sentLately)) {
                Log("pipe command 'tv-standby': the TV reports %s, nothing sent",
                    cec::PowerStatusName(status));
                return "OK";
            }
            // Which state it was in goes in the log, so a set that stops
            // answering while off shows up as that rather than as a mystery.
            Log("pipe command 'tv-standby': the TV reports %s, sending <Standby>",
                cec::PowerStatusName(status));
            const DWORD sentAt = GetTickCount();
            ok = device_->SendStandby();
            if (ok) {
                lastStandbySentTick_ = sentAt;
                g_userActiveTick = 0;
                // Verified, unlike every other command here, because this one
                // has a second route when the first is refused and a menu item
                // that quietly does nothing is the complaint that found it. The
                // wait is short: the set answers in about three seconds, and
                // the person who pressed it is watching the television.
                // Five seconds, so that an "on" from a question asked after
                // the set has had time to act can arrive at all. The key it
                // leads to only turns a set off, and the wait ends the moment
                // standby is reported, which on the TCL is about three seconds.
                const cec::PowerStatus after = device_->WaitForStandby(5000);
                if (after == cec::PowerStatus::On) PressPowerOff(4000, sentAt);
            }
        } else if (name == "active-source") {
            ok = device_->SendActiveSource();
        } else if (name == "audio-status") {
            // Diagnostic, and not on any menu. Sending volume proves nothing -
            // a TV that acts on it and one that ignores it ACK the frame the
            // same way - so this asks the only question that distinguishes them.
            uint8_t status = 0;
            ok = device_->QueryAudioStatus(&status);
            if (ok) {
                Log("audio status from %s: volume %u%s",
                    device_->hasAudioSystem() ? "the audio system" : "the TV",
                    status & 0x7F, (status & 0x80) ? ", muted" : "");
            }
        } else if (name == "volume-up") {
            ok = device_->SendVolumeTap(cec::CecDevice::kUiVolumeUp);
        } else if (name == "volume-down") {
            ok = device_->SendVolumeTap(cec::CecDevice::kUiVolumeDown);
        } else if (name == "mute") {
            ok = device_->SendVolumeTap(cec::CecDevice::kUiMute);
        } else if (name == "press-volume-up") {
            ok = device_->SendVolumePressed(cec::CecDevice::kUiVolumeUp);
        } else if (name == "press-volume-down") {
            ok = device_->SendVolumePressed(cec::CecDevice::kUiVolumeDown);
        } else if (name == "press-mute") {
            ok = device_->SendVolumePressed(cec::CecDevice::kUiMute);
        } else if (name == "release") {
            // Always accepted, even with nothing held: a stray release is
            // harmless, whereas a missed one leaves a device repeating a key.
            ok = device_->SendVolumeReleased();
        } else if (name == "set-volume") {
            // Absolute volume, CEC 2.0's <Set Audio Volume Level>.
            //
            // Sent without checking whether the set claims to support it,
            // because there is no way to check: <Give Features> is itself 2.0,
            // so a 1.4 device cannot be asked about a 2.0 message. The TCL
            // reports 1.4 and accepts this perfectly, which is the case that
            // would have been lost by trusting the version number.
            //
            // A set that does not know the opcode ignores it or answers
            // <Feature Abort>, and the panel's own read-back then puts the
            // slider where the volume actually is. Nothing needs to detect
            // anything.
            // An empty argument is refused too: atoi reads it as 0, and a
            // request that lost its number must not become silence.
            if (argument.empty() || argument.size() > 3) {
                return "ERR volume must be 0-100";
            }
            const int level = std::atoi(argument.c_str());
            if (level < 0 || level > 100 ||
                argument.find_first_not_of("0123456789") != std::string::npos) {
                return "ERR volume must be 0-100";
            }
            ok = device_->SendSetAudioVolumeLevel(static_cast<uint8_t>(level));
        } else if (name == "input") {
            // HDMI 1-4 map to physical addresses 1.0.0.0 through 4.0.0.0. Range
            // checked here rather than trusted from the caller.
            if (argument.size() != 1 || argument[0] < '1' || argument[0] > '4') {
                return "ERR input must be 1-4";
            }
            // The same road as the menu item, rather than a second copy of it.
            // This command and Switch Input ask for exactly the same thing, and
            // they used to do it with two sets of retries and two ideas of what
            // counted as success. See SwitchTo.
            return SwitchTo(static_cast<uint16_t>((argument[0] - '0') << 12));
        } else {
            return "ERR unknown command";
        }

        // The announce tv-on skipped on the way in. Behind the claim, where it
        // delays nothing the person can see, and followed by one more
        // <Active Source> if the television did not act on the first - which is
        // the case a stale registration would have caused.
        // Only when the refresh actually happened. A television that had
        // dropped this device from its topology may have ignored the claim that
        // went out in front of it, and one more <Active Source> is a single
        // broadcast frame - the same belt and braces the wake path uses, where
        // <Image View On> is sent both before and after the announce.
        if (announceAfterClaim && EnsureAnnounced()) {
            Log("tv-on: repeating the claim, since the registration was "
                "refreshed behind it");
            device_->SendActiveSource();
        }

        // A volume answer that landed while this command held the lock is
        // collected now, before letting go. The pump looks for it as well, but
        // it only ever tries the lock, and a run of fast clicks keeps the lock
        // busy back to back - so the answer sat unread in the adapter until the
        // clicking stopped. Done after the frame has gone, so it never delays
        // one, and only while an answer is actually due: one register read.
        if (audioAsked_) {
            device_->ServicePending(kPumpFramesPerTick);
            PublishAudioNews();
        }

        // Volume arrives a dozen commands a second, and this is file I/O on the
        // command path - a run of presses was filling the log with thousands of
        // identical lines and burying the power events it exists to record.
        // Failures always get a line; successes only where there is something
        // worth going back to.
        const bool routine = name.compare(0, 6, "press-") == 0 ||
                             name.compare(0, 7, "volume-") == 0 ||
                             name == "release" || name == "mute" ||
                             name == "set-volume";
        // A press that needed retrying is worth a line even when it got
        // through: that is how it shows whether the retry rescues clicks.
        const int retries =
            name.compare(0, 6, "press-") == 0 ? device_->pressRetries() : 0;
        if (!ok || !routine || retries) {
            // The reason goes with a failure. Without it a run of failed
            // presses could not be told apart: a TV refusing a frame, a
            // collision on the bus, and an adapter that has gone quiet all
            // read the same.
            char retried[32] = "";
            if (retries) {
                snprintf(retried, sizeof(retried), " after %d %s", retries,
                         retries == 1 ? "retry" : "retries");
            }
            Log("pipe command '%s%s%s' %s%s%s%s", name.c_str(),
                argument.empty() ? "" : " ", argument.c_str(),
                ok ? "sent" : "FAILED", retried, ok ? "" : ": ",
                ok ? "" : device_->lastError().c_str());
        }
        // Which input is on screen may have changed, and this still holds the
        // lock the snapshot is built under.
        RefreshSnapshot();
        return ok ? "OK" : ("ERR " + device_->lastError());
    }

private:
    // Rebuilds the backend list and finds a CEC-capable port. Everything is
    // re-acquired from scratch because the DisplayPort link drops across sleep.
    bool AcquireDevice() {
        // Order matters: device_ holds a reference to a backend inside
        // backends_, so it has to go first. Replacing backends_ while device_
        // still referenced one would destroy that backend - calling
        // NvAPI_Unload - out from under a live reference.
        ReleaseDevice();
        // Built once and kept. Recreating them per attach tore down and
        // reinitialised NVAPI every time - LoadLibrary, NvAPI_Initialize,
        // NvAPI_Unload - which at cold boot the retry loop could repeat sixty
        // times before the driver was even ready. What actually goes stale is
        // the CEC state and possibly the display list, and both are rebuilt
        // below regardless.
        if (backends_.empty()) backends_ = cec::CreateAllBackends();
        cec::AuxBackend* backend = nullptr;
        cec::AuxPort port;
        std::string error;
        if (!cec::FindCecPort(backends_, &backend, &port, &error)) {
            lastError_ = error;
            ReleaseDevice();
            return false;
        }
        device_ = std::make_unique<cec::CecDevice>(*backend, port);
        // Range-checked before the cast, so a stray value in the registry
        // cannot wrap round into an address that looks valid.
        const DWORD stored = ReadNumber(L"LogicalAddress");
        const uint8_t remembered =
            stored < 16 ? static_cast<uint8_t>(stored) : 0;
        device_->set_preferredAddress(remembered);
        // The address to start on: the one the last device held, or failing
        // that the one remembered from the last run, or failing both the
        // default. set_logicalAddress ignores anything that is not one of the
        // four this app uses, so the second call wins only when it is valid
        // and a zero or bad value leaves the one before it in place.
        //
        // This is what makes a wake come back on the address it went to sleep
        // on. The wake path builds a new device and never allocates, so the
        // address set here is the one it claims, announces and keeps. A full
        // attach still probes, preferring the remembered address as before;
        // what changes there is that the address it treats as its own, and so
        // never avoids, is now really its own rather than always 4.
        device_->set_logicalAddress(remembered);
        device_->set_logicalAddress(heldAddress_);
        device_->set_knownIdentityKey(ReadText(L"LastTv"));
        // A new device counts its news from zero, so the count last passed on
        // belongs to the old one. Left as it was, a stale number would never
        // match again and a question asked of the old device would stay
        // outstanding.
        publishedAudioCount_ = 0;
        audioAsked_ = false;
        device_->set_osdName(PcName());
        // Installed unconditionally; the observer itself checks the setting, so
        // turning the log on takes effect at the next LoadSettings rather than
        // needing the device to be rebuilt.
        device_->set_frameObserver(LogCecFrame);
        device_->set_routingObserver(LogRoutingFrame);
        device_->set_screenPathSink([](uint16_t path, bool known) {
            g_onScreenPath.store(known ? static_cast<int32_t>(path) : -1);
        });
        device_->set_sendObserver(LogCecSend);
        device_->set_takeScreenHandler([this] { return TakeScreenForPeer(); });
        device_->set_deviceObserver(
            [this](const std::vector<uint8_t>& frame, bool) {
                NoteDeviceFrame(frame);
            });
        device_->set_peerFoundHandler(
            [this](uint8_t address, uint16_t where, const std::string& name,
                   bool answered) {
                NotePeer(address, where, name, answered);
            });
        device_->set_peerGoneHandler([this](uint8_t address) {
            ForgetPeer(address);
        });
        device_->set_userControlObserver(OnUserControl);
        // Seed the last known address so a sink with an unreadable EDID still
        // has something sane to announce.
        if (knownAddress_) device_->set_physicalAddress(knownAddress_);
        // The addresses other copies of this app hold, given to the device
        // before anything can allocate with it. A new device starts with none,
        // and a full attach allocates straight away, so it used to probe with
        // no mask at all until the next neighbour round set one - which is how
        // a PC re-attaching mid-run could take a sleeping neighbour's address.
        TidyPeerAddresses();
        return true;
    }

    // Lets go of the device, keeping the CEC address it held for the next one.
    // Every path that discards a working device comes through here, so a
    // recovery that rebuilds the device keeps the address as a wake does.
    void ReleaseDevice() {
        if (device_) heldAddress_ = device_->logicalAddress();
        device_.reset();
    }

    // The service control handler runs on an SCM thread, separate from the one
    // running ServiceMain, and both drive this object. Without this, a power
    // event arriving during the startup attach would have two threads swapping
    // backends_ and device_ at once - a use-after-free in a SYSTEM service.
    // Recursive because the recovery paths re-enter through Attach().
    std::recursive_timed_mutex mutex_;
    std::vector<std::unique_ptr<cec::AuxBackend>> backends_;
    std::unique_ptr<cec::CecDevice> device_;
    uint16_t knownAddress_ = 0;
    // The CEC logical address the last working device held, carried into the
    // next one. See ReleaseDevice and AcquireDevice. Zero until there has been
    // a device, and then the remembered address stands in for it.
    uint8_t heldAddress_ = 0;
    // The search for a port after a display arrives with none attached. See
    // AttachIfDisplayReturned. Zero when no search is open.
    DWORD attachUntilTick_ = 0;
    DWORD nextAttachTick_ = 0;
    DWORD lastAnnounceTick_ = 0;
    DWORD lastStandbySentTick_ = 0;
    // The display-off path has passed its last look at a power-down and is
    // sending <Standby>. Read without the lock by a power-down waiting for it.
    std::atomic<bool> displayCommitted_{false};
    DWORD lastAudioProbeTick_ = 0;
    std::atomic<DWORD> lastTrafficTick_{0};
    // Read by the pipe for the menu, written by the pump. Its own lock, held
    // only long enough to copy, so a menu never waits on the CEC link.
    std::mutex snapshotMutex_;
    // Says it is still looking, not merely unattached. The pipe server
    // answers questions a clear second before the first attach finishes and
    // well before the pump that refreshes this has started, so this is the
    // literal answer a menu opened straight after logon gets - and without the
    // flag it read as a TV that had been looked for and not found.
    std::string statusSnapshot_ = "OK attached=0 looking=1";
    std::vector<Peer> deviceSnapshot_;
    uint16_t onScreenSnapshot_ = 0;

    // True from the moment the service starts until the opening attach has
    // either worked or run out of attempts. Read by RefreshSnapshot from the
    // pipe thread while AttachWithRetry is still running on the main one, so it
    // is an atomic rather than a plain bool.
    std::atomic<bool> looking_{true};
    DWORD lastGreetTick_ = 0;
    DWORD lastInputRequestTick_ = 0;
    // Until when a request for this PC to take the screen is this PC's own
    // handover echoing back. See kHandoverQuietMs.
    DWORD handoverUntilTick_ = 0;
    // Which television the peer list was learnt from. See ForgetPeersIfTvChanged.
    std::string peersForTv_;
    // The leash on address moves. See kAddressRetryGapMs.
    DWORD addressRetryTick_ = 0;
    int addressRetries_ = 0;
    uint32_t warnedMalformed_ = 0;
    uint32_t warnedOverflows_ = 0;
    // A deque rather than a vector, and that is load-bearing. DeviceAt() hands
    // back a reference into this list, and MapTheBus holds one while it asks a
    // device where it is and what it is called - questions that read frames
    // off the bus, and a frame from an address not seen before adds a peer.
    // Growing a vector moves its contents, so the reference the sweep was
    // still holding would name freed memory, and the answer would be written
    // there. A deque never moves what it already holds.
    std::deque<Peer> peers_;
    bool mapWanted_ = false;
    // Which device the next spare <Give Physical Address> goes to, and when the
    // last one went. See GreetPeersNow; the cursor only has to advance, so
    // wrapping is harmless.
    size_t portCursor_ = 0;
    DWORD lastPortRefreshTick_ = 0;
    // The window after a display arrives in which this PC keeps asking where it
    // is, and when the next of those asks is due. See
    // RecheckAddressIfDisplayChanged.
    DWORD recheckUntilTick_ = 0;
    DWORD nextRecheckTick_ = 0;
    bool quiesced_ = false;
    bool warnedBusRefusing_ = false;
    // The volume question that is out, if any, and how much news has already
    // gone to the tray. The flag is atomic because the pump reads it before
    // taking the lock, to decide how soon to look again.
    std::atomic<bool> audioAsked_{false};
    DWORD audioAskTick_ = 0;
    uint32_t publishedAudioCount_ = 0;
    // When the last keep-alive was considered, and the reason it last sent
    // nothing, so a reason that holds for hours is logged once.
    std::atomic<DWORD> lastKeepAliveTick_{0};
    const char* keepAwakeSkip_ = nullptr;
    DWORD holdStartTick_ = 0;
    bool holding_ = false;
    std::string lastError_;
};

CecSession g_session;

// ---------------------------------------------------------------- query pump
//
// A CEC device is expected to be reachable. Before this existed, the receive
// buffer was serviced in exactly one place - three half-second windows during
// the first nine seconds of service life - so for practically its whole run
// this process was a device that never answered anything.
//
// That is what made the input name unreliable. An unsolicited <Set OSD Name>
// during the announce is a suggestion the TV may take or ignore, and mostly it
// took it; but a TV that instead asks <Give OSD Name> and hears nothing back
// falls through to a placeholder, or drops the device and shows the bare input
// number. Intermittent, unreproducible, and entirely explained by whether the
// TV happened to ask at a moment nobody was listening.
//
// The earlier note that the TV "sends nothing but a poll" after a re-announce
// was a real measurement, but a narrow one: one TV, in the two seconds directly
// after the announce. It does not cover a question asked ten seconds later, or
// on a different set.

HANDLE g_pumpThread = nullptr;

DWORD WINAPI QueryPump(LPVOID) {
    for (;;) {
        // Fastest while a volume answer is due, because that is the one thing
        // the pump waits for that someone is watching arrive.
        const DWORD interval = g_session.AwaitingVolumeAnswer() ? kVolumeAnswerPollMs
                               : RemoteIsActive()                ? kPumpRemoteMs
                               : PumpIsHot()                     ? kPumpBusyMs
                                                                 : kPumpIdleMs;
        if (WaitForSingleObject(g_stopEvent, interval) == WAIT_OBJECT_0) return 0;
        if (g_pumpPaused) continue;

        // Files away whatever the session has learned about the television.
        // Here because this thread never holds the CEC lock while doing it, and
        // almost every tick finds nothing to write.
        g_tvStore.Sync();

        // A skipped tick is not a missed question. Skipping means a command
        // holds the link, and every command path drains the receive buffer
        // itself, so the question is answered by whoever is already there.
        const int answered = g_session.PumpQueries();
        if (answered > 0) {
            // Questions arrive in bursts, so one answer is a good reason to
            // expect more.
            GoHot();
        }

        if (KeepAwakeRequested()) {
            g_session.KeepTvAwake();
        } else if (g_keepAwakeUntilTick.load()) {
            // A lease that ran out rather than being ended: the tray stopped
            // renewing it, most likely because it exited. Said once.
            g_keepAwakeUntilTick = 0;
            Log("keep-awake: ended, the tray app stopped renewing it");
        }
    }
}

// ------------------------------------------------------ display power follow
//
// Sleep and shutdown were always covered. Blanking was not: with a power plan
// display timeout, or a screen saver, or the lock screen, Windows turns the
// picture off while the machine keeps running - and a television, unlike a
// monitor, sits there lit and showing black.
//
// GUID_CONSOLE_DISPLAY_STATE is the notification for exactly that, and it is
// delivered to services, which is the half of the pair that matters here;
// GUID_SESSION_DISPLAY_STATUS is its session-bound sibling and is no use from
// Session 0. Data is a single DWORD: 0 off, 1 on, 2 dimmed.
//
// Declared in winnt.h but defined in a lib this project does not link, so it is
// spelled out rather than dragged in for one constant.
const GUID kGuidConsoleDisplayState = {
    0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

HPOWERNOTIFY g_displayNotify = nullptr;

// GUID_DEVINTERFACE_MONITOR, written out for the same reason as the one above.
const GUID kGuidMonitorInterface = {
    0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
HDEVNOTIFY g_monitorNotify = nullptr;
HANDLE g_displayEvent = nullptr;   // auto-reset; signalled by the handler

// Signalled by the pipe when the tray expects the screen to go off soon. Its
// own event rather than the display one, so that a guess can never be read as
// the display actually changing state.
HANDLE g_prepareEvent = nullptr;

// Signalled when the tray has seen a person touch this PC while the television
// is off by this service's doing.
HANDLE g_userWakeEvent = nullptr;
HANDLE g_displayThread = nullptr;

// 0 off, 1 on, 2 dimmed - the values Windows uses, kept rather than reduced to
// a bool, because dimming is the only advance warning this service ever gets
// that the picture is about to go.
std::atomic<DWORD> g_displayState{1};

// Set only when this service is the one that put the TV into standby because
// the display went off. Nothing else turns it back on, and the suspend and
// shutdown paths read it to avoid sending <Standby> a second time - which on a
// set that treats the command as a power toggle turns the TV back on.
std::atomic<bool> g_displayStandbySent{false};

// Set the moment a sleep begins and cleared on resume; a shutdown sets it and
// never needs to clear it, because the process does not outlive one. Read by
// the display-off path, which stands aside for either.
std::atomic<bool> g_powerDownInProgress{false};

// Set when the machine resumes, cleared once somebody turns out to be there
// and the television is turned on for them. See the resume handler.
std::atomic<bool> g_wakePending{false};

// Windows' word that a person woke the machine, kept when it arrives before the
// resume notice it is documented to follow. It was dropped in that case, and a
// television stayed off: on 2026-10-30 at 10:33, a power button wake that
// Windows treated as attended (it kept the machine up for its full fifteen
// minute idle timeout, where an unattended wake sleeps again after two), and
// on 2026-11-01 at 13:12, a keyboard wake, where the television came on only
// when the remote was picked up and the mouse moved a minute later. The same
// wake source turned the set on at once two hours earlier, so it was not
// Windows deciding nobody was there.
std::atomic<bool> g_userResumeEarly{false};

// Guards against acting on shutdown twice. Windows normally sends PRESHUTDOWN
// *instead of* SHUTDOWN, but both are handled in case a future Windows, or a
// failure to register PRESHUTDOWN, delivers only the latter. Sending <Standby>
// twice would be actively harmful here: this TV treats it as a power toggle,
// so a duplicate turns the TV back on.
std::atomic<bool> g_systemShutdownHandled{false};

bool DisplayStillOff() { return g_displayState.load() == 0; }
bool DisplayOwnsStandby() { return g_displayStandbySent.load(); }
void SetDisplayOwnsStandby(bool owned) { g_displayStandbySent = owned; }
bool PowerDownStarted() {
    return g_powerDownInProgress.load() || g_systemShutdownHandled.load();
}

// Whether a tray is watching this screen for signs of a person.
//
// A television changing power state drops its HDMI signal and restores it,
// Windows treats the display reappearing as reason to wake the screen, and the
// path below would then turn the television straight back on - undoing the
// <Standby> sent a moment earlier. Seen on both test machines:
//
//   08:01:24  windows: display disconnected, TCL9653
//   08:01:25  windows: screen on
//   08:01:25  display on: turning the TV back on
//
// Nothing about the event itself tells the two apart, and neither does what
// follows: the screen stayed on for seven minutes afterwards, exactly as it
// would have with somebody sitting there. The only real difference is input,
// which this service cannot see - Session 0 has no desktop - and the tray can.
//
// So the tray says when a screen has come on, and says again if a person then
// touches the PC, however much later. A screen-on nobody has claimed leaves the
// television off until they do. When no tray is running at all, at the login
// screen or with the app closed, there is nothing to wait for and the wake
// happens at once, as it always did.
bool TrayIsWatchingTheScreen() {
    // Waited for briefly, because Windows tells this service and the tray at
    // the same moment and they then race. Deciding on the instant always won
    // the race and always took the no-tray path, which is how the first version
    // of this shipped doing nothing at all. The wait costs a fraction of a
    // second on a screen the person woke themselves, since the tray reports
    // straight away, and nothing at all when there is no tray to wait for.
    const DWORD start = GetTickCount();
    for (;;) {
        const DWORD reported = g_trayScreenOnTick.load();
        if (reported && GetTickCount() - reported < kTrayReportFreshMs) return true;
        if (GetTickCount() - start >= kTrayReportWaitMs) return false;
        if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) return true;
    }
}

// Turns the television on, and keeps at it for about a minute.
//
// A driver can take that long to bring the port back: the Steam Deck took half
// a minute after a wake and, with a single attempt, stayed detached until the
// next reboot. Abandoned the moment the machine starts going down again or the
// screen goes off, since either means the television should not now be woken.
// <Image View On> is not a toggle, so a repeat is harmless.
//
// Returns false only when the service is stopping, which is the caller's cue to
// end its thread.
bool WakeTheTvPatiently() {
    bool woke = g_session.SendPowerOn();
    for (int attempt = 0; !woke && attempt < 30; ++attempt) {
        if (WaitForSingleObject(g_stopEvent, 2000) == WAIT_OBJECT_0) return false;
        if (PowerDownStarted() || DisplayStillOff()) break;
        woke = g_session.SendPowerOn(true, true);   // the first said why
    }
    if (!woke) {
        Log("wake: gave up turning the TV on (%s)",
            g_session.LastError().c_str());
    }
    GoHot();
    return true;
}

// The work runs here rather than in the control handler. Control callbacks are
// serialised, so a verified standby taking several seconds inside one would put
// a suspend arriving behind it - and suspend is the path with a real deadline.
// Nothing about a blanked display is urgent, so it yields.
DWORD WINAPI DisplayFollowThread(LPVOID) {
    HANDLE waits[4] = {g_stopEvent, g_displayEvent, g_prepareEvent,
                       g_userWakeEvent};
    const DWORD count = (g_prepareEvent && g_userWakeEvent) ? 4 : 2;
    for (;;) {
        const DWORD woken = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (woken == WAIT_OBJECT_0) return 0;

        // The tray's warning that the screen is about to go off. Only worth
        // acting on while the screen is on, the TV has not been turned off by
        // this service, and nothing is going down - and only if the setting
        // that would use it is on.
        if (woken == WAIT_OBJECT_0 + 2) {
            LoadSettings();
            if (g_settings.standbyOnDisplayOff && g_displayState.load() == 1 &&
                !g_displayStandbySent.load() && !PowerDownStarted()) {
                g_session.PrepareForBlank();
            }
            continue;
        }

        // A person has turned up while the television is off by this service's
        // doing. This is what actually brings it back, rather than the screen
        // coming on, because the screen also comes on when the television's own
        // signal returns and nobody is there.
        if (woken == WAIT_OBJECT_0 + 3) {
            if (PowerDownStarted()) continue;

            // Somebody is here after a wake. The screen is no use for telling
            // that: a scheduled wake on this desktop turned the display on
            // with nobody in the room, logged at 23:52 on 2026-09-25, so the
            // television waits for a person instead - Windows saying the wake
            // came from user activity, or the tray seeing a key or the mouse.
            // With sleep not followed, the wake is let go and the branch below
            // decides: the screen path turns the set back on only if it was
            // the one that turned it off, which is exactly that setting's job.
            if (g_wakePending.exchange(false) && SleepFollowed()) {
                // Released here as well as in the two branches below, and
                // forgetting it here cost a real bug: this flag says the
                // display-off path has a <Standby> still standing, and a
                // shutdown reads it to avoid sending a second one and toggling
                // a TCL back on. Turning the television on ends that, whichever
                // road did the turning. Left set, the next shutdown logged "the
                // TV is already off (the display blanked first); nothing to do"
                // and left a television on all night, on a machine that had
                // simply slept and woken first.
                SetDisplayOwnsStandby(false);
                Log("someone is at this PC after a wake; turning the TV on");
                if (!WakeTheTvPatiently()) return 0;
                continue;
            }

            if (DisplayOwnsStandby() && g_displayState.load() == 1) {
                SetDisplayOwnsStandby(false);
                Log("someone is using this PC; turning the TV back on");
                g_session.SendPowerOnWithRetry(g_stopEvent);
                GoHot();
            }
            continue;
        }

        // No settle delay here. There used to be one, and it was pure waiting:
        // the standby path then spent another second announcing before it sent
        // anything, and the TV showed its own no-signal screensaver throughout.
        // The abort window now lives inside SendStandbyForDisplayOff, which
        // re-reads the display state after its announce - so the wait does the
        // work instead of merely being a wait.

        // A machine on its way down owns the TV; whichever path got there
        // first has already decided what happens to it.
        if (g_systemShutdownHandled.load()) continue;

        const DWORD state = g_displayState.load();

        // Dim is not a blank. Nothing is switched off here; the announce is
        // simply moved off the critical path, so that when the picture does go
        // the <Standby> can leave immediately instead of a second later.
        if (state == 2) {
            LoadSettings();
            if (!g_settings.standbyOnDisplayOff) continue;
            if (g_displayStandbySent.load()) continue;
            Log("display dimmed: announcing now so the standby need not wait");
            g_session.PreAnnounce();
            continue;
        }

        if (state == 0) {
            LoadSettings();
            if (!g_settings.standbyOnDisplayOff) {
                // The TV is left alone, but on a Modern Standby machine this is
                // still the sleep, and the adapter still has to get off the bus
                // - see below. It used to skip that along with the <Standby>,
                // so turning this setting off on such a machine left a sleeping
                // adapter refusing every other device's broadcasts.
                if (ModernStandby()) g_session.LeaveBusQuiet();
                continue;
            }
            // Already off, by us. Still leaves the bus on a Modern Standby
            // machine, for the reason above: this is its sleep. Skipping that
            // along with the <Standby> left a sleeping laptop's adapter on the
            // bus whenever the TV was already off on this service's account -
            // which after an overnight update restart is every time.
            if (g_displayStandbySent.load()) {
                if (ModernStandby()) g_session.LeaveBusQuiet();
                continue;
            }
            Log("display off: turning the TV off to match");
            // Set after the send, and that ordering matters both ways. A sleep
            // or shutdown reads this to decide whether the TV is already off,
            // and setting it first - which an earlier version did, to close a
            // race with shutdown - meant a suspend arriving during the announce
            // would skip a <Standby> that had not gone out, then drop the link
            // and leave the television on. The race it was closing is now
            // handled properly, by StandbyAlreadySent() taking the CEC lock and
            // by this path standing aside once a power-down starts.
            g_session.SendStandbyForDisplayOff();   // records ownership itself

            // On a Modern Standby machine the screen going off is the sleep:
            // Windows sends no suspend notification at all, so this is the only
            // moment to put the adapter out of the way before nothing is left
            // running to collect what arrives on the bus. An adapter in that
            // state refuses everyone else's broadcasts, which on the test bench
            // left the desktop unable to switch the television off while a
            // laptop dozed in the corner. Taken up again when the screen
            // returns, from the pump.
            if (ModernStandby()) g_session.LeaveBusQuiet();
        } else {
            // The screen coming on after a wake, which is the moment a person
            // is known to be there: Windows leaves it off for the wakes that
            // are its own business. This, not the resume, is what turns the
            // television on - see the resume handler.
            // Only when nothing can see input. With a tray running, a person
            // is what brings the television back, because on this desktop a
            // scheduled wake turns the screen on by itself; without one - at
            // the login screen, or with the app closed - the screen is the
            // only sign there is.
            if (!TrayIsWatchingTheScreen() && g_wakePending.exchange(false) &&
                SleepFollowed()) {
                // Released for the same reason as the wake branch above: the
                // television is being turned on, so the <Standby> the
                // display-off path was still holding is spent.
                SetDisplayOwnsStandby(false);
                Log("the screen came on after a wake, and nothing here can see "
                    "input, so the TV is turned on");
                if (!WakeTheTvPatiently()) return 0;
                continue;
            }

            // Only undo what this service did. If the display simply came back
            // and we never turned the TV off, there is nothing to turn on -
            // and a TV the user switched off by hand stays off.
            //
            // Deliberately not gated on the setting: this is releasing state
            // this service created, and turning the setting off midway through
            // must not strand the TV off with the flag stuck set, which would
            // then suppress every later standby on sleep and shutdown.
            if (!DisplayOwnsStandby()) continue;
            // Left to the tray, which can see input. The television is woken by
            // the 'user-active' branch above when a person turns up, and stays
            // off when the screen was woken by its own signal returning.
            if (TrayIsWatchingTheScreen()) {
                Log("display on: waiting to see whether a person did that");
                continue;
            }
            SetDisplayOwnsStandby(false);
            Log("display on: turning the TV back on");
            g_session.SendPowerOnWithRetry(g_stopEvent);
            GoHot();
        }
    }
}

// --------------------------------------------------------- remote keys
//
// A television hands its remote's buttons to whichever device is the active
// source, so a PC on an HDMI input can be driven from the sofa. The frames
// arrive here; the keystrokes cannot. This service runs in Session 0, which has
// no desktop, and SendInput from there reaches nothing. So the press is mapped,
// queued, and the tray app - which has a desktop but no CEC - is woken to
// collect it.
//
// Transport keys were the whole of this at first, on the reasoning that a
// misdelivered media key only pauses something while a misdelivered Enter lands
// in whatever window is focused. The first real test corrected that: the only
// key an LG OLED forwarded was 0x00, <Select>, and its Magic Remote has no
// transport buttons to forward. A remote that can move a cursor and press it is
// what this television actually offers, and mapping only what it does not send
// is a principle protecting nothing.
//
// The risk was also overstated. Nothing arrives unbidden - a TV forwards a key
// because somebody pressed it - so a stray keystroke means the viewer pressed a
// button on the remote in their hand, which is the feature rather than a fault.

struct RemoteKey {
    uint8_t ui;        // CEC UI command code
    uint8_t vk;        // Windows virtual key
    const char* name;
};

// CEC 1.4 table 27. Sets differ in which of the two encodings they send for the
// same physical button - a "Play" key may arrive as 0x44 or as the 0x60 "Play
// Function" - so both are mapped rather than guessed between.
//
// Which of these repeat when held is decided by the tray, from the key a button
// ends up sending - see RemoteKeyRepeats in CecTray.cpp.
constexpr RemoteKey kRemoteKeys[] = {
    {0x00, VK_RETURN,           "Select"},
    {0x01, VK_UP,               "Up"},
    {0x02, VK_DOWN,             "Down"},
    {0x03, VK_LEFT,             "Left"},
    {0x04, VK_RIGHT,            "Right"},
    {0x0D, VK_BROWSER_BACK,     "Exit"},
    {0x44, VK_MEDIA_PLAY_PAUSE, "Play"},
    {0x46, VK_MEDIA_PLAY_PAUSE, "Pause"},
    {0x60, VK_MEDIA_PLAY_PAUSE, "Play Function"},
    {0x61, VK_MEDIA_PLAY_PAUSE, "Pause-Play Function"},
    {0x45, VK_MEDIA_STOP,       "Stop"},
    {0x64, VK_MEDIA_STOP,       "Stop Function"},
    {0x49, VK_MEDIA_NEXT_TRACK, "Fast Forward"},
    {0x4B, VK_MEDIA_NEXT_TRACK, "Forward"},
    {0x48, VK_MEDIA_PREV_TRACK, "Rewind"},
    {0x4C, VK_MEDIA_PREV_TRACK, "Backward"},
};

HANDLE g_remoteKeyEvent = nullptr;
std::mutex g_remoteKeyMutex;

// What the tray collects: the button itself as well as the key it sends by
// default, because the tray lets a person send something else for any button,
// including buttons with no default at all. Which key a button sends is a
// per-user preference, so it is decided on that side of the pipe; the table
// above stays the one place the defaults are written down.
struct QueuedRemoteKey {
    uint8_t ui;
    uint8_t vk;        // 0 when the button has no default
    bool repeat;       // the TV repeating a held button
};
std::vector<QueuedRemoteKey> g_remoteKeys;

// A queue this deep already means nobody is collecting. Bounded so that a tray
// app that has exited cannot make the service grow a list forever.
constexpr size_t kMaxQueuedKeys = 16;

// The code currently held down, so that a TV auto-repeating <User Control
// Pressed> for as long as a button is held produces one keystroke rather than a
// stream of them. Play/pause is a toggle; repeating it would flicker.
//
// Plain, not atomic, and that is safe rather than sloppy: every path that
// reaches AnswerQuery - the pump, a pipe command, the name reassert - holds the
// CEC mutex for the duration, so these are only ever touched under it.
uint8_t g_heldUiCommand = 0;
bool g_holdingRemoteKey = false;
DWORD g_heldUiTick = 0;

// How long after a press the next one can still be the television repeating
// it. CEC has a set repeat a held button every half second at the slowest, and
// a device that hears nothing for 550ms is told to treat the button as let go;
// this is that, with room for the pump to read the frame late.
//
// A press used to count as a repeat for as long as no release had arrived, and
// releases do go missing. The LG on 2026-09-30 sent Select at 20:17:44 and no
// release, and the Select pressed four seconds later was taken for the same
// button still held - so it was dropped, as a repeat of a key that does not
// repeat, and the person pressed it again. Exit went the same way twice that
// evening. With a time limit, a press after a pause is a press, whatever
// happened to the release before it.
constexpr DWORD kRemoteRepeatGapMs = 700;

// Called from the CEC layer while it holds the CEC mutex. Does as little as
// possible for that reason: a keypress must not be what makes a volume press
// wait.
void OnUserControl(uint8_t uiCommand, bool pressed) {
    if (!pressed) {
        g_holdingRemoteKey = false;
        return;
    }
    const DWORD now = GetTickCount();
    const bool repeat = g_holdingRemoteKey && uiCommand == g_heldUiCommand &&
                        now - g_heldUiTick < kRemoteRepeatGapMs;
    g_heldUiCommand = uiCommand;
    g_heldUiTick = now;
    g_holdingRemoteKey = true;

    // Any forwarded key means someone is holding the remote, so listen closely
    // whether or not this particular one is mapped. Done before the setting is
    // consulted: the pump rate is not a user-visible behaviour.
    GoRemoteHot();

    const RemoteKey* match = nullptr;
    for (const auto& key : kRemoteKeys) {
        if (key.ui == uiCommand) { match = &key; break; }
    }
    // Logged rather than dropped silently, and every button rather than only
    // the mapped ones. Which buttons a TV forwards is not documented by anyone
    // and differs by manufacturer, so this line is the only way to find out
    // what a given set is offering. It is also how the navigation keys above
    // came to be mapped at all. A held button's repeats are not logged.
    if (!repeat) {
        if (match) {
            Log("remote key %s (0x%02X)", match->name, uiCommand);
        } else {
            Log("remote key 0x%02X, no default", uiCommand);
        }
    }
    if (!g_settings.forwardRemoteKeys) return;

    // Buttons with no default are passed on too, and so are a held button's
    // repeats: whether a button does anything, and whether holding it should
    // repeat, depends on the key the person has chosen for it, which only the
    // tray knows. A TV repeats <User Control Pressed> for as long as a button
    // is held; for an arrow that is the point, and for play/pause it would
    // toggle it forty times, so the tray drops repeats for keys like that.
    {
        std::lock_guard<std::mutex> guard(g_remoteKeyMutex);
        if (g_remoteKeys.size() >= kMaxQueuedKeys) return;
        g_remoteKeys.push_back({uiCommand, match ? match->vk : uint8_t{0}, repeat});
    }
    if (g_remoteKeyEvent) SetEvent(g_remoteKeyEvent);
}

std::string DrainRemoteKeys() {
    std::lock_guard<std::mutex> guard(g_remoteKeyMutex);
    std::string response = "OK";
    for (const auto& key : g_remoteKeys) {
        char item[12];
        snprintf(item, sizeof(item), " %02X%02X%s", key.ui, key.vk,
                 key.repeat ? "R" : "");
        response += item;
    }
    g_remoteKeys.clear();
    return response;
}

// ------------------------------------------------------------ volume answers
//
// The same shape as remote keys, for the same reason: something the tray needs
// to hear the moment it happens, delivered without the tray sitting on the pipe
// waiting for it, which would wedge every button press behind it. The latest
// answer is kept here and the event says there is one to collect with VOLUME.
// Only the latest, because a level superseded before anyone read it is not
// worth showing.

HANDLE g_volumeEvent = nullptr;
std::mutex g_volumeReplyMutex;
std::string g_volumeReply = "OK";

// Called with the CEC lock held, so it stores and signals and nothing more.
void PublishVolume(const std::string& reply) {
    {
        std::lock_guard<std::mutex> guard(g_volumeReplyMutex);
        g_volumeReply = reply;
    }
    if (g_volumeEvent) SetEvent(g_volumeEvent);
}

std::string LatestVolume() {
    std::lock_guard<std::mutex> guard(g_volumeReplyMutex);
    return g_volumeReply;
}

// The tray cannot wait on an object it has no right to, and the service creates
// these, so the DACL is set here. Authenticated users get SYNCHRONIZE and
// nothing else: waiting, not signalling. Both events share the rights.
HANDLE CreateTrayEvent(const wchar_t* name, const char* what) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            cec::kRemoteKeyEventSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        Log("%s: could not build the event security descriptor", what);
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE event = CreateEventW(&attributes, FALSE, FALSE, name);
    LocalFree(descriptor);
    if (!event) {
        Log("%s: could not create the wake event (error %lu)", what,
            GetLastError());
    }
    return event;
}

void CreateTrayEvents() {
    g_remoteKeyEvent = CreateTrayEvent(cec::kRemoteKeyEventName, "remote keys");
    g_volumeEvent = CreateTrayEvent(cec::kVolumeEventName, "volume readout");
}

// ------------------------------------------------------------- pipe handler

cec::PipeServer g_pipeServer;
// The questions-only server. Same class, its own name and its own thread, so a
// command that sits on the one above for three seconds cannot delay it. See
// kReadPipeName and HandleReadRequest.
cec::PipeServer g_readPipeServer;

// The STATUS answer, identical on both pipes.
//
// Settings ride along with the status. The tray draws its whole menu from this
// one response; asking separately for each setting meant four round trips every
// time the menu opened, and any one of them losing a race drew a checkbox wrong.
//
// The read-only pipe used to answer with the session's line alone, and the
// menu asks that pipe first, so every setting it drew read as off: the
// checkboxes looked stuck while each click still reached the service, and
// tv= and pcvol went missing from what the tray routes volume by. One function
// for both, so the two answers cannot drift apart again.
//
// Memory only - no registry read or write, no CEC lock - which is what the
// read-only pipe requires. The command pipe brings the settings and the TV
// store up to date before calling it; the read-only pipe takes them as the
// last sync left them, which the pump and every SET keep current.
std::string FullStatus() {
    // pcvol is the value in force for the set on the other end, which is the
    // only one the tray can act on.
    bool pcVolume = g_settings.volumeControlsPc;
    g_tvStore.VolumeControlsPc(&pcVolume);
    char flags[160];
    snprintf(flags, sizeof(flags),
             " log=%d shutdown=%d sleep=%d blank=%d keys=%d pcvol=%d s0=%d",
             g_settings.loggingEnabled ? 1 : 0,
             g_settings.standbyOnShutdown ? 1 : 0,
             g_settings.standbyOnSleep ? 1 : 0,
             g_settings.standbyOnDisplayOff ? 1 : 0,
             g_settings.forwardRemoteKeys ? 1 : 0,
             pcVolume ? 1 : 0,
             ModernStandby() ? 1 : 0);
    return g_session.DescribeStatus() + flags + g_tvStore.Describe();
}

// Only these settings are reachable from the pipe, matched by exact name.
// SuspendSettleMs and DiagnosePower are deliberately absent: they are debugging
// knobs, not user settings, and every additional writable field is more surface
// exposed to a non-admin caller.
bool SettingByName(const std::string& name, bool** slot, const wchar_t** key) {
    if (name == "LoggingEnabled") {
        *slot = &g_settings.loggingEnabled;
        *key = L"LoggingEnabled";
    } else if (name == "StandbyOnShutdown") {
        *slot = &g_settings.standbyOnShutdown;
        *key = L"StandbyOnShutdown";
    } else if (name == "StandbyOnSleep") {
        *slot = &g_settings.standbyOnSleep;
        *key = L"StandbyOnSleep";
    } else if (name == "StandbyOnDisplayOff") {
        *slot = &g_settings.standbyOnDisplayOff;
        *key = L"StandbyOnDisplayOff";
    } else if (name == "ForwardRemoteKeys") {
        *slot = &g_settings.forwardRemoteKeys;
        *key = L"ForwardRemoteKeys";
    } else if (name == "VolumeControlsPc") {
        *slot = &g_settings.volumeControlsPc;
        *key = L"VolumeControlsPc";
    } else {
        return false;
    }
    return true;
}

// Splits on single spaces into at most three fields; the protocol needs no more
// than that, and refusing anything longer keeps parsing trivial.
void SplitRequest(const std::string& request, std::string parts[3], int& count) {
    count = 0;
    size_t start = 0;
    while (count < 3 && start < request.size()) {
        size_t space = request.find(' ', start);
        if (space == std::string::npos || count == 2) space = request.size();
        parts[count++] = request.substr(start, space - start);
        start = space + 1;
    }
}

// What is plugged into this television, for the "Switch Input" menu. One
// entry per HDMI port: the port, flags, then the name, which comes last
// because a name may contain spaces. Entries are separated by tabs.
//
// Lifted out of the command handler because the read-only pipe answers this
// one too, and the two must never drift: the tray reads these fields by
// position, and that has broken before.
std::string DescribeDevices() {
    std::string reply = "OK";
    // The live value, not the pump's snapshot. See g_onScreenPath: the list of
    // devices changes rarely and can wait for the next tick, but which of them
    // is on screen changes during exactly the seconds the pump is standing
    // aside, and that is the one thing somebody opens this menu to see.
    const int32_t live = g_onScreenPath.load();
    const uint16_t onScreen =
        live >= 0 ? static_cast<uint16_t>(live) : g_session.WhatIsOnScreen();
    const uint16_t here = g_ownAddress.load();
    for (const Peer& one : g_session.DevicesForMenu()) {
        reply += "\t";
        reply += cec::FormatPhysicalAddress(one.where);
        reply += one.present ? "|awake" : "|asleep";
        reply += one.where == onScreen ? "|onscreen" : "|";
        reply += one.where == here ? "|thispc" : "|";
        // An audio system is on a real HDMI port and is not an input: there
        // is nothing behind it to put on screen, so asking the television
        // to switch there did nothing and logged "nothing there answered".
        // It stays in the list, greyed, because seeing the soundbar listed
        // is how somebody knows it was found at all - and it is where the
        // volume is going.
        // One field for both, because they cannot both apply: an audio
        // system is never offered as an input, so it is never asked to
        // take the screen and never counted here.
        reply += IsAudioSystem(one) ? "|audio"
                 : one.switchIgnored >= kSwitchIgnoredSure ? "|ignores"
                                                           : "|";
        reply += "|";
        reply += one.name.empty() ? DeviceTypeName(one.type) : one.name.c_str();
    }
    return reply;
}

std::string HandlePipeRequest(const std::string& request) {
    std::string parts[3];
    int count = 0;
    SplitRequest(request, parts, count);
    if (count == 0) return "ERR empty request";

    const std::string& verb = parts[0];

    if (verb == "STATUS") {
        LoadSettings();
        g_tvStore.Sync();
        return FullStatus();
    }

    if (verb == "GET" && count >= 2) {
        bool* slot = nullptr;
        const wchar_t* key = nullptr;
        if (!SettingByName(parts[1], &slot, &key)) return "ERR unknown setting";
        LoadSettings();
        g_tvStore.Sync();
        bool value = *slot;
        if (parts[1] == "VolumeControlsPc") g_tvStore.VolumeControlsPc(&value);
        return std::string("OK ") + (value ? "1" : "0");
    }

    if (verb == "SET" && count >= 3) {
        bool* slot = nullptr;
        const wchar_t* key = nullptr;
        if (!SettingByName(parts[1], &slot, &key)) return "ERR unknown setting";
        if (parts[2] != "0" && parts[2] != "1") return "ERR value must be 0 or 1";
        const bool value = parts[2] == "1";
        // The tray names the setting and never the television. Which set it
        // applies to is decided here, from the EDID, so no registry path is
        // ever built from anything that came down the pipe.
        if (parts[1] == "VolumeControlsPc") {
            LoadSettings();
            g_tvStore.Sync();
            if (g_tvStore.SetVolumeControlsPc(value)) {
                Log("setting VolumeControlsPc changed to %d for this TV via pipe",
                    value ? 1 : 0);
                return "OK";
            }
        }
        if (!WriteFlag(key, value)) return "ERR could not write setting";
        *slot = value;
        Log("setting %s changed to %d via pipe", parts[1].c_str(), value ? 1 : 0);
        return "OK";
    }

    if (verb == "KEYS") return DrainRemoteKeys();

    // VOLUME alone is the latest answer and touches nothing. VOLUME ask puts a
    // question on the wire and returns before the answer, which arrives through
    // the volume event.
    if (verb == "VOLUME") {
        if (count >= 2 && parts[1] == "ask") return g_session.AskVolume();
        return LatestVolume();
    }

    // The keep-awake lease, answered here rather than as a command. A command
    // may re-announce first when the link has been idle, and a renewal every
    // two minutes would then put an announce on the bus every two minutes for
    // nothing. Neither of these touches the bus.
    if (verb == "CMD" && count >= 2 && parts[1] == "keep-awake") {
        const bool wasOn = KeepAwakeRequested();
        const DWORD until = GetTickCount() + kKeepAwakeLeaseMs;
        g_keepAwakeUntilTick = until ? until : 1;
        if (!wasOn) {
            g_session.StartKeepAlive();
            Log("keep-awake: on, keep-alive every %lu minutes",
                kKeepAliveIntervalMs / 60000);
        }
        return "OK";
    }
    // Answered at once and done on the display thread, so a second's announce
    // never holds up the pipe, and never through RunCommand, which counts its
    // commands as traffic and may announce first on its own account.
    // Real keyboard or mouse input, which only the tray can see. Sent for a few
    // seconds after the screen comes on, and nothing else, so this costs a
    // message about as often as a person sits down at the PC.
    if (verb == "CMD" && count >= 2 && parts[1] == "user-active") {
        const DWORD now = GetTickCount();
        g_userActiveTick = now ? now : 1;
        // A person is here. That matters when this service turned the TV off,
        // and equally after a wake, where it is the only reliable sign that
        // anybody is in the room.
        if ((DisplayOwnsStandby() || g_wakePending.load()) && g_userWakeEvent) {
            SetEvent(g_userWakeEvent);
        }
        return "OK";
    }

    // The screen has come on. Only says a tray is watching; whether a person
    // did it follows separately, or never.
    if (verb == "CMD" && count >= 2 && parts[1] == "screen-on") {
        const DWORD now = GetTickCount();
        g_trayScreenOnTick = now ? now : 1;
        return "OK";
    }
    if (verb == "DEVICES") return DescribeDevices();

    // Put that HDMI port on screen. The service decides how; see SwitchTo.
    if (verb == "CMD" && count >= 3 && parts[1] == "switch-to") {
        const uint16_t port = cec::ParsePhysicalAddress(parts[2]);
        if (!port) return "ERR bad HDMI port";
        return g_session.SwitchTo(port);
    }

    if (verb == "CMD" && count >= 2 && parts[1] == "prepare-blank") {
        if (g_prepareEvent) SetEvent(g_prepareEvent);
        return "OK";
    }
    if (verb == "CMD" && count >= 2 && parts[1] == "keep-awake-off") {
        if (KeepAwakeRequested()) Log("keep-awake: off");
        g_keepAwakeUntilTick = 0;
        return "OK";
    }

    if (verb == "CMD" && count >= 2) {
        return g_session.RunCommand(parts[1], count >= 3 ? parts[2] : "");
    }

    return "ERR unrecognised request";
}

// The whole of what the second pipe will do. See kReadPipeName.
//
// A whitelist of two verbs rather than a filter over the handler above, so that
// a verb added to that one can never turn up here by accident. Both are a copy
// out of the snapshot the pump keeps, taken under a mutex that is never held for
// longer than the copy. Neither touches the hardware, the registry, the settings
// or the CEC lock, and neither changes anything at all.
//
// That last property is what makes serving this at the same time as a command
// safe: it shares no mutable state with the command thread, so there is nothing
// for the two to race over. Running on its own thread is the entire point - the
// command pipe's handler can sit inside SwitchTo for three seconds waiting on a
// television, and until this existed the menu's question waited behind it and
// drew a stale answer during the exact seconds somebody had just watched the
// picture change and opened the menu to look.
std::string HandleReadRequest(const std::string& request) {
    if (request == "STATUS") return FullStatus();
    if (request == "DEVICES") return DescribeDevices();
    return "ERR unrecognised request";
}

// -------------------------------------------------------- service plumbing

void SetState(DWORD state, DWORD waitHint = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_RUNNING)
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
               SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_POWEREVENT)
            : 0;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        g_status.dwCheckPoint = 0;
    } else {
        ++g_status.dwCheckPoint;
    }
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

void HandleSystemShutdown(const char* phase) {
    if (g_systemShutdownHandled.exchange(true)) {
        Log("%s: shutdown already handled, ignoring", phase);
        return;
    }

    // Re-read rather than trusting what was loaded at boot, so a setting
    // changed since then actually takes effect.
    LoadSettings();

    std::string detail;
    const cec::ShutdownRecord seen =
        cec::QueryShutdown(120000, g_shutdownWords, g_startedAt, &detail);
    const cec::ShutdownKind kind = seen.kind;
    // A word this does not know is kept, with the time of the event that
    // carried it, so that the next start can ask Kernel-Power 109 what it meant
    // and be certain the answer is about this shutdown. See LearnShutdownWords.
    if (kind == cec::ShutdownKind::Unknown && !seen.word.empty() &&
        !seen.when.empty()) {
        WriteWideText(L"ShutdownPendingWord", seen.word);
        WriteWideText(L"ShutdownPendingWhen", seen.when);
    }
    // Unknown counts as a restart, and that is the safer way round rather than
    // an arbitrary one. A machine whose Windows writes a word this has not been
    // taught reads every shutdown as unknown: taking that as a power off turns
    // the television off through POST on every reboot, which is the very thing
    // the restart exception exists to prevent, while taking it as a restart
    // leaves a television on with no signal, which the set itself resolves by
    // sleeping. It only applies until the first shutdown teaches the word.
    const bool restart = (kind != cec::ShutdownKind::PowerOff);
    const bool wanted = !restart && g_settings.standbyOnShutdown;

    // Windows Update restarting the machine by itself, most often in the
    // night. The next start then leaves the TV alone - see StartAfterUpdate -
    // where it would otherwise turn it on, at three in the morning, for a PC
    // nobody is using. Written before anything below can return early, and
    // only for a restart: a power off is followed by somebody pressing the
    // button, who wants the TV.
    if (restart && seen.byWindowsUpdate) WriteNumber(L"StartAfterUpdate", 1);

    Log("%s: type=%s%s [%s]", phase, cec::ShutdownKindName(kind),
        seen.byWindowsUpdate ? ", by Windows Update" : "", detail.c_str());

    if (!wanted) {
        Log(restart ? "leaving the TV on, as a restart always does"
                    : "leaving the TV on (StandbyOnShutdown is off)");
        return;
    }

    // The display may have blanked on the way down, in which case the TV is
    // already off and this service is the one that turned it off. Sending
    // <Standby> again would toggle it back on and leave it on all night. The
    // TV is asked as well as the record - see TvNeedsStandby.
    if (!g_session.TvNeedsStandby(g_session.StandbyAlreadySent(), "shutdown")) {
        return;
    }

    if (!g_session.ShowingThisPc()) {
        Log("the TV is showing another input; left alone");
    } else {
        g_session.SendStandbyVerified(10000, true);
    }

    // A PC that is off leaves its adapter powered on plenty of machines, and an
    // adapter nobody is minding ends up refusing everyone else's broadcasts.
    g_session.LeaveBusQuiet();
}

DWORD WINAPI HandlerEx(DWORD control, DWORD eventType, LPVOID eventData,
                       LPVOID context) {
    (void)eventData;
    (void)context;

    switch (control) {
        // A manual stop - a rebuild, an uninstall, an admin stopping the
        // service - must NOT touch the TV. Only the machine actually going
        // down should.
        case SERVICE_CONTROL_STOP:
            Log("stop requested");
            SetState(SERVICE_STOP_PENDING, 3000);
            SetEvent(g_stopEvent);
            return NO_ERROR;

        // Preferred path. Preshutdown arrives before other services are torn
        // down - crucially while the Event Log service is still answering, so
        // the restart-vs-power-off lookup actually works - and carries a
        // three-minute budget rather than five seconds.
        case SERVICE_CONTROL_PRESHUTDOWN:
            SetState(SERVICE_STOP_PENDING, 30000);
            HandleSystemShutdown("system preshutdown");
            SetEvent(g_stopEvent);
            return NO_ERROR;

        // Fallback only; normally not delivered once PRESHUTDOWN is accepted.
        case SERVICE_CONTROL_SHUTDOWN:
            SetState(SERVICE_STOP_PENDING, 5000);
            HandleSystemShutdown("system shutting down");
            SetEvent(g_stopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_POWEREVENT:
            switch (eventType) {
                // Single unverified send, deliberately. Measured on this TV,
                // <Standby> takes 1.5-2.2s before the set reports STANDBY,
                // while suspend allows only about two seconds in total - so any
                // check here fires while the TV is still transitioning, reads
                // ON, and resends, which toggles it back on. That is not
                // hypothetical: it happened on 15 of 15 suspends when this path
                // verified at 800ms. Verification belongs only where there is
                // budget to wait out the transition, i.e. preshutdown.
                case PBT_APMSUSPEND:
                    LoadSettings();
                    Log("power event: suspending");
                    // Before anything else, so a display-off standby that has
                    // not yet sent stands aside rather than racing this.
                    g_powerDownInProgress = true;
                    // A sleeping machine has no picture. Recorded so that a
                    // wake with the screen left off is not mistaken for a
                    // person, which it would be if this still said "on" from
                    // before the sleep.
                    g_displayState = 0;
                    g_wakePending = false;
                    g_userResumeEarly = false;
                    // Anything the display thread had queued is stale from
                    // here: this path owns the television now, and a screen
                    // that went off on the way down is not news worth acting
                    // on when the machine comes back. Left signalled, it was
                    // acted on at the moment of the wake - an announce and a
                    // second of the link, spent to discover that the display
                    // had come back after all, at the busiest moment there is.
                    if (g_displayEvent) ResetEvent(g_displayEvent);
                    if (g_prepareEvent) ResetEvent(g_prepareEvent);
                    // The link is about to go down, and nothing that arrives
                    // now can be answered. Stopping first also keeps the pump
                    // out of the way of a path that is working to a deadline.
                    g_pumpPaused = true;
                    // Still leaves the bus, whatever the setting: a sleeping
                    // adapter that stays on it refuses everyone else's
                    // broadcasts, and that is not the user's choice to make.
                    if (!SleepFollowed()) {
                        Log("sleep: the TV is left as it is (it does not follow "
                            "sleep)");
                        g_session.LeaveBusQuiet();
                        break;
                    }
                    // Windows blanks the display before it suspends, so on a
                    // machine that sleeps after its display timeout the TV is
                    // already off and this service put it there. A second
                    // <Standby> would toggle it straight back on - the exact
                    // failure the single-send rule above exists to avoid. The
                    // record says whether it is already off, and the TV is
                    // asked as well - see TvNeedsStandby.
                    if (!g_session.TvNeedsStandby(g_session.StandbyAlreadySent(),
                                                  "sleep")) {
                        g_session.LeaveBusQuiet();
                        break;
                    }
                    g_session.SendStandbyAndSettle(
                        static_cast<int>(g_settings.suspendSettleMs));
                    // Last, because the adapter has to stay on the bus until
                    // the <Standby> above is done with.
                    g_session.LeaveBusQuiet();
                    break;

                // RESUMEAUTOMATIC always fires on wake; RESUMESUSPEND only
                // fires when a user is present. Handling just the first avoids
                // sending the command twice.
                // Sent only when the wake came from user activity, which is
                // exactly the question the television's power turns on. It
                // arrives after RESUMEAUTOMATIC, which is sent for every wake
                // including the ones the machine arranges for itself.
                // Logged because without it a wake that left the television
                // off could not say whether Windows ever spoke.
                case PBT_APMRESUMESUSPEND:
                    if (g_wakePending.load()) {
                        Log("power event: a person woke this PC");
                        if (g_userWakeEvent) SetEvent(g_userWakeEvent);
                    } else {
                        Log("power event: a person woke this PC (ahead of the "
                            "resume notice)");
                        g_userResumeEarly = true;
                    }
                    break;

                case PBT_APMRESUMEAUTOMATIC:
                    LoadSettings();
                    Log("power event: resuming");
                    // No name reassert here. It was tried and measured: the TV
                    // ignores <Set OSD Name> after a re-announce - four sends
                    // per wake changed nothing - while adding ~4s to the handler.
                    // The label is cosmetic; the wake path is not the place to
                    // spend time on something that does not work.
                    //
                    // Answering when asked is the part that does work, and a
                    // wake is when a TV is most likely to ask, so the pump comes
                    // back hot rather than merely resumed. It costs nothing on
                    // the handler's own timeline: it listens, it does not wait.
                    // The television is not turned on here.
                    //
                    // A PC resumes for its own reasons - Windows Update, a
                    // maintenance window, a scheduled task, a network packet -
                    // and does it with the screen left off. Turning the TV on
                    // for those means a bedroom television lighting up at four
                    // in the morning and going off again two minutes later,
                    // which the log caught happening on 2026-09-07 at 03:40.
                    //
                    // Windows lights the screen when a person is there and not
                    // otherwise, so that is what this waits for. The wait costs
                    // nothing on a real wake: the screen-on notification
                    // arrives while the desktop is still being drawn, which is
                    // as early as the old code managed anyway.
                    g_wakePending = true;
                    g_pumpPaused = false;
                    GoHot();
                    // Whatever the TV's state owed to the display blanking, the
                    // wake has just settled it.
                    g_displayStandbySent = false;
                    g_powerDownInProgress = false;
                    // Deliberately not setting the display state to on. It is
                    // whatever Windows last said, and after an unattended wake
                    // that is off - which is the whole point.
                    //
                    // Nothing is signalled here either. What turns the
                    // television on is a person: Windows' own word for a wake
                    // that came from user activity, or the tray seeing input.
                    // Both arrive on their own.
                    if (g_displayEvent) ResetEvent(g_displayEvent);
                    // Unless Windows has already said a person did this, ahead
                    // of the notice it is meant to follow.
                    if (g_userResumeEarly.exchange(false) && g_userWakeEvent) {
                        SetEvent(g_userWakeEvent);
                    }
                    break;

                // Windows blanking or restoring the picture while the machine
                // keeps running: a power plan timeout, a screen saver, the lock
                // screen. Recorded and handed to the worker, because a control
                // handler is the wrong place to spend seconds.
                case PBT_POWERSETTINGCHANGE: {
                    const auto* setting =
                        static_cast<const POWERBROADCAST_SETTING*>(eventData);
                    if (!setting ||
                        !IsEqualGUID(setting->PowerSetting,
                                     kGuidConsoleDisplayState) ||
                        setting->DataLength < sizeof(DWORD)) {
                        break;
                    }
                    DWORD state = 0;
                    memcpy(&state, setting->Data, sizeof(state));
                    if (state > 2) break;
                    // Only a change is news. Windows repeats the state it is
                    // already in - locking with Windows+L sends "on" to a screen
                    // that never went off - and the worker read the repeat as
                    // the screen coming back, which turned on a TV the service
                    // was holding off for a person: ten seconds after locking,
                    // with nobody touching anything, in the test of the start
                    // after an update. The worker acts on the state it reads,
                    // so a repeat has nothing to tell it in any case.
                    if (state == g_displayState.load()) break;
                    // Every change, whether or not anything acts on it, so a
                    // screen woken by the TV's signal dropping and returning can
                    // be told from one woken by a person.
                    static const char* const kStates[] = {"off", "on", "dimmed"};
                    Log("windows: screen %s", kStates[state]);
                    g_displayState = state;
                    if (g_displayEvent) SetEvent(g_displayEvent);
                    break;
                }

                default:
                    break;
            }
            return NO_ERROR;

        // A display appearing or going away, which is what a TV dropping and
        // restoring its HDMI signal looks like to Windows. The name carries the
        // display's model code, so one television can be told from another in
        // the log - and an arrival also asks the pump to read this PC's address
        // again, since a cable that has moved is the one thing that changes it
        // without this service being involved.
        case SERVICE_CONTROL_DEVICEEVENT:
            if ((eventType == DBT_DEVICEARRIVAL ||
                 eventType == DBT_DEVICEREMOVECOMPLETE) && eventData) {
                const auto* device =
                    static_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(eventData);
                char model[32] = {};
                if (device->dbcc_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    // \\?\DISPLAY#TCL0101#... - the part between the first two #.
                    const wchar_t* start = wcschr(device->dbcc_name, L'#');
                    for (int i = 0; start && start[1 + i] && start[1 + i] != L'#' &&
                                    i < 31; ++i) {
                        const wchar_t c = start[1 + i];
                        model[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
                    }
                }
                Log("windows: display %s%s%s",
                    eventType == DBT_DEVICEARRIVAL ? "connected" : "disconnected",
                    model[0] ? ", " : "", model);
                // A display arriving may be another television, or the same one
                // on another HDMI port, and either changes this PC's physical
                // address. Read again by the pump, not here.
                if (eventType == DBT_DEVICEARRIVAL) g_displayArrived = true;
            }
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, HandlerEx, nullptr);
    if (!g_statusHandle) return;

    SetState(SERVICE_START_PENDING, 30000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    LoadSettings();
    // The version first, because "which build is running" is the question
    // every measurement has to answer before it can be trusted - a build in
    // the repo is not the one the SCM runs - and the log is the one place the
    // answer is always to hand.
    // Stamped before anything can ask, because everything that reads event 1074
    // needs to know which events are older than this run of the service.
    {
        FILETIME now = {};
        GetSystemTimeAsFileTime(&now);
        g_startedAt = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) |
                      now.dwLowDateTime;
    }
    Log("service starting, version %s", CEC_VERSION_STRING);
    // Before anything else asks what a shutdown was: the answer is in the
    // event log from the last one, and reading it now means the first shutdown
    // after this start is already understood.
    LearnShutdownWords();
    if (ModernStandby()) {
        Log("this PC uses Modern Standby, so sleeping arrives as the screen "
            "turning off; the screen-off setting is what covers sleep here");
    }

    // Before the pipe server and well before the pump, both of which touch it.
    // A key arriving while this did not yet exist would be queued and not
    // signalled - harmless, since the next key collects it too, but there is no
    // reason to leave the window open. Failure here is not worth aborting for:
    // everything except the TV remote and the live volume readout works
    // without them.
    CreateTrayEvents();

    // Report running before the display work: at cold boot the GPU driver may
    // take a while, and the SCM must not be left waiting on us.
    SetState(SERVICE_RUNNING);

    if (!g_pipeServer.Start(cec::kPipeName, HandlePipeRequest,
                            [](const char* message) { Log("%s", message); })) {
        // Not fatal: power control is the point, the tray is a convenience.
        Log("pipe server did not start; tray app will be unable to connect");
    }
    // Also not fatal, and with a softer failure than the one above: a tray that
    // cannot reach this falls back to asking over the command pipe, which is
    // what it did before this existed. The only loss is the staleness this was
    // added to remove.
    if (!g_readPipeServer.Start(cec::kReadPipeName, HandleReadRequest,
                                [](const char* message) { Log("%s", message); })) {
        Log("read-only pipe did not start; the menu will fall back to the "
            "command pipe and may lag a switch by a second or two");
    }

    // No resume event fires on a cold boot, so the initial power-on is sent
    // here, once the hardware is actually ready.
    // The pump starts once the TV has been told this PC is here, and hot,
    // because a TV that has just seen this device appear is about to ask about
    // it. From here on the process stays reachable for as long as it runs.
    //
    // Before the name is re-sent, not after, although that once seemed the
    // tidier order. The re-sending takes about ten seconds, and the pump is
    // what collects volume answers and recognises the TV, so with it waiting
    // behind the name the first volume question after a service start went
    // unread for all of that time and the slider did not appear. The pump
    // only ever tries the lock, so it cannot hold the name up.
    //
    // Except after a restart Windows Update made by itself, which the shutdown
    // path records. Nobody pressed anything, the TV was usually off for the
    // night, and turning it on here left it on. Read and cleared before the
    // attach, so it is honoured once and a slow driver cannot strand it. The
    // device still attaches and announces; only <Image View On> and the claim
    // to the screen are left out. See LeaveTvForAPerson for how it is turned
    // on again in the morning.
    const bool afterUpdate = ReadNumber(L"StartAfterUpdate") != 0;
    if (afterUpdate) DeleteValue(L"StartAfterUpdate");
    const bool attached = g_session.AttachWithRetry(60, 2000, g_stopEvent);
    const bool turnOn =
        attached && !(afterUpdate && g_session.LeaveTvForAPerson());
    const bool turnedOn = turnOn && g_session.SendPowerOn(false);
    GoHot();
    g_pumpThread = CreateThread(nullptr, 0, QueryPump, nullptr, 0, nullptr);
    if (!g_pumpThread) Log("query pump did not start; the input name may be wrong");
    // Tried again if the first did not land, as the screen-on path does. A
    // start is the one moment nothing else will: there is no wake to follow
    // it and no screen coming on, so a single frame whose status the adapter
    // never reported left the TV off at boot. After the pump has started, so
    // the television's questions are answered while this tries, and never
    // when the first one worked.
    if (turnOn && !turnedOn) g_session.SendPowerOnWithRetry(g_stopEvent);
    // The TV may not have been listening yet when we first announced.
    if (attached) g_session.ReassertOsdName(3, 3000, g_stopEvent);

    // After the opening sequence, for the same reason as the pump. Registering
    // delivers the current display state immediately, and that first
    // notification is harmless by construction: the display is on, and the
    // display-on path does nothing unless this service turned the TV off.
    g_displayEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_prepareEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_userWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_displayThread =
        CreateThread(nullptr, 0, DisplayFollowThread, nullptr, 0, nullptr);
    if (g_displayEvent && g_displayThread) {
        g_displayNotify = RegisterPowerSettingNotification(
            g_statusHandle, &kGuidConsoleDisplayState,
            DEVICE_NOTIFY_SERVICE_HANDLE);
    }
    if (!g_displayNotify) {
        Log("display power notifications unavailable; the TV will follow sleep "
            "and shutdown only");
    }

    // Displays connecting and disconnecting, for the log only. See
    // SERVICE_CONTROL_DEVICEEVENT.
    DEV_BROADCAST_DEVICEINTERFACE_W monitors = {};
    monitors.dbcc_size = sizeof(monitors);
    monitors.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    monitors.dbcc_classguid = kGuidMonitorInterface;
    g_monitorNotify = RegisterDeviceNotificationW(g_statusHandle, &monitors,
                                                  DEVICE_NOTIFY_SERVICE_HANDLE);

    WaitForSingleObject(g_stopEvent, INFINITE);

    // Stop the notifications before the thread that consumes them, so nothing
    // signals an event that is about to go away.
    if (g_displayNotify) {
        UnregisterPowerSettingNotification(g_displayNotify);
        g_displayNotify = nullptr;
    }
    if (g_monitorNotify) {
        UnregisterDeviceNotification(g_monitorNotify);
        g_monitorNotify = nullptr;
    }

    // Both wait on the same stop event, so they are already unwinding.
    if (g_pumpThread) {
        WaitForSingleObject(g_pumpThread, 3000);
        CloseHandle(g_pumpThread);
        g_pumpThread = nullptr;
    }
    if (g_displayThread) {
        // Longer than the pump: this one may be in the middle of a verified
        // standby, which is allowed several seconds of its own.
        WaitForSingleObject(g_displayThread, 10000);
        CloseHandle(g_displayThread);
        g_displayThread = nullptr;
    }

    // Nothing will be minding this adapter once the threads are gone, and an
    // adapter nobody is minding refuses the rest of the bus's broadcasts.
    g_session.LeaveBusQuiet();

    g_pipeServer.Stop();
    g_readPipeServer.Stop();
    if (g_remoteKeyEvent) {
        // Signalled once on the way out so a tray app blocked on it wakes,
        // finds an empty queue and a closed pipe, and exits its own loop rather
        // than sitting on a handle to an object nobody will ever set again.
        SetEvent(g_remoteKeyEvent);
        CloseHandle(g_remoteKeyEvent);
        g_remoteKeyEvent = nullptr;
    }
    if (g_volumeEvent) {
        // The same, for the same reason.
        SetEvent(g_volumeEvent);
        CloseHandle(g_volumeEvent);
        g_volumeEvent = nullptr;
    }
    Log("service stopping");
    SetState(SERVICE_STOPPED);
    // Deliberately not closing g_stopEvent: the control handler runs on another
    // thread and may still reference it. The process is exiting anyway, so the
    // handle costs nothing, whereas closing it under a concurrent SetEvent is a
    // real handle-reuse bug.
}

// ------------------------------------------------------ install / uninstall

bool ExePath(std::wstring& out) {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    out.assign(path, n);
    return true;
}

void PrintLastError(const char* what) {
    const DWORD code = GetLastError();
    wprintf(L"%hs failed (error %lu)\n", what, code);
    if (code == ERROR_ACCESS_DENIED) {
        wprintf(L"Run this from an elevated command prompt.\n");
    }
}

// Lets the person at the keyboard start this service, and nothing else.
//
// The tray app's "Launch Service" exists for when the service is not running,
// and starting a service is normally an administrator's right, so without this
// every use of it raises a consent prompt for a Windows tool whose name says
// nothing about this program. The grant is deliberately a single right,
// SERVICE_START, to interactive users only: the people actually signed in at
// the machine, not anything reaching it over the network.
//
// What that allows is narrow. It cannot stop the service, change how it is
// configured, or point it at another binary, and the service starts with
// Windows anyway, so the only state it can bring about is the one the machine
// was going to be in regardless.
void GrantStartToInteractiveUsers(SC_HANDLE service) {
    // Read what is there, add one entry, write it back. Building a descriptor
    // from scratch would throw away whatever Windows or an administrator has
    // put on the service.
    DWORD needed = 0;
    QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, nullptr, 0,
                               &needed);
    if (!needed) return;
    std::vector<BYTE> buffer(needed);
    PSECURITY_DESCRIPTOR current = buffer.data();
    if (!QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, current,
                                    needed, &needed)) {
        return;
    }

    LPWSTR text = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
            current, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text,
            nullptr)) {
        return;
    }
    std::wstring sddl = text;
    LocalFree(text);

    // RP is SERVICE_START and IU is the interactive users group.
    const wchar_t* kStartForInteractive = L"(A;;RP;;;IU)";

    // Install runs again on every upgrade and every repair, and a descriptor
    // that grows another identical entry each time is a mess waiting to happen.
    if (sddl.find(kStartForInteractive) != std::wstring::npos) return;
    // Only a DACL was asked for, so that is all this string should hold.
    if (sddl.rfind(L"D:", 0) != 0) return;
    // Appended rather than inserted at the front: an allow entry placed ahead
    // of a deny entry would quietly overrule it, and the order a system hands
    // back is the order it means.
    sddl += kStartForInteractive;

    PSECURITY_DESCRIPTOR updated = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &updated, nullptr)) {
        return;
    }
    if (!SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, updated)) {
        // Not fatal. The tray app falls back to asking for elevation, which is
        // how it worked before this existed.
        wprintf(L"Could not grant start permission (error %lu).\n",
                GetLastError());
    }
    LocalFree(updated);
}

int Install() {
    std::wstring exe;
    if (!ExePath(exe)) {
        wprintf(L"Could not determine executable path.\n");
        return 1;
    }
    const std::wstring command = L"\"" + exe + L"\"";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        PrintLastError("OpenSCManager");
        return 1;
    }

    SC_HANDLE service = CreateServiceW(
        scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,   // earliest a user-mode service can start
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr,              // LocalSystem
        nullptr);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        // Already registered: point the existing registration at this binary
        // instead of failing. That makes install idempotent, which is what
        // lets an upgrade and a repair both run without removing first - and
        // removing first is the step that can strand the service marked for
        // deletion until a reboot.
        service = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (service) {
            ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 command.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr, kDisplayName);
        }
    }
    if (!service) {
        PrintLastError("CreateService");
        CloseServiceHandle(scm);
        return 1;
    }

    // Materialise the defaults so the settings exist somewhere discoverable
    // rather than living only as fallbacks in code.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const struct { const wchar_t* name; DWORD value; } defaults[] = {
            {L"LoggingEnabled", 1},
            {L"StandbyOnShutdown", 1},
            {L"StandbyOnSleep", 1},
            {L"StandbyOnDisplayOff", 1},
            {L"ForwardRemoteKeys", 1},
            {L"VolumeControlsPc", 0},
            {L"DiagnosePower", 0},
            {L"LogCecTraffic", 0},
            {L"SuspendSettleMs", 2500},
        };
        for (const auto& setting : defaults) {
            DWORD existing = 0;
            DWORD size = sizeof(existing);
            // Do not clobber a choice the user already made.
            if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, setting.name,
                             RRF_RT_REG_DWORD, nullptr, &existing,
                             &size) != ERROR_SUCCESS) {
                RegSetValueExW(key, setting.name, 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&setting.value),
                               sizeof(setting.value));
            }
        }
        RegCloseKey(key);
    }

    SERVICE_DESCRIPTIONW description = {const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    // Restart on unexpected failure rather than silently staying dead.
    SC_ACTION actions[3] = {};
    actions[0] = {SC_ACTION_RESTART, 5000};
    actions[1] = {SC_ACTION_RESTART, 30000};
    actions[2] = {SC_ACTION_RESTART, 60000};
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = 86400;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    // So the tray app's own "Launch Service" needs no consent prompt.
    GrantStartToInteractiveUsers(service);

    wprintf(L"Installed. Starting...\n");
    int result = 0;
    if (StartServiceW(service, 0, nullptr)) {
        wprintf(L"Started. Log: %s\n", LogPath().c_str());
    } else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        wprintf(L"Already running.\n");
    } else {
        // Reported rather than swallowed: the installer decides what to show
        // the user from this exit code, and a service that registered but
        // never started is a failure however tidy the registration looks.
        PrintLastError("StartService");
        result = 1;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return result;
}

int Uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        PrintLastError("OpenSCManager");
        return 1;
    }
    SC_HANDLE service =
        OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        PrintLastError("OpenService");
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS status = {};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        wprintf(L"Stopping...\n");
        // Wait for it to actually stop rather than guessing at a duration.
        // DeleteService on a service that is still stopping leaves it "marked
        // for deletion" until the next reboot, and while it is in that state a
        // reinstall fails - so a hurried uninstall is what makes a later
        // install impossible. The preshutdown path can take a few seconds, so
        // the wait is generous.
        for (int waited = 0; waited < 30000; waited += 250) {
            if (!QueryServiceStatus(service, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(250);
        }
    }

    int result = 0;
    if (DeleteService(service)) {
        wprintf(L"Uninstalled.\n");
    } else if (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) {
        wprintf(L"Already removed; takes effect on reboot.\n");
    } else {
        PrintLastError("DeleteService");
        result = 1;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return result;
}

// Runs the same logic in the foreground, so the sleep/wake behaviour can be
// exercised without installing anything.
int RunConsole() {
    wprintf(L"Running in console mode. Log: %s\n", LogPath().c_str());
    Log("console mode starting");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_session.AttachWithRetry(5, 2000, g_stopEvent)) {
        wprintf(L"Attached. Sending power-on.\n");
        g_session.SendPowerOn();
    } else {
        wprintf(L"Could not attach to a CEC-capable port.\n");
    }
    wprintf(L"Press Ctrl+C to exit.\n");
    WaitForSingleObject(g_stopEvent, INFINITE);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2) {
        const std::wstring arg = argv[1];
        if (arg == L"install")   return Install();
        if (arg == L"uninstall") return Uninstall();
        if (arg == L"console")   return RunConsole();
        wprintf(L"Usage: CecService [install|uninstall|console]\n");
        return 1;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        wprintf(L"This is a Windows Service.\n\n"
                L"  CecService install     install and start it\n"
                L"  CecService uninstall   stop and remove it\n"
                L"  CecService console     run in the foreground for testing\n");
        return 1;
    }
    return 0;
}
