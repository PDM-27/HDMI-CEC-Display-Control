// CecTray - user-session companion to CecService.
//
// The service runs as SYSTEM in Session 0 and therefore cannot show any UI at
// all; this process provides the tray icon and asks the service to do things
// over a named pipe.
//
// One hidden window and one tray icon, idle in GetMessage almost all the time.
// Its few timers are slow (the half-minute service watch, the keep-awake
// renewal) or run only while something is in use (an open panel, a held key,
// the idle pointer), and the pipe is used when there is something to do. The
// window is hidden rather than message-only, which matters - see the note where
// it is created.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <richedit.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <versionhelpers.h>
#include <wtsapi32.h>
#include <powrprof.h>

#include <devicetopology.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "../CecTest/PipeProtocol.h"
#include "PointerHider.h"
#include "VolumePanel.h"
#include "resource.h"

// Without this the app gets comctl32 v5, and Windows draws its buttons,
// menus and dialogs in the pre-XP classic style - which is exactly what it
// looked like. Requesting v6 opts into the themed controls the rest of the
// system uses.
#pragma comment(linker,                                                    \
                "\"/manifestdependency:type='win32' "      \
                "name='Microsoft.Windows.Common-Controls' "       \
                "version='6.0.0.0' "                              \
                "processorArchitecture='*' "                      \
                "publicKeyToken='6595b64144ccf1df' "              \
                "language='*'\"")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "powrprof.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"CecTrayWindow";
constexpr wchar_t kAppName[] = L"HDMI-CEC Display Control";
// The Windows service this app is a front end for. Named here as well as in the
// pipe's own name because this app now asks the Service Control Manager about
// it, which is a different question from whether the pipe answers.
constexpr wchar_t kServiceName[] = L"CecService";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CecTray";
constexpr wchar_t kAppKey[] = L"Software\\CecTray";
constexpr wchar_t kShortcutName[] =
    L"HDMI-CEC Display Control.lnk";

// Bumped whenever an existing Start menu shortcut needs rewriting. Version 2
// carries an explicit icon; version 1 was written before the app had one.
constexpr DWORD kShortcutVersion = 2;

constexpr UINT kTrayMessage = WM_APP + 1;

// Posted by the command worker when a request never reached the service. The
// balloon itself is raised on the UI thread: Shell_NotifyIcon from a worker
// probably works, and "probably" is not a good enough reason to find out on
// someone else's machine.
constexpr UINT kServiceGoneMessage = WM_APP + 2;

// Carries a volume level from the command worker to the panel, which lives on
// the UI thread. wParam is 0-100, kVolumeUnknown for no answer, or
// kVolumeRefused for a set that said it will not say; lParam is the mute flag.
constexpr UINT kVolumeReadoutMessage = WM_APP + 3;
constexpr WPARAM kVolumeUnknown = 0xFFFF;
constexpr WPARAM kVolumeRefused = 0xFFFE;

// Posted when the volume buttons switch between the TV and this PC for a
// reason other than the menu item - in practice a different television with its
// own setting. The readout on screen was about the old target.
constexpr UINT kVolumeRoutingMessage = WM_APP + 4;
// Whoever owns volume on this bus has answered <Set Audio Volume Level> with
// <Feature Abort>, so no level sent to it will ever land. Set from the worker
// when the service says so and read on the UI thread, hence an atomic. It is
// never cleared once set: the message is a statement about the device, and the
// device does not change while it is plugged in. See VolumePanel::
// SetLevelsSettable and g_levelCommandWorks, which this also turns off.
// Mirrors the service, and deliberately does not latch.
//
// It did latch, on the reasoning that a device refusing an absolute level is a
// fact about that device. True, and it is the service's job to remember, not
// this one's: the service forgets it when the television changes, while the
// tray outlives every service restart and every television. So the first set
// that refused hid the volume slider for every set afterwards, until somebody
// restarted the tray. Reading the service's answer each time is both simpler
// and right, and the service's own flag does not flap, so neither does this.
std::atomic<bool> g_levelsRefused{false};
// Sent once, when that becomes true. Deliberately not the routing message,
// which withdraws the readout before asking for it again: that is right when
// volume moves between the television and this PC, and wrong here, where the
// level just reported is good and only the thumb is going away.
constexpr UINT kLevelsRefusedMessage = WM_APP + 14;

// Posts it on the edge, from wherever the news arrives first. Both carriers are
// wanted: the status line is the one that is right after a tray restart, and the
// volume answer is the one that is quick.
void NoteLevelsRefused(const std::string& reply);

// Posted when Windows' sound output may have changed, from the endpoint
// notification thread or when the service names a different television. The
// UI thread looks and decides. See OutputIsElsewhere.
constexpr UINT kAudioOutputMessage = WM_APP + 11;

// Posted by the worker when the service did not answer, so the icon's mark is
// brought up to date from the UI thread, which is the only one that may touch
// the tray icon.
constexpr UINT kServiceCheckMessage = WM_APP + 5;

// Posted by the command worker as volume steps go out and questions are asked,
// in the order they reached the bus. wParam on a step is 1 for up, 0 for down.
// See VolumeFollow.
constexpr UINT kVolumeStepMessage = WM_APP + 6;
constexpr UINT kVolumeAskedMessage = WM_APP + 7;
// A level sent for a knob or strip has gone out, or failed to. See VolumeBurst.
constexpr UINT kVolumeLevelSentMessage = WM_APP + 8;

// Posted by the keyboard hook thread for a volume key. wParam is the key,
// lParam is 1 for pressed and 0 for let go.
constexpr UINT kVolumeKeyMessage = WM_APP + 9;

// Posted by the event thread each time it starts listening to the service,
// which is the first moment a volume answer can be collected. See
// kWarmReadoutTimer.
constexpr UINT kServiceListeningMessage = WM_APP + 10;

// Another PC was asked to take the screen and did not. See OnCommand.
constexpr UINT kHandOverFailedMessage = WM_APP + 12;

// Whether the television reports its volume, learned from what it does and
// then remembered, so the slider stays put instead of coming and going.
//
// The slider used to follow the latest answer alone, and three unanswered
// questions were enough to take it away. Misses happen - an answer a little
// late, a busy bus - and they added up, so it vanished at random and came back
// a moment later. Now one real answer settles it. From then on the slider
// stays, and a question that goes unanswered leaves it on the last level.
//
// It is taken away only on evidence: the set refusing outright, or a long run
// of silence, which is a television that has gone rather than one that was
// slow. And it is forgotten when the display changes or the PC wakes, since
// that may be a different television. Forgetting hides nothing; it only means
// the next few answers have to confirm it again.
//
// A set known not to report is not polled while the panel is open. Opening
// the panel still asks it once, which is how a wrong "no" - a TV that happened
// to be off at the time - corrects itself.
//
// The service also remembers, for each television, whether it has ever
// answered (g_tvReportsVolume). When it has, the patient limit applies from the
// first question after a wake, so a set that is still starting up does not
// lose the slider to its first few slow answers.
//
// UI thread only.
enum class VolumeSupport { kUnknown, kYes, kNo };
VolumeSupport g_volumeSupport = VolumeSupport::kUnknown;
int g_volumeMisses = 0;
constexpr int kUnknownMissLimit = 3;    // a second or two of a new set's silence
constexpr int kKnownMissLimit = 12;     // several seconds of a known set's

// Following the television between its answers.
//
// Every question and answer costs about as much bus time as a click, so during
// a run of fast clicks the TV can only be asked every few steps, and the number
// sat still in between. It cannot be asked more often without slowing the
// clicks themselves, which matter more. But each click's press is a step the TV
// takes the moment it hears it, and the service confirms when a press went out,
// so the number moves one step per confirmed press and every real answer puts
// it back on the TV's own level.
//
// This is prediction, and an earlier version of it was dropped for running ahead
// of the set and springing back. That one counted clicks, including ones still
// waiting in the queue, and corrected only on release. This one differs in the
// ways that caused that:
//
// - It counts presses that have reached the TV, never clicks still queued.
// - Each answer becomes the new starting point, so nothing accumulates.
// - An answer is treated as a range, not a level. It includes every step that
//   went out before its question, and some unknown number of those after it -
//   always the earliest, because the TV hears them in order. That bounds where
//   the TV can be now. A count inside the range is left exactly where it is, so
//   nothing on screen moves; one outside is moved the least distance that puts
//   it back. The first version of this took each answer as the level now, and
//   during fast clicking a press was nearly always still between the question
//   and the reply - so every answer pulled the number back a step, and the next
//   click pushed it on again.
// - It checks itself. Two answers in a row that the count had to be corrected
//   for, with steps sent, and the set does not step once per press: counting
//   stops and the number simply follows answers, as before. Changes with no
//   steps sent are corrected but not held against it, since those are the TV
//   remote's.
// - Only the first press of a click counts. During a hold the TV repeats on its
//   own at a rate this app does not know, and the hold's questions follow that.
//
// UI thread only; the worker's messages arrive in the order it saw things.
struct VolumeFollow {
    bool trusted = true;
    bool baseKnown = false;
    int base = 0;             // the level the TV last reported
    bool muted = false;       // and its mute flag at the time
    long steps = 0;           // confirmed steps, a running total
    long stepsAtBase = 0;     // the total when the level on screen was set
    // The steps since the latest question went out, and the lowest and highest
    // their running sum has been, which is what bounds the next answer.
    long sinceQuestion = 0;
    long lowestSince = 0;
    long highestSince = 0;
    int contradictions = 0;   // answers in a row the count had to be moved for
};
VolumeFollow g_follow;

// When the TV last said what its volume is. The count keeps the number right
// through this app's own steps, but not through the TV's remote, so a level
// sent for a knob is only worked out from an answer this recent. UI thread.
DWORD g_lastAnswerTick = 0;

// Whether the last first press had a question go out ahead of it, which tells
// a burst whether that tap is in the answer or still to be added. UI thread.
bool g_lastPressAskedFirst = false;

// True from a volume press until its release.
//
// Nothing asks about volume in between except the hold's own questions. A click
// is a press and a release a moment apart, and a question queued between the
// two made the release wait behind it, so every click held the key down longer
// - long enough for the TV to start repeating it, which is what once made the
// buttons feel slower. A real hold is different: the panel re-sends the press
// every 300ms, and a question sent right after one of those can only ever make
// the release wait for a single frame, the same as the re-send itself does.
// Set on the UI thread; read wherever a question is considered.
std::atomic<bool> g_buttonHeld{false};

// One outstanding volume question at a time.
//
// Without this, holding a button would queue a question behind every re-sent
// press, and a television that answers slowly would build a backlog that
// outlives the pressing. With it, a slow set simply updates the number less
// often, which is the failure anyone would choose.
//
// The answer comes back through the service's volume event rather than as the
// reply to the question, so it can go missing - a service restarting mid-way,
// say. A question older than this counts as gone, rather than holding the flag
// for ever and silencing the readout.
std::atomic<bool> g_volumeQueryPending{false};
std::atomic<DWORD> g_volumeAskTick{0};
constexpr DWORD kVolumeQuestionExpiryMs = 1500;

// Set when a question was wanted while another was still outstanding.
//
// The one-question rule on its own lost exactly the answer that mattered. If a
// query was in flight when the last press or mute arrived, the new one was
// dropped - and the one in flight had left *before* that last action, so it
// came back describing the state before it. Nothing asked again. This is why
// quick mute toggles left the readout showing the wrong one. With the flag, a
// dropped question becomes one more question once the current answer lands, so
// the display always ends on the state after the final action.
std::atomic<bool> g_volumeQueryDirty{false};

// Declared here because the command worker, which is defined first, uses them.
// duringHold is for the hold's own questions, sent right behind a re-sent press;
// every other question waits for the release. See g_buttonHeld.
bool AskForVolume(bool duringHold = false);
void VolumeQuestionSettled();
void PostVolumeReply(const std::string& reply);
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuTvOn = 100,
    kMenuTvStandby,
    kMenuKeepAwake,
    kMenuLogging,
    kMenuStandbyShutdown,
    kMenuStandbySleep,
    kMenuStandbyDisplayOff,
    kMenuForwardRemoteKeys,
    kMenuVolumeToPc,
    kMenuKeyboardVolume,
    kMenuHotkeyEnabled,
    // Not on the menu. Reachable only by right-clicking the toggle above,
    // which posts it - so it looks unused and is not.
    kMenuHotkeyChange,
    // Likewise, from right-clicking "TV remote controls this PC".
    kMenuRemoteKeysChange,
    kMenuHidePointer,
    // Likewise, from right-clicking "Hide the mouse pointer when idle".
    kMenuHidePointerOptions,
    kMenuStartWithWindows,
    kMenuOpenLog,
    kMenuGuide,
    // From the line that says there is no CEC display. It opens the guide at
    // the answer rather than doing anything to the hardware.
    kMenuNoDisplayHelp,
    kMenuLaunchService,
    kMenuExit,
    // One per HDMI input, numbered from here. Built when the menu opens, from
    // what the service says is plugged in, so there is no fixed list.
    kMenuFirstInput = 200,
    // Fifteen, which is past both limits that apply: a physical address allows
    // fifteen HDMI ports, and a CEC bus has room for about eleven devices,
    // because addresses come from small pools per type of device.
    kMenuLastInput = kMenuFirstInput + 14,
};

NOTIFYICONDATAW g_icon = {};
HWND g_window = nullptr;
HINSTANCE g_instance = nullptr;

// Explorer broadcasts this when the taskbar is (re)created - after it crashes
// and restarts, or after a shell upgrade. Every tray icon is destroyed with the
// old taskbar and has to be added again, and an app that does not listen simply
// disappears from the tray until it is restarted.
UINT g_taskbarCreated = 0;
tray::VolumePanel g_volumePanel;

// Whether the volume buttons turn down this PC rather than the television. Kept
// here rather than in the volume-routing section below because the command
// queue refreshes it at startup, and the queue is declared first.
std::atomic<bool> g_volumeToPc{false};

// Whether the service has heard this television report its volume before, on
// this day or an earlier one. It only ever makes the readout more patient - see
// g_volumeSupport - and never shows anything by itself.
std::atomic<bool> g_tvReportsVolume{false};

// Whether Windows is playing sound on something other than the television:
// headphones, a USB or Bluetooth speaker, another monitor. Volume is then left
// to Windows, exactly as if "Volume buttons change this PC's volume" were on,
// without that setting being touched, so it all comes back by itself when the
// TV is the output again. See OutputIsElsewhere. Written on the UI thread.
std::atomic<bool> g_outputElsewhere{false};

// Where the volume controls act: this PC's own volume, by the setting or
// because sound is going somewhere other than the TV, or else the TV.
bool VolumeGoesToPc() { return g_volumeToPc.load() || g_outputElsewhere.load(); }

// The television the service last recognised, as the EDID manufacturer word
// and product code packed together, or 0 when it has not named one. Taken from
// STATUS on the worker and read on the UI thread.
std::atomic<uint32_t> g_tvSinkId{0};

// One check at a time is enough, however many notifications arrive together:
// plugging something in raises several.
std::atomic<bool> g_outputCheckPending{false};

void RequestOutputCheck() {
    if (g_outputCheckPending.exchange(true)) return;
    if (!g_window || !PostMessageW(g_window, kAudioOutputMessage, 0, 0)) {
        g_outputCheckPending = false;
    }
}

// Whether a volume question is worth a frame on the bus: the set has not
// been judged silent, or the service knows it has answered before, in which
// case the judgement was most likely a moment after a service restart.
// Defined below g_volumeSupport's section; UI thread only.
bool MayAskVolume();

// How long a caller waits for the service to be free.
//
// The service answers one caller at a time, and this app has three that can
// want it at once - the command worker, the menu, and the event thread - so
// busy is the ordinary case rather than a failure. Five seconds covers the
// longest thing the service does with the pipe held in ordinary use, turning
// the TV on from standby.
constexpr DWORD kPipeBusyWaitMs = 5000;


// Defined below, with the rest of the status parsing.
bool StatusFlag(const std::string& status, const char* key);
void TakeStatus(const std::string& status);

// Defined with the menu, which is where what they answer is drawn. Declared
// here because the command worker is the thread that asks, and it comes first.
struct InputEntry;
std::vector<InputEntry> AskForInputs();
void KeepAnswers(const std::string& status, std::vector<InputEntry> inputs);

// A tray balloon, built on its own copy of the icon data rather than on g_icon,
// so that a worker thread can raise one without racing the UI thread over the
// shared struct. Only NIF_INFO is set, so the icon itself is left alone.
void ShowBalloon(const wchar_t* title, const std::wstring& text, DWORD flags);

// Defined below, with the guide window that first needed it.
bool AppsUseDarkTheme();

void ShowBalloon(const wchar_t* title, const std::wstring& text, DWORD flags) {
    if (!g_window) return;
    NOTIFYICONDATAW balloon = {};
    balloon.cbSize = sizeof(balloon);
    balloon.hWnd = g_window;
    balloon.uID = kTrayId;
    balloon.uFlags = NIF_INFO;
    wcscpy_s(balloon.szInfoTitle, title);
    wcsncpy_s(balloon.szInfo, text.c_str(), _TRUNCATE);
    balloon.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

// ------------------------------------------------------------------ pipe

// One request, one response, then the handle closes. Connectionless by design:
// there is no session to lose, nothing to reconnect, and a service restart is
// invisible to us. Returns an empty string if the service is not reachable.
std::string SendRequestTo(const wchar_t* name, const std::string& request) {
    // Tried until it connects or the wait runs out. This used to try twice and
    // give up, and a menu opened just after hovering - which queues three
    // requests of its own - lost the race for the pipe often enough to say
    // "Service not running" about a service that was running perfectly well.
    const DWORD start = GetTickCount();
    for (;;) {
        HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            // Busy is the one case worth waiting for; anything else means the
            // service is not running and retrying will not help.
            if (GetLastError() != ERROR_PIPE_BUSY) return {};
            const DWORD elapsed = GetTickCount() - start;
            if (elapsed >= kPipeBusyWaitMs) return {};
            WaitNamedPipeW(name, kPipeBusyWaitMs - elapsed);
            continue;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        std::string line = request + "\n";
        DWORD written = 0;
        WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written,
                  nullptr);

        char buffer[cec::kPipeMaxMessage] = {};
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

std::string SendRequest(const std::string& request) {
    return SendRequestTo(cec::kPipeName, request);
}

// The same thing on the questions-only pipe, which has a server thread of its
// own and so is never stuck behind a command. See kReadPipeName.
std::string SendReadRequest(const std::string& request) {
    return SendRequestTo(cec::kReadPipeName, request);
}

// A command that never reached the service, said out loud once in a while.
//
// This exists because two failures in a row were invisible: a hotkey Windows had
// refused, and a service that was stopped. Both looked exactly like a television
// declining to switch, and each cost a round of testing to tell apart. The
// button did nothing and nothing said why.
//
// Volume is excluded. It is pressed several times a second, and a balloon per
// press would be a worse bug than the one being reported.
void ReportUnreachable(const std::string& request) {
    // Keep-awake too: its renewal goes every two minutes, and the half of it
    // that keeps this PC awake works with no service at all, so nothing went
    // anywhere worth a balloon.
    if (request.find("volume") != std::string::npos ||
        request.find("release") != std::string::npos ||
        request.find("prepare") != std::string::npos ||
        request.find("user-active") != std::string::npos ||
        request.find("screen-on") != std::string::npos ||
        request.find("keep-awake") != std::string::npos) {
        return;
    }
    // Rate limited, so a run of failures is one balloon rather than a stack.
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (lastTick && now - lastTick < 30000) return;
    lastTick = now;
    if (g_window) PostMessageW(g_window, kServiceGoneMessage, 0, 0);
}

// Pipe work runs on a worker thread, never on the UI thread.
//
// A command costs a pipe round trip plus whatever CEC takes on the wire - tens
// to hundreds of milliseconds. Doing that inside the button handler froze the
// panel after a single click, which read as the app being broken. A queue also
// keeps clicks in order, which matters for volume: an out-of-order press and
// release would leave the TV repeating the key.
class CommandQueue {
public:
    void Start() {
        worker_ = std::thread([this] {
            for (;;) {
                std::string request;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return stop_ || !pending_.empty(); });
                    if (stop_ && pending_.empty()) return;
                    request = std::move(pending_.front());
                    pending_.pop_front();
                }
                // A leading + or - marks the first press of a volume click,
                // which is a step the TV takes the moment it hears it. Counted
                // only once the service says the frame went out, and posted in
                // the same order as everything else this thread learns, so the
                // readout can move with each step - see VolumeFollow.
                int step = 0;
                if (!request.empty() && (request[0] == '+' || request[0] == '-')) {
                    step = request[0] == '+' ? 1 : -1;
                    request.erase(0, 1);
                }
                // A leading = marks a level sent for a knob or strip, which
                // the UI needs to see leave, in order, before it trusts the
                // question that follows it. See VolumeBurst.
                if (!request.empty() && request[0] == '=') {
                    request.erase(0, 1);
                    const std::string reply = SendRequest(request);
                    if (reply.empty()) ReportUnreachable(request);
                    if (g_window) PostMessageW(g_window, kVolumeLevelSentMessage, 0, 0);
                    continue;
                }

                // An answer the service has signalled. Read here, on the one
                // thread that uses the pipe for volume, rather than by the
                // thread that heard the signal: the pipe serves one caller at
                // a time, and a second caller racing a run of clicks for it
                // lost often enough to keep the number still until they
                // stopped - and two losses in a row looked like the service
                // had gone.
                if (request == "VOLUME-READ") {
                    const std::string reply = SendRequest("VOLUME");
                    if (reply.rfind("OK", 0) == 0) PostVolumeReply(reply);
                    VolumeQuestionSettled();
                    continue;
                }

                // Not a service verb either. It goes through this queue so
                // the question takes its place in order with the presses
                // around it, which is what puts it behind a re-sent press and
                // never between a click's press and release. The answer does
                // not come back here: it arrives through the service's volume
                // event - see ServiceEventThread.
                if (request == "VOLUME-ASK") {
                    const std::string reply = SendRequest("VOLUME ask");
                    if (reply.rfind("OK", 0) != 0) {
                        // Never reached a set, so no answer is coming. To the
                        // readout that is silence, and it is counted as such.
                        if (g_window) {
                            PostMessageW(g_window, kVolumeReadoutMessage,
                                         kVolumeUnknown, 0);
                        }
                        VolumeQuestionSettled();
                    } else if (g_window) {
                        // Where in the run of steps the question went out, so
                        // its answer can be checked against the count.
                        PostMessageW(g_window, kVolumeAskedMessage, 0, 0);
                    }
                    continue;
                }

                // Not a service verb: the queue is also the only thread that
                // may block on the pipe, so the startup refresh rides it.
                if (request == "STATUS-REFRESH") {
                    const std::string status = SendRequest("STATUS");
                    if (!status.empty()) {
                        TakeStatus(status);
                        // Both of the menu's questions answered here, on this
                        // thread, so that opening it draws from what is already
                        // known. See KeepAnswers.
                        KeepAnswers(status, AskForInputs());
                    } else {
                        // Nothing answered, so what was kept is no longer
                        // worth showing: a menu drawn from it would report a
                        // healthy service while the icon says otherwise.
                        KeepAnswers({}, {});
                        // Whether this means the service is gone is the UI
                        // thread's question to ask, because the icon is its to
                        // draw.
                        if (g_window) {
                            PostMessageW(g_window, kServiceCheckMessage, 0, 0);
                        }
                    }
                    continue;
                }
                const std::string reply = SendRequest(request);
                // Which input is on screen has just changed, or was just tried,
                // so what the menu would draw is stale by definition. Refreshed
                // here, on the thread that already has the pipe open and with
                // nobody waiting, so the next menu is right without a person
                // having to hover first.
                if (request.rfind("CMD switch-to", 0) == 0 ||
                    request == "CMD tv-on") {
                    const std::string status = SendRequest("STATUS");
                    if (!status.empty()) {
                        TakeStatus(status);
                        KeepAnswers(status, AskForInputs());
                    }
                }
                if (reply.empty()) {
                    ReportUnreachable(request);
                } else if (reply.rfind("ERR", 0) == 0 &&
                           request.rfind("CMD switch-to", 0) == 0 && g_window) {
                    // Handing the screen to another PC is the one command whose
                    // failure is invisible: the television simply does not
                    // change, and the reason lives on the other machine.
                    //
                    // Two failures, and the service has already told them
                    // apart: a port where nothing answered at all, and a device
                    // that answered a poll during the exchange and threw the
                    // request away. Matched on the word rather than the whole
                    // sentence, so the wording can be improved without this
                    // quietly falling back to the vaguer balloon.
                    const bool ignores =
                        reply.find("ignores") != std::string::npos;
                    PostMessageW(g_window, kHandOverFailedMessage,
                                 ignores ? 1 : 0, 0);
                } else if (step && reply == "OK" && g_window) {
                    PostMessageW(g_window, kVolumeStepMessage, step > 0 ? 1 : 0, 0);
                }
            }
        });
    }

    // Returns whether the request was queued. A caller that is tracking an
    // outstanding request needs to know when one was dropped, or it waits for
    // an answer to a question that was never asked.
    bool Post(const std::string& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A held-down button should not be able to build an unbounded
            // backlog the TV then has to work through after the click stops.
            //
            // A release is exempt, and that exemption is the point of the
            // whole cap being written this way. Everything else here is a
            // volume step: dropping one costs the user a press. Dropping a
            // release leaves the TV repeating a key with nothing on screen
            // still holding it down - the runaway-volume failure - and the
            // retry that would fix it is another release, which a full queue
            // would drop as well. The one message that must never be lost is
            // also the one whose loss the queue cannot recover from.
            if (request.find("release") == std::string::npos &&
                pending_.size() >= kMaxPending) {
                return false;
            }
            // Asking twice for the same thing is not asking for it twice.
            //
            // Somebody who presses the hotkey again because the television has
            // not reacted yet wants the first press to work, not a second full
            // sequence behind it - and each one claims the screen and then
            // spaces out repeats for three seconds, so six presses meant the
            // best part of twenty seconds of the television being told
            // something it had already been told. One log caught exactly that.
            // A press that arrives after the queue has drained still gets its
            // own attempt, which is the retry a person actually means.
            if (request.rfind("CMD tv-on", 0) == 0 ||
                request.rfind("CMD switch-to", 0) == 0) {
                for (const std::string& queued : pending_) {
                    if (queued == request) return true;
                }
            }
            pending_.push_back(request);
        }
        ready_.notify_one();
        return true;
    }

    // Puts a request at the head of the queue, once. For reads that touch
    // nothing on the bus: a reading is worth most the moment it lands, and
    // making it wait behind a run of clicks is what kept the number still
    // until the clicking stopped. Landing between a press and its release does
    // no harm, because a read is a millisecond on the pipe and no frame at all.
    // Once, because a single read already returns the latest answer.
    void PostFirst(const std::string& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const std::string& queued : pending_) {
                if (queued == request) return;
            }
            pending_.push_front(request);
        }
        ready_.notify_one();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

private:
    // Deep enough that ordinary clicking never reaches it, shallow enough that
    // a backlog cannot outlive the clicking that produced it.
    static constexpr size_t kMaxPending = 16;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> pending_;
    bool stop_ = false;
};

CommandQueue g_commands;

// Pulls "key=1" out of a STATUS response. Cheaper and more reliable than a
// separate GET per setting: one request, one consistent snapshot.
bool StatusFlag(const std::string& status, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t at = status.find(needle);
    if (at == std::string::npos) return false;
    const size_t value = at + needle.size();
    return value < status.size() && status[value] == '1';
}

// Takes what STATUS says about volume. Called from the worker as well as the
// menu, so it touches only atomics and leaves the panel to the UI thread.
void TakeStatus(const std::string& status) {
    g_tvReportsVolume = StatusFlag(status, "tvvol");

    // tv=TCL-2E5A-00000001: three letters, the product code, the serial. The
    // letters go back into the five-bits-each word the EDID stores them as, so
    // they can be compared with what the sound driver reports. Anything that
    // does not have exactly that shape leaves the TV unnamed.
    uint32_t sink = 0;
    const size_t tv = status.find(" tv=");
    if (tv != std::string::npos && status.size() >= tv + 4 + 9) {
        const char* key = status.c_str() + tv + 4;
        bool valid = key[3] == '-' && key[8] == '-';
        uint32_t letters = 0;
        for (int i = 0; valid && i < 3; ++i) {
            valid = key[i] >= 'A' && key[i] <= 'Z';
            letters = (letters << 5) | static_cast<uint32_t>(key[i] - 'A' + 1);
        }
        uint32_t product = 0;
        for (int i = 4; valid && i < 8; ++i) {
            const char c = key[i];
            valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
            product = (product << 4) |
                      static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'A' + 10);
        }
        if (valid) sink = (letters << 16) | product;
    }
    // A different television. Everything this app had concluded about the last
    // one is now worthless: whether it reports volume at all, the level the
    // readout was following, and where volume goes. WM_DISPLAYCHANGE covers
    // most moves between sets, but the service's own key is the thing that
    // actually knows, and it can change without the desktop changing at all.
    // The routing handler already does exactly this work, so it is reused
    // rather than copied.
    if (g_tvSinkId.exchange(sink) != sink) {
        RequestOutputCheck();
        if (g_window) PostMessageW(g_window, kVolumeRoutingMessage, 0, 0);
    }

    const bool toPc = StatusFlag(status, "pcvol");
    if (g_volumeToPc.exchange(toPc) != toPc && g_window) {
        PostMessageW(g_window, kVolumeRoutingMessage, 0, 0);
    }

    NoteLevelsRefused(status);
}

void NoteLevelsRefused(const std::string& reply) {
    const bool refused = StatusFlag(reply, "nolevel");
    if (g_levelsRefused.exchange(refused) != refused && g_window) {
        PostMessageW(g_window, kLevelsRefusedMessage, 0, 0);
    }
}

// What is plugged into the television, as the "Switch Input" menu shows it.
//
// Asked for when the menu opens. The service keeps the list current from what
// devices announce and a light check every ten seconds, so this is a question
// it answers from memory rather than a trip to the bus.
struct InputEntry {
    std::wstring port;    // "2.0.0.0", which is what a switch is addressed to
    std::wstring label;   // "HDMI 2 - Chromecast"
    bool onScreen = false;
    bool thisPc = false;
    bool asleep = false;
    // A soundbar or receiver, which occupies an HDMI port without being
    // anything to look at. Listed, so that somebody can see it was found and
    // that volume is going there, and greyed, because there is nothing behind
    // it to put on screen. Asking the television to switch to one did nothing
    // and left "the TV did not switch to 1.0.0.0; nothing there answered" in
    // the log, which is the set being right and us being wrong.
    bool audio = false;
    // Asked to take the screen more than once, while awake, and did nothing.
    // The service decides when it is sure; this is only how the menu is told.
    bool cannotSwitch = false;
};

std::vector<InputEntry> g_inputs;


// The last answers from the service, kept so that opening the menu draws it
// rather than asking for it.
//
// A right click used to make two pipe round trips before a single item was
// drawn, and a menu that takes a moment appears wherever the pointer has got
// to by then. Both answers are refreshed when the pointer reaches the tray
// icon, which happens before the click, on the half-minute watch, and again
// after each menu closes. Nothing here is worth a person waiting for: a
// setting toggled elsewhere shows up a moment later instead of instantly.
std::mutex g_answersMutex;
std::condition_variable g_answersReady;
std::string g_statusAnswer;
std::vector<InputEntry> g_inputsAnswer;
DWORD g_answersTick = 0;
// Bumped every time the answers are replaced, so a caller can tell a fresh set
// from the one it already had without watching the clock.
unsigned g_answersRound = 0;

// How old the kept answers may be before the menu tops them up rather than
// drawing them. Everything that changes on its own - a switch made from another
// machine, a device going to sleep - reaches this app only when it asks, and a
// dot beside the wrong input is the one piece of the menu that is simply wrong
// rather than merely late.
//
// Zero, so the menu always asks. This was 1500, which opened a window a second
// and a half wide where a switch that had already happened was not drawn:
// change the input and open the menu quickly, and the kept answer predated the
// switch. Reported from the field, and the whole lag was this - the television
// announces the change immediately, and the service files it on its next pump
// tick, 150ms while anything is happening.
//
// The large value dates from when the menu's questions took the CEC lock and
// could sit behind an announce for a second, which is what used to make the
// popup appear wherever the pointer had got to. That has not been true since
// the service started answering from a snapshot: the round trip is a named pipe
// and a copy, well under a millisecond, and it never touches the bus. The 250ms
// cap below is the safety net, so the worst case here is what the stale path
// already did - draw what we have - rather than a wait anybody can see.
constexpr DWORD kAnswersFreshMs = 0;

void KeepAnswers(const std::string& status, std::vector<InputEntry> inputs) {
    {
        std::lock_guard<std::mutex> guard(g_answersMutex);
        g_statusAnswer = status;
        g_inputsAnswer = std::move(inputs);
        g_answersTick = GetTickCount();
        ++g_answersRound;
    }
    g_answersReady.notify_all();
}

// How long the menu will wait for a fresh set of answers before drawing what it
// has. Long enough for a pipe round trip the service answers from a snapshot,
// which is well under a millisecond, plus room for the link to be busy; short
// enough that nobody sees it.
constexpr DWORD kAnswersWaitMs = 250;

// Waits for the worker to replace the answers, and gives up quietly.
//
// The worker owns the pipe, and this is why the menu asks through it rather than
// opening a connection of its own. It used to do the latter, and the two
// collided: the pointer arriving at the tray icon starts a refresh, the click
// that follows a fraction of a second later found the answers still stale, and
// its own request then met ERROR_PIPE_BUSY against the refresh already running -
// so it waited, gave up, and drew the stale answer anyway. Which of the two won
// decided whether the dot was on the right input.
void WaitForFreshAnswers() {
    std::unique_lock<std::mutex> lock(g_answersMutex);
    const unsigned before = g_answersRound;
    g_answersReady.wait_for(lock, std::chrono::milliseconds(kAnswersWaitMs),
                            [before] { return g_answersRound != before; });
}

// Milliseconds since the kept answers were taken, or a number past every window
// when nothing has ever been kept.
DWORD AnswersAge() {
    std::lock_guard<std::mutex> guard(g_answersMutex);
    return g_answersTick ? GetTickCount() - g_answersTick : 0xFFFFFFF;
}

std::string LastStatus() {
    std::lock_guard<std::mutex> guard(g_answersMutex);
    return g_statusAnswer;
}

std::vector<InputEntry> LastInputs() {
    std::lock_guard<std::mutex> guard(g_answersMutex);
    return g_inputsAnswer;
}

// The crescent that marks a sleeping device, drawn into the small column where
// a menu puts its tick.
//
// A menu item may be given a bitmap for that column, which is the one way to
// put a mark there without taking over the drawing of the entire menu. The
// glyph is rendered black on white and then turned into a mask: the darker a
// pixel came out, the more opaque it becomes, and every pixel takes the menu's
// own text colour. That keeps the edges smooth, leaves the background clear so
// the highlight shows through when the item is under the pointer, and makes it
// light on dark or dark on light without asking which mode Windows is in.
//
// Kept between menus, and redrawn when the mark size or the text colour has
// changed, which covers a display scale change and a switch to dark mode.
struct MenuMark {
    HBITMAP bitmap = nullptr;
    int size = 0;
    COLORREF colour = 0;
};

HBITMAP DrawMark(MenuMark& kept, const wchar_t* glyph, COLORREF colour) {
    const int width = GetSystemMetrics(SM_CXMENUCHECK);
    const int height = GetSystemMetrics(SM_CYMENUCHECK);
    if (kept.bitmap && kept.size == width && kept.colour == colour) {
        return kept.bitmap;
    }
    if (kept.bitmap) DeleteObject(kept.bitmap);
    kept.bitmap = nullptr;
    kept.size = width;
    kept.colour = colour;
    if (width <= 0 || height <= 0) return nullptr;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   // top down, so rows read naturally
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap =
        CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC canvas = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!bitmap || !canvas || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (canvas) DeleteDC(canvas);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(canvas, bitmap);
    RECT area = {0, 0, width, height};
    FillRect(canvas, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    // The menu's own font, at the height of the mark column, so the crescent
    // matches the tick Windows would have drawn there.
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    HFONT font = nullptr;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0)) {
        metrics.lfMenuFont.lfHeight = -height;
        metrics.lfMenuFont.lfWidth = 0;
        font = CreateFontIndirectW(&metrics.lfMenuFont);
    }
    HGDIOBJ oldFont = font ? SelectObject(canvas, font) : nullptr;
    SetBkMode(canvas, TRANSPARENT);
    SetTextColor(canvas, RGB(0, 0, 0));
    DrawTextW(canvas, glyph, 1, &area,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    if (oldFont) SelectObject(canvas, oldFont);
    if (font) DeleteObject(font);
    SelectObject(canvas, oldBitmap);
    DeleteDC(canvas);

    // White is nothing, black is the glyph. Alpha comes from how dark each
    // pixel ended up, and the colour is premultiplied by it, which is what a
    // menu bitmap with transparency has to be.
    const int red = GetRValue(colour);
    const int green = GetGValue(colour);
    const int blue = GetBValue(colour);
    BYTE* row = static_cast<BYTE*>(pixels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            BYTE* pixel = row + x * 4;
            const int alpha = 255 - pixel[1];   // green channel of a grey
            pixel[0] = static_cast<BYTE>(blue * alpha / 255);
            pixel[1] = static_cast<BYTE>(green * alpha / 255);
            pixel[2] = static_cast<BYTE>(red * alpha / 255);
            pixel[3] = static_cast<BYTE>(alpha);
        }
        row += width * 4;
    }
    return kept.bitmap = bitmap;
}

// The crescent beside a device that has gone to sleep, in the menu's own text
// colour, and the red dot beside a service that is not running, which is the
// same red the tray icon wears at that moment. Both are drawn by Windows once
// they exist, so the menu keeps its own styling - an item this app draws
// itself costs the whole popup its rounded corners, which was tried.
HBITMAP AsleepMark() {
    static MenuMark kept;
    // Not COLOR_MENUTEXT: the system colours stay light-theme black even
    // when Windows is drawing the menu dark, so the crescent would vanish
    // into it.
    return DrawMark(kept, L"\u263E",
                    AppsUseDarkTheme() ? RGB(255, 255, 255)
                                       : GetSysColor(COLOR_MENUTEXT));
}

HBITMAP ServiceDownMark() {
    static MenuMark kept;
    return DrawMark(kept, L"\u25CF", RGB(196, 43, 28));
}

// The note beside a soundbar or receiver, which is listed but is not somewhere
// the picture can come from.
//
// A music note rather than a speaker because the mark is one character drawn in
// the menu's own font, and every speaker in Unicode lives outside the Basic
// Multilingual Plane - so it would need a surrogate pair and would arrive as a
// colour emoji from a fallback font, next to a crescent that is neither.
HBITMAP AudioMark() {
    static MenuMark kept;
    return DrawMark(kept, L"\u266A",
                    AppsUseDarkTheme() ? RGB(255, 255, 255)
                                       : GetSysColor(COLOR_MENUTEXT));
}

std::vector<InputEntry> ParseInputs(const std::string& response) {
    std::vector<InputEntry> found;
    if (response.rfind("OK", 0) != 0) return found;

    // "OK	2.0.0.0|awake|onscreen||Chromecast	3.0.0.0|asleep|||OFFICE"
    size_t at = response.find('\t');
    while (at != std::string::npos && found.size() < 15) {
        const size_t end = response.find('\t', at + 1);
        const std::string entry = response.substr(
            at + 1, end == std::string::npos ? end : end - at - 1);
        at = end;

        // Six fields, and the name last because a name may contain spaces.
        std::string fields[6];
        size_t field = 0;
        for (size_t i = 0; i < entry.size() && field < 6; ++i) {
            if (entry[i] == '|' && field < 5) {
                ++field;
            } else {
                fields[field].push_back(entry[i]);
            }
        }
        if (fields[0].size() != 7) continue;   // not an address like 2.0.0.0

        InputEntry one;
        one.port.assign(fields[0].begin(), fields[0].end());
        one.onScreen = fields[2] == "onscreen";
        one.thisPc = fields[3] == "thispc";
        // Every flag is set here, before the label, because the label is built
        // from some of them. Two of these used to be assigned below it, which
        // meant the row never carried its note: the test was reading a field
        // that had not been filled in yet. Nothing below this point may add a
        // flag the label depends on.
        one.audio = fields[4] == "audio";
        one.cannotSwitch = fields[4] == "ignores";
        one.asleep = fields[1] == "asleep";

        // "HDMI 2 - Chromecast", and for a device behind a receiver or a
        // switch, "HDMI 2 port 1 - Xbox", because that is where it really is.
        std::string label;
        label += "HDMI ";
        label += fields[0][0];
        if (fields[0][2] != '0') {
            label += " port ";
            label += fields[0][2];
        }
        label += " - ";
        label += fields[5].empty() ? "Device" : fields[5];
        if (one.thisPc) label += " (this PC)";
        // On the row, in words, rather than greyed or badged. Three reasons.
        // Greying a menu item also disables it, and there is no grey-but-
        // clickable without drawing the item here, which costs the whole popup
        // its rounded corners - tried, and recorded. Disabling it would also
        // remove the only way to find out the device has started behaving,
        // which matters because this is learnt by trying and can be wrong. And
        // a glyph in the mark column would need a legend, would compete with
        // the dot and the crescent for one slot, and every symbol that reads as
        // "no" is outside the fonts a menu can rely on. Words need none of
        // that, and this row is the one place a person is already looking.
        if (one.cannotSwitch) label += " (use remote)";
        one.label.assign(label.begin(), label.end());

        // asleep is marked with a crescent in the menu's own mark column,
        // drawn below, rather than greyed - greying would read as "not
        // available", the opposite of the truth, since switching to a sleeping
        // device is how it gets woken. All three flags are set above, with the
        // others.
        found.push_back(one);
    }
    return found;
}

std::vector<InputEntry> AskForInputs() {
    return ParseInputs(SendRequest("DEVICES"));
}

// One thread whose only job is to ask the service what to draw.
//
// It exists because the command worker cannot do this job. That thread sends a
// request and waits for the answer, and a switch takes the service up to three
// seconds - it runs on the service's pipe thread, inside SwitchTo, waiting on
// the television. The picture changes about two seconds in, so for the second
// after that the screen is already right, the menu's question is sitting in a
// queue behind a command, and the menu draws the answer from before the switch.
// That was the "open it twice" everybody noticed, and it was never the cache:
// the question was being asked and nobody was free to answer it.
//
// So this thread asks on the read-only pipe, which the service serves from a
// thread of its own that only copies a snapshot. Nothing it does can block on
// CEC, and nothing the command worker does can block it.
//
// It is a thread rather than a call from the UI thread because the read in
// SendRequestTo has no timeout: a service that connected and then wedged would
// hang the menu, and through it the whole tray. Here the worst case is that
// WaitForFreshAnswers gives up after its 250ms and the menu draws what it has,
// which is exactly the old behaviour.
std::mutex g_askMutex;
std::condition_variable g_askReady;
bool g_askWanted = false;
bool g_askStop = false;
std::thread g_askThread;

void RefreshAnswersNow() {
    // Both questions, in order, so status and inputs always describe the same
    // moment. Published together for the same reason.
    const std::string status = SendReadRequest("STATUS");
    if (!status.empty()) {
        TakeStatus(status);
        KeepAnswers(status, ParseInputs(SendReadRequest("DEVICES")));
        return;
    }
    // No read-only pipe. Either the service is not running, or it is an older
    // build that does not offer one, which is a real case for the few seconds
    // of an upgrade. Fall back to the command pipe, which is what this did
    // before the second pipe existed: slower under load, never wrong.
    g_commands.PostFirst("STATUS-REFRESH");
}

void StartAnswerRefresher() {
    g_askThread = std::thread([] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(g_askMutex);
                g_askReady.wait(lock, [] { return g_askStop || g_askWanted; });
                if (g_askStop) return;
                g_askWanted = false;
            }
            RefreshAnswersNow();
        }
    });
}

void StopAnswerRefresher() {
    if (!g_askThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_askMutex);
        g_askStop = true;
    }
    g_askReady.notify_all();
    g_askThread.join();
}

// Asks for a refresh without waiting for it. Collapses: a second request while
// one is already pending does not queue a second round trip, because the answer
// either way is whatever the service holds when the thread gets there.
void WantFreshAnswers() {
    {
        std::lock_guard<std::mutex> lock(g_askMutex);
        g_askWanted = true;
    }
    g_askReady.notify_all();
}

// Flips a service setting, on the command worker rather than here.
//
// It used to ask the service for the current value and then send the new one:
// two blocking round trips on the UI thread. The command pipe serves one caller
// at a time and an input switch holds it for up to thirteen seconds, so a click
// on a setting during one froze the tray for up to ten - and a question that
// timed out read as "off", so the click turned the setting on whatever it had
// been. The value the menu showed is the one the person meant to change, so
// that is what is flipped. flag is the setting's name in the STATUS line.
// Returns the value asked for.
bool ToggleSetting(const char* name, const char* flag) {
    const bool on = !StatusFlag(LastStatus(), flag);
    g_commands.Post(std::string("SET ") + name + (on ? " 1" : " 0"));
    // Behind the change in the same queue, so the next menu draws it.
    g_commands.Post("STATUS-REFRESH");
    return on;
}

// Everything the volume panel asks for goes through here, and so does every
// volume key caught from the keyboard, so both are counted, timed, and routed
// the same way. See VolumeFollow and kVolumeConfirmMs.
void SendVolumeCommand(const char* command);

// Defined with the tray's other per-user settings further down.
DWORD ReadAppDword(const wchar_t* name, DWORD fallback);

// ----------------------------------------------------------- start with Windows

bool StartsWithWindows() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegQueryValueExW(key, kRunValue, nullptr, nullptr,
                                         nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void SetStartWithWindows(bool enabled) {
    // HKCU, so no elevation - the tray app is an ordinary user process and
    // nothing about it needs administrator rights.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (!enabled) {
        RegDeleteValueW(key, kRunValue);
    } else {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
            RegSetValueExW(key, kRunValue, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(quoted.c_str()),
                           static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
        }
    }
    RegCloseKey(key);
}

void ToggleStartWithWindows() { SetStartWithWindows(!StartsWithWindows()); }

// Puts the app in the Start menu so that choosing Exit is recoverable -
// without this it lives only in the tray and there is no obvious way back.
void CreateStartMenuShortcut() {
    wchar_t exePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (!length || length >= MAX_PATH) return;

    // The installer puts a machine-wide entry in the All Users Start menu, so
    // adding a per-user one on top would show the app twice. This one exists
    // for the case the installer was never involved - a copy run straight from
    // a build folder - where without it there is no way back after Exit.
    PWSTR shared = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr,
                                       &shared))) {
        const std::wstring existing =
            std::wstring(shared) + L"\\" + kShortcutName;
        CoTaskMemFree(shared);
        if (GetFileAttributesW(existing.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
    }

    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        return;
    }
    std::wstring link = std::wstring(programs) + L"\\" + kShortcutName;
    CoTaskMemFree(programs);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    IShellLinkW* shell = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void**>(&shell)))) {
        shell->SetPath(exePath);
        shell->SetDescription(L"HDMI-CEC display power control");
        // Named explicitly rather than left to resolve from the target. A
        // shortcut with no icon of its own does inherit the target's, but the
        // shell caches what it resolved, and the first shortcut this app wrote
        // was resolved back when the exe had no icon resource at all.
        shell->SetIconLocation(exePath, 0);
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile,
                                            reinterpret_cast<void**>(&file)))) {
            if (SUCCEEDED(file->Save(link.c_str(), TRUE))) {
                // Tells Explorer the file changed, so an already-open Start
                // menu picks up the new icon instead of its cached one.
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH | SHCNF_FLUSH,
                               link.c_str(), nullptr);
            }
            file->Release();
        }
        shell->Release();
    }
    CoUninitialize();
}

// Runs once, ever. The point of this app is to be present without being
// asked for, so autostart defaults ON - but only on the very first run, so
// that a user who deliberately turns it off is not overruled next launch.
void FirstRunSetup() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0,
                        KEY_READ | KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool alreadyRun =
        RegQueryValueExW(key, L"Configured", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS;
    if (!alreadyRun) {
        SetStartWithWindows(true);
        const DWORD one = 1;
        RegSetValueExW(key, L"Configured", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&one), sizeof(one));
    }

    // The shortcut is versioned separately from first-run so it can be
    // corrected in place. Anyone who installed before the app had an icon has
    // a .lnk pointing at an exe that had none, and nothing would ever rewrite
    // it - so this rewrites it once, and then never again.
    DWORD written = 0;
    size = sizeof(written);
    RegQueryValueExW(key, L"ShortcutVersion", nullptr, nullptr,
                     reinterpret_cast<LPBYTE>(&written), &size);
    if (written < kShortcutVersion) {
        CreateStartMenuShortcut();
        RegSetValueExW(key, L"ShortcutVersion", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&kShortcutVersion),
                       sizeof(kShortcutVersion));
    }
    RegCloseKey(key);
}

// ------------------------------------------------------- sound output
//
// Whether Windows is sending sound to the television at all. When it is not -
// headphones on, a USB speaker chosen, a different monitor - the TV's volume is
// the wrong thing to change, so every volume control is handed back to Windows
// until it is the output again. No setting, because nothing is guessed: it
// acts only on evidence that the output is something else.
//
// The evidence is the same EDID the service recognises the TV by. An HDMI or
// DisplayPort sound device carries the manufacturer and product of the screen
// on the end of its cable, read by the graphics driver from that screen's
// EDID, and the service names the TV from the EDID on the CEC port. Same cable,
// same numbers. A soundbar on the TV's ARC or eARC plays whatever the TV sends
// it, so Windows' output is still the TV and volume still goes over CEC; a
// soundbar between the PC and the TV is what both ends read, so they agree.
// Renaming devices in Windows changes nothing here, since names are not used.
//
// Measured on the development PC (NVIDIA, through the DP134): the TV's sound
// device reported manufacturer 0x6C50 and product 0x9653 for "TCL-9653", the
// manufacturer with its bytes swapped against the EDID's order. Either order
// is accepted, so a driver that keeps the EDID's own order matches too.
//
// Where the numbers cannot be had - no TV named yet, or a driver that does not
// report them - the kind of device decides instead: an HDMI or DisplayPort
// output is taken to be the TV, and anything else is not. Headphones, USB, and
// Bluetooth devices are never a screen, so they are always recognised.

// Properties of a sound device, spelled out because the headers only declare
// them. {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}: 0 is the form factor, 8 the
// jack's subtype.
constexpr PROPERTYKEY kFormFactorKey = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
constexpr PROPERTYKEY kJackSubTypeKey = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 8};
constexpr UINT kFormFactorDisplay = 9;   // DigitalAudioDisplayDevice

bool IsScreenOutput(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return false;
    bool screen = false;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(kFormFactorKey, &value)) && value.vt == VT_UI4) {
        screen = value.ulVal == kFormFactorDisplay;
    }
    PropVariantClear(&value);
    if (!screen && SUCCEEDED(store->GetValue(kJackSubTypeKey, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        // KSNODETYPE_HDMI_INTERFACE and KSNODETYPE_DISPLAYPORT_INTERFACE.
        screen = !_wcsicmp(value.pwszVal, L"{D1B9CC2A-F519-417F-91C9-55FA65481001}") ||
                 !_wcsicmp(value.pwszVal, L"{E47E4031-3EA6-418D-8F9B-B73843CCBA97}");
    }
    PropVariantClear(&value);
    store->Release();
    return screen;
}

// The screen on the end of the device's cable, as its driver reports it.
// False when there is none to report: not a screen output, or a driver that
// does not say.
bool ReadScreenIds(IMMDevice* device, WORD* manufacturer, WORD* product) {
    bool ok = false;
    IDeviceTopology* topology = nullptr;
    if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER,
                                nullptr, reinterpret_cast<void**>(&topology)))) {
        return false;
    }
    IConnector* connector = nullptr;
    if (SUCCEEDED(topology->GetConnector(0, &connector))) {
        IConnector* other = nullptr;
        if (SUCCEEDED(connector->GetConnectedTo(&other))) {
            IPart* part = nullptr;
            if (SUCCEEDED(other->QueryInterface(&part))) {
                IKsJackSinkInformation* sink = nullptr;
                if (SUCCEEDED(part->Activate(CLSCTX_INPROC_SERVER,
                                             __uuidof(IKsJackSinkInformation),
                                             reinterpret_cast<void**>(&sink)))) {
                    KSJACK_SINK_INFORMATION info = {};
                    if (SUCCEEDED(sink->GetJackSinkInformation(&info)) &&
                        (info.ManufacturerId || info.ProductId)) {
                        *manufacturer = info.ManufacturerId;
                        *product = info.ProductId;
                        ok = true;
                    }
                    sink->Release();
                }
                part->Release();
            }
            other->Release();
        }
        connector->Release();
    }
    topology->Release();
    return ok;
}

// True only when Windows' default output is known not to be the television.
// Everything uncertain - no output at all, a call that fails - answers false,
// which leaves volume with the TV exactly as it was before this existed.
bool OutputIsElsewhere() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) {
        return false;
    }
    bool elsewhere = false;
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
        const uint32_t tv = g_tvSinkId.load();
        WORD manufacturer = 0;
        WORD product = 0;
        if (tv && ReadScreenIds(device, &manufacturer, &product)) {
            const auto same = [](WORD reported, WORD expected) {
                return reported == expected || reported == _byteswap_ushort(expected);
            };
            elsewhere = !same(manufacturer, static_cast<WORD>(tv >> 16)) ||
                        !same(product, static_cast<WORD>(tv & 0xFFFF));
        } else {
            elsewhere = !IsScreenOutput(device);
        }
        device->Release();
    }
    enumerator->Release();
    return elsewhere;
}

// Windows says when the default output changes, or a device comes or goes, on
// a thread of its own. Nothing may wait on that thread, and calling back into
// the sound API from it can deadlock, so it only asks the UI thread to look.
class OutputWatcher final : public IMMNotificationClient {
public:
    // One static instance for the life of the process, so the count is moot.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        // Either playback role: Windows' own settings move both together, but a
        // switching tool may set only one, and a check that finds nothing
        // changed does nothing. Communications is a separate choice.
        if (flow == eRender && role != eCommunications) RequestOutputCheck();
        return S_OK;
    }
    // A screen swapped on the same output keeps the device and changes what is
    // on the end of it, which shows up only as the device's state changing.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        RequestOutputCheck();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                     const PROPERTYKEY) override {
        return S_OK;
    }
};

OutputWatcher g_outputWatcher;
IMMDeviceEnumerator* g_outputEnumerator = nullptr;

// Registered on the UI thread at startup and kept for the life of the process.
// If it cannot be, the output is still checked at startup and whenever the
// service names a TV, just not the moment it changes.
void StartOutputWatch() {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_outputEnumerator)))) {
        g_outputEnumerator = nullptr;
        return;
    }
    if (FAILED(g_outputEnumerator->RegisterEndpointNotificationCallback(
            &g_outputWatcher))) {
        g_outputEnumerator->Release();
        g_outputEnumerator = nullptr;
    }
}

void StopOutputWatch() {
    if (!g_outputEnumerator) return;
    g_outputEnumerator->UnregisterEndpointNotificationCallback(&g_outputWatcher);
    g_outputEnumerator->Release();
    g_outputEnumerator = nullptr;
}

// ------------------------------------------------------- volume routing
//
// Some televisions acknowledge a CEC volume command and do nothing with it.
// Measured on an LG OLED driving its own speakers: <User Control Pressed> is
// ACKed every time, the volume does not move, and the set will not even answer
// <Give Audio Status> - so there is nothing to ask it more politely, and
// nothing on this side to fix.
//
// The fallback is to turn down the PC instead of the TV. The room gets quieter
// either way; only who is being turned down differs. It is also instant, since
// it never leaves this process.
//
// The flag itself is declared with the other globals above; the queue reads it
// before this point in the file.

// The default output's volume control, or null when there is no output. The
// caller releases it.
//
// Acquired fresh each time rather than held. The default output changes
// whenever headphones are plugged in or another device is chosen, and a held
// interface would then report the volume of something nobody is listening to.
// It costs a fraction of a millisecond, so the cost does not matter and the
// staleness would.
IAudioEndpointVolume* OpenPcVolume() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) {
        return nullptr;
    }
    IAudioEndpointVolume* volume = nullptr;
    IMMDevice* endpoint = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint))) {
        if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume),
                                      CLSCTX_INPROC_SERVER, nullptr,
                                      reinterpret_cast<void**>(&volume)))) {
            volume = nullptr;
        }
        endpoint->Release();
    }
    enumerator->Release();
    return volume;
}

bool ReadPcVolume(int* level, bool* muted) {
    IAudioEndpointVolume* volume = OpenPcVolume();
    if (!volume) return false;
    float scalar = 0.0f;
    BOOL isMuted = FALSE;
    const bool ok = SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar)) &&
                    SUCCEEDED(volume->GetMute(&isMuted));
    if (ok) {
        if (level) *level = static_cast<int>(scalar * 100.0f + 0.5f);
        if (muted) *muted = isMuted != FALSE;
    }
    volume->Release();
    return ok;
}

// Pushes whatever Windows currently reports into the panel. Only meaningful in
// PC-volume mode, where Windows is the thing being changed.
//
// A read fails for a moment while Windows is switching outputs, and the open
// panel reads ten times a second, so it would catch that moment and flash the
// slider away and back. So it is taken away only on a second failure in a row,
// which a missing output gives and a switch does not.
void RefreshPcReadout() {
    static int failures = 0;
    int level = 0;
    bool muted = false;
    if (ReadPcVolume(&level, &muted)) {
        failures = 0;
        g_volumePanel.SetReadout(true, level, muted);
    } else if (++failures >= 2) {
        g_volumePanel.SetReadout(false, 0, false);
    }
}

// Queues a volume question unless one is already outstanding, or a button is
// held and this is not one of the hold's own questions (see g_buttonHeld).
// Neither is lost: it is remembered as dirty, and asked once the answer lands
// or the button is let go.
bool AskForVolume(bool duringHold) {
    if (g_buttonHeld.load() && !duringHold) {
        g_volumeQueryDirty = true;
        return false;
    }
    const DWORD now = GetTickCount();
    if (g_volumeQueryPending.exchange(true) &&
        now - g_volumeAskTick.load() < kVolumeQuestionExpiryMs) {
        g_volumeQueryDirty = true;   // asked again when the current one lands
        return false;
    }
    g_volumeAskTick = now;
    // The question going out now covers everything wanted so far, so nothing
    // is left owing. Without this a release would ask, and then ask again when
    // that very answer landed.
    g_volumeQueryDirty = false;
    if (!g_commands.Post("VOLUME-ASK")) {
        // Dropped by the queue's cap. Nothing will answer it, so the flag must
        // not be left claiming a question is outstanding.
        g_volumeQueryPending = false;
        return false;
    }
    return true;
}

// A question has been answered, or will not be. Whatever was wanted while it
// was out is asked for now.
void VolumeQuestionSettled() {
    g_volumeQueryPending = false;
    if (g_volumeQueryDirty.exchange(false)) AskForVolume();
}

// Turns a VOLUME reply into a readout for the panel. The panel lives on the UI
// thread, so the reading is posted rather than applied.
void PostVolumeReply(const std::string& reply) {
    // Before the level is posted, so the thumb is gone by the time the number
    // it belonged to is drawn. This is the quick carrier: an open panel polls
    // about once a second and a refusal bumps the service's news count, where
    // the status line is only read on the half-minute watch and on hover - which
    // is why the slider stayed draggable for several seconds after the soundbar
    // had already said no.
    NoteLevelsRefused(reply);

    WPARAM level = kVolumeUnknown;
    LPARAM muted = 0;
    const size_t at = reply.find(" vol=");
    if (at != std::string::npos) {
        level = static_cast<WPARAM>(atoi(reply.c_str() + at + 5));
        muted = StatusFlag(reply, "mute") ? 1 : 0;
    } else if (reply == "OK refused") {
        level = kVolumeRefused;
    }
    if (g_window) PostMessageW(g_window, kVolumeReadoutMessage, level, muted);
}

// Asks whichever source is in charge for the current level.
//
// Called on hover as well as on opening, and that is the reason the panel does
// not visibly resize: the answer has usually arrived by the time anyone
// clicks, so the readout band is already part of the layout rather than
// appearing a moment later and pushing the buttons down.
//
// The last known level is deliberately kept across closes. It is refreshed
// before the panel can be acted on, so the only way to see a stale one is to
// click without ever moving the pointer onto the icon, and it corrects itself
// within a couple of hundred milliseconds even then.
void RefreshReadout() {
    if (VolumeGoesToPc()) {
        RefreshPcReadout();
    } else {
        AskForVolume();
    }
}

// See g_volumeSupport. Hides nothing: the slider stays as it is and the next
// few answers decide.
bool MayAskVolume() {
    return g_volumeSupport != VolumeSupport::kNo || g_tvReportsVolume.load();
}

void ForgetVolumeSupport() {
    g_volumeSupport = VolumeSupport::kUnknown;
    g_volumeMisses = 0;
    // Possibly a different set, which earns a fresh chance at being counted.
    g_follow = VolumeFollow();
}

void GiveUpOnVolume() {
    g_volumeSupport = VolumeSupport::kNo;
    g_volumeMisses = 0;
    g_follow.baseKnown = false;
    g_volumePanel.SetReadout(false, 0, false);
}

long ClampLevel(long level) { return level < 0 ? 0 : level > 100 ? 100 : level; }

// The level the count puts the TV at now.
long FollowedLevel() {
    return ClampLevel(g_follow.base + (g_follow.steps - g_follow.stepsAtBase));
}

// A real answer. Returns the level to show, and starts counting again from it.
int FollowReport(int level, bool muted) {
    // If the TV had heard the first k steps after the question when it
    // answered, it is now at the answer plus the rest: the total since the
    // question, less the running sum at k. Over every possible k, that runs
    // from the total less the highest running sum to the total less the lowest.
    const long low =
        ClampLevel(level + g_follow.sinceQuestion - g_follow.highestSince);
    const long high =
        ClampLevel(level + g_follow.sinceQuestion - g_follow.lowestSince);

    // With nothing counted yet, or counting switched off, the answer itself is
    // shown: of every reading, it is the one that can never run ahead of the TV.
    long shown = level;
    if (g_follow.trusted && g_follow.baseKnown) {
        const long counted = FollowedLevel();
        shown = counted < low ? low : counted > high ? high : counted;
        // Held against the count only when it had steps to get wrong. A move
        // with none is the TV remote, and is simply taken.
        if (g_follow.steps != g_follow.stepsAtBase) {
            if (shown != counted) {
                if (++g_follow.contradictions >= 2) g_follow.trusted = false;
            } else {
                g_follow.contradictions = 0;
            }
        }
    }

    g_follow.baseKnown = true;
    g_follow.base = static_cast<int>(shown);
    g_follow.muted = muted;
    g_follow.stepsAtBase = g_follow.steps;
    return static_cast<int>(shown);
}

// Returns whether the command was handled here, in which case it must not also
// be sent to the TV.
//
// Windows' volume is set directly, not by sending it the volume keys, which is
// what this once did. A key is handled whenever the shell gets round to it, so
// the level read back straight after was usually still the old one and the
// panel caught up only at its next poll, up to a second later - most visibly
// mute, which went red at once and took that long to turn back. The keys also
// put Windows' own volume popup over the panel.
bool HandlePcVolume(const char* command) {
    if (!VolumeGoesToPc()) return false;

    // The panel re-asserts a held button every 300ms so a TV keeps repeating
    // the key. Here each re-assert is one more step, which is what a held key
    // does too; but mute must not re-toggle three times a second, so a held
    // mute acts once and the rest of the hold is swallowed.
    static bool muteHeld = false;

    // Windows' own volume keys move two points a step.
    constexpr int kStep = 2;
    int step = 0;
    int level = -1;
    bool toggleMute = false;
    if (!strcmp(command, "press-volume-up") || !strcmp(command, "volume-up")) {
        step = kStep;
        muteHeld = false;
    } else if (!strcmp(command, "press-volume-down") ||
               !strcmp(command, "volume-down")) {
        step = -kStep;
        muteHeld = false;
    } else if (!strcmp(command, "press-mute") || !strcmp(command, "mute")) {
        if (muteHeld) return true;
        muteHeld = true;
        toggleMute = true;
    } else if (!strncmp(command, "set-volume ", 11)) {
        // Absolute, and here it is simply true rather than requested: no frame,
        // no read-back, no chance of the set disagreeing.
        level = static_cast<int>(ClampLevel(atoi(command + 11)));
    } else if (!strcmp(command, "volume-settled")) {
        return true;   // nothing to confirm; the value came from Windows
    } else if (!strcmp(command, "volume-poll")) {
        RefreshPcReadout();   // local and free, and catches every other source
        return true;
    } else if (!strcmp(command, "release")) {
        muteHeld = false;
        return true;   // swallowed: there is no key held down to release
    } else {
        return false;  // not a volume command; the TV still handles it
    }

    if (IAudioEndpointVolume* volume = OpenPcVolume()) {
        if (toggleMute) {
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMute(&muted))) {
                volume->SetMute(muted ? FALSE : TRUE, nullptr);
            }
        } else {
            float scalar = 0.0f;
            if (step && SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar))) {
                level = static_cast<int>(
                    ClampLevel(static_cast<long>(scalar * 100.0f + 0.5f) + step));
                // As Windows' volume keys do: a step also unmutes.
                volume->SetMute(FALSE, nullptr);
            }
            if (level >= 0) volume->SetMasterVolumeLevelScalar(level / 100.0f, nullptr);
        }
        volume->Release();
    }

    // Exact now, since the change has already happened.
    RefreshPcReadout();
    return true;
}

// ----------------------------------------------------------- remote keys
//
// The service sees the TV forward its remote's buttons, and cannot act on them:
// it runs in Session 0, which has no desktop, so SendInput there reaches
// nothing. This is the half with a desktop. It waits on an event the service
// signals, collects whatever is queued through the ordinary pipe, and injects.
//
// Waiting rather than polling matters more than it looks. A media key has to
// land within a tenth of a second to feel like a button rather than a request,
// and a poll fast enough for that would be running all day for the few seconds
// a year anyone picks up the remote. Blocked on an event, this thread costs
// nothing at all until there is something to do.

HANDLE g_remoteKeyThread = nullptr;
HANDLE g_remoteKeyStop = nullptr;

// The key each remote button sends in place of its default, indexed by the
// button's CEC UI command, as chosen in the dialog under Settings. Zero keeps
// the default. A key is stored with its modifiers above it: (MOD_ALT,
// MOD_CONTROL, MOD_SHIFT bits << 16) | virtual key. Atomic because the dialog
// writes on the UI thread while the event thread reads.
std::atomic<DWORD> g_remoteBinds[256];

// The rebinding dialog while it is open. A button pressed then is the person
// choosing which button to change, so it goes to the dialog instead of being
// sent as a key.
std::atomic<HWND> g_remoteCaptureWindow{nullptr};

// Posted to that dialog. wParam is the UI command, lParam its default key.
constexpr UINT kRemoteButtonMessage = WM_APP + 20;

// Whether holding a button keeps sending the key. The TV repeats a held button
// on its own, and that is wanted for moving through a list and for nothing
// else: repeated, play/pause toggles forty times and Enter opens forty things.
// Judged by the key sent rather than by the button, so a button given an arrow
// repeats like one.
bool RemoteKeyRepeats(UINT vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_PRIOR: case VK_NEXT: case VK_BACK: case VK_DELETE:
        case VK_VOLUME_UP: case VK_VOLUME_DOWN:
            return true;
        default:
            return false;
    }
}

// Keys that share a virtual key with the number pad, or sit on the right of
// the keyboard, and need the extended flag to be the key they are named for.
// Without it Shift with an arrow can arrive as a number pad key instead.
bool IsExtendedKey(UINT vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

// Down and up together, with any modifiers wrapped around the key. A held
// button is the TV repeating its press, so each repeat is a whole tap too, the
// same as a keyboard's own auto-repeat looks to most programs.
void InjectRemoteKey(UINT mods, UINT vk) {
    static constexpr UINT kModifiers[3][2] = {
        {MOD_CONTROL, VK_CONTROL}, {MOD_ALT, VK_MENU}, {MOD_SHIFT, VK_SHIFT}};
    INPUT input[8] = {};
    UINT count = 0;
    const auto add = [&](UINT key, bool up) {
        INPUT& item = input[count++];
        item.type = INPUT_KEYBOARD;
        item.ki.wVk = static_cast<WORD>(key);
        item.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
                          (IsExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);
    };
    for (const auto& modifier : kModifiers) {
        if (mods & modifier[0]) add(modifier[1], false);
    }
    add(vk, false);
    add(vk, true);
    for (int i = 2; i >= 0; --i) {
        if (mods & kModifiers[i][0]) add(kModifiers[i][1], true);
    }
    SendInput(count, input, sizeof(INPUT));
}

// A remote button can send a mouse button instead of a key.
//
// The mice people use with a television usually have the two thumb buttons that
// go back and forward in a browser or a file window, and nothing on a keyboard
// is quite the same: some applications listen for the button and not for
// VK_BROWSER_BACK. So the binding can be a mouse button, marked by the top bit
// of the stored value, which no key binding ever sets - modifiers live in bits
// 16 to 18 and the key itself in the low word.
constexpr DWORD kRemoteMouseFlag = 0x80000000;
// Numbered in the order they were added, never renumbered: these values are in
// people's registries.
constexpr DWORD kMouseRight = 1;
constexpr DWORD kMouseMiddle = 2;
constexpr DWORD kMouseBack = 3;
constexpr DWORD kMouseForward = 4;
constexpr DWORD kMouseLeft = 5;
constexpr DWORD kWheelUp = 6;
constexpr DWORD kWheelDown = 7;

const wchar_t* DescribeRemoteMouse(DWORD which) {
    switch (which) {
        case kMouseRight:   return L"Right click";
        case kMouseMiddle:  return L"Middle click";
        case kMouseBack:    return L"Mouse Back";
        case kMouseForward: return L"Mouse Forward";
        case kMouseLeft:    return L"Left click";
        case kWheelUp:      return L"Wheel up";
        case kWheelDown:    return L"Wheel down";
        default:            return L"a mouse button";
    }
}

// Down and up together, like the key path above: anything watching for a click
// wants both, and holding a mouse button down from a remote is not something
// any of these buttons should do.
void InjectRemoteMouse(DWORD which) {
    // The wheel is one event rather than a press and a release, so it is sent
    // on its own. A notch is WHEEL_DELTA, the same as one click of a real
    // wheel, and down is that negative - as a DWORD, which is what the field
    // is.
    if (which == kWheelUp || which == kWheelDown) {
        INPUT wheel = {};
        wheel.type = INPUT_MOUSE;
        wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wheel.mi.mouseData =
            static_cast<DWORD>(which == kWheelUp ? WHEEL_DELTA : -WHEEL_DELTA);
        SendInput(1, &wheel, sizeof(INPUT));
        return;
    }

    DWORD down = 0, up = 0, data = 0;
    switch (which) {
        case kMouseLeft:   down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP;   break;
        case kMouseRight:  down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  break;
        case kMouseMiddle: down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; break;
        case kMouseBack:
            down = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; data = XBUTTON1;
            break;
        case kMouseForward:
            down = MOUSEEVENTF_XDOWN; up = MOUSEEVENTF_XUP; data = XBUTTON2;
            break;
        default: return;
    }
    INPUT input[2] = {};
    for (int i = 0; i < 2; ++i) {
        input[i].type = INPUT_MOUSE;
        input[i].mi.dwFlags = i ? up : down;
        input[i].mi.mouseData = data;
    }
    SendInput(2, input, sizeof(INPUT));
}

// Defined with the editor further down, and called from here because the list
// of a remote's buttons is learned from ordinary use rather than from having
// the editor open.
void NoteRemoteButton(UINT ui, UINT defaultVk);

void OnRemoteButton(UINT ui, UINT defaultVk, bool repeat) {
    if (!repeat) NoteRemoteButton(ui, defaultVk);
    if (HWND capture = g_remoteCaptureWindow.load()) {
        if (!repeat) PostMessageW(capture, kRemoteButtonMessage, ui, defaultVk);
        return;
    }
    const DWORD bind = g_remoteBinds[ui & 0xFF].load();
    if (bind & kRemoteMouseFlag) {
        const DWORD which = bind & 0xFF;
        // A held button scrolls, because that is what holding a scroll button
        // is for, but does not machine-gun a click: a remote firing the back
        // button through a file manager is nobody's idea of help.
        const bool scrolls = which == kWheelUp || which == kWheelDown;
        if (repeat && !scrolls) return;
        InjectRemoteMouse(which);
        return;
    }
    const UINT vk = bind ? (bind & 0xFFFF) : defaultVk;
    const UINT mods = bind ? (bind >> 16) : 0;
    if (!vk) return;   // a button with no default that nobody has given a key
    if (repeat && !RemoteKeyRepeats(vk)) return;
    InjectRemoteKey(mods, vk);
}

// The same thread also collects volume answers, which the service signals on an
// event of their own for the same reason: a number that has to appear the
// moment the television answers, without anything sitting on the pipe waiting.
DWORD WINAPI ServiceEventThread(LPVOID) {
    // Opened, never created. The service owns these objects and grants us
    // SYNCHRONIZE only, so if one is missing the service is not running - and
    // retrying is the right answer, because it may be about to start, or may
    // have restarted under us during an upgrade.
    for (;;) {
        //
        // Both or neither. The service creates them one after the other, and a
        // thread that opened the first in the gap went on without the second,
        // so no volume answer was ever collected until the service restarted
        // again. Looked for every second rather than five, because right after
        // a service restart is exactly when someone is trying the volume.
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, cec::kRemoteKeyEventName);
        HANDLE volume =
            event ? OpenEventW(SYNCHRONIZE, FALSE, cec::kVolumeEventName) : nullptr;
        if (!event || !volume) {
            if (event) CloseHandle(event);
            if (WaitForSingleObject(g_remoteKeyStop, 1000) == WAIT_OBJECT_0) {
                return 0;
            }
            continue;
        }

        if (g_window) PostMessageW(g_window, kServiceListeningMessage, 0, 0);
        HANDLE waits[3] = {g_remoteKeyStop, event, volume};
        const DWORD count = 3;
        for (;;) {
            const DWORD which =
                WaitForMultipleObjects(count, waits, FALSE, INFINITE);
            if (which == WAIT_OBJECT_0) {
                CloseHandle(event);
                CloseHandle(volume);
                return 0;
            }

            if (which == WAIT_OBJECT_0 + 2) {
                // An answer to collect, handed to the command worker rather
                // than read from here - see VOLUME-READ. A service on its way
                // out signals this too; that read simply fails, and the remote
                // key event, signalled at the same moment, is what takes this
                // loop back to waiting for the service to return.
                g_commands.PostFirst("VOLUME-READ");
                continue;
            }
            if (which != WAIT_OBJECT_0 + 1) break;   // handle went away

            // The service signals this once on its way out, so an empty answer
            // here is the normal shape of a service stopping, not an error.
            const std::string response = SendRequest("KEYS");
            if (response.rfind("OK", 0) != 0) break;   // service gone; reopen

            // Four hex digits a button, the UI command and then its default
            // key, with an R when it is the TV repeating a held button. See
            // KEYS in PipeProtocol.h.
            size_t at = 2;
            while (at < response.size()) {
                while (at < response.size() && response[at] == ' ') ++at;
                unsigned value = 0;
                int digits = 0;
                while (at < response.size() && digits < 4 &&
                       isxdigit(static_cast<unsigned char>(response[at]))) {
                    const char c = response[at++];
                    value = value * 16 +
                            static_cast<unsigned>(c <= '9' ? c - '0'
                                                           : (c | 0x20) - 'a' + 10);
                    ++digits;
                }
                if (digits < 4) break;
                const bool repeat = at < response.size() && response[at] == 'R';
                if (repeat) ++at;
                OnRemoteButton(value >> 8, value & 0xFF, repeat);
            }
        }
        CloseHandle(event);
        CloseHandle(volume);
        if (WaitForSingleObject(g_remoteKeyStop, 2000) == WAIT_OBJECT_0) return 0;
    }
}

// ------------------------------------------------------------- tray icon

// Whether the service is known to be stopped. UI thread only.
//
// This app is only a front end: with the service gone every control does
// nothing, and the first sign of that used to be a balloon after something had
// already failed. A mark on the icon says so before anything is pressed.
bool g_serviceDown = false;

// Whether "Keep PC and TV awake" is ticked. UI thread only. See SetKeepAwake.
bool g_keepAwake = false;

// Reloaded rather than kept, because the right size depends on the display
// scale and that can change while the app is running.
//
// LoadImageW at the shell's own small-icon metric, not LoadIconW: the latter
// hands back the 32px image and scales it down, which looks soft. SM_CXSMICON
// already accounts for scaling, so this picks the 16, 20, 24 or 32px entry to
// match - which is why the .ico carries all four.
HICON LoadTrayIcon() {
    return static_cast<HICON>(
        LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
}

// A badge's colours, as fractions: the fill, and a rim drawn just inside its
// edge. The red has no rim of its own. The yellow needs one: on the light
// taskbar a plain yellow dot all but disappears against the grey, and the rim
// is what keeps its outline there without changing how it reads on a dark one.
struct BadgeColour {
    double red, green, blue;
    double rimRed, rimGreen, rimBlue;
};
// The service is not running.
constexpr BadgeColour kServiceDownBadge = {0.90, 0.13, 0.13, 0.90, 0.13, 0.13};
// "Keep PC and TV awake" is on, which stops this PC sleeping or its screen
// going off on its own - easy to forget, and a PC left that way overnight is
// one that never slept.
constexpr BadgeColour kKeepAwakeBadge = {0.99, 0.80, 0.10, 0.80, 0.56, 0.02};

// Builds a copy of the tray icon with a badge hanging off its top right
// corner, the way an unread count sits on a phone's app icon.
//
// A badge needs somewhere to hang, and an icon has nothing outside its own
// bitmap, so the artwork is drawn smaller to leave a margin and the badge is
// centred on the artwork's corner. A quarter of the circle then overlaps the
// artwork and the rest sits in the margin. Drawing the badge over a full-size
// icon was the first attempt and it looked like damage rather than a mark.
//
// The smaller artwork is redrawn from a larger image inside the .ico rather
// than by shrinking the one already loaded, because shrinking a 16-pixel
// drawing only smears it. Averaging whole blocks of the larger image keeps the
// edges clean, and colour is weighted by alpha so that transparent edges do not
// drag it towards black.
//
// The pixels are edited directly rather than drawn with GDI, which ignores the
// alpha channel: a circle drawn with Ellipse comes out with square corners
// wherever it meets a transparent pixel. Coverage comes from the distance to
// the centre, which gives a smooth edge, and everything is composited as
// straight alpha because that is how the shell reads an icon's bitmap.
//
// Returns nothing if the icon is not the 32-bit kind or is an unexpected shape,
// and the caller then shows the plain icon: no mark beats no icon.
// colour may be null, which draws the artwork at the badged size with no
// badge: the icon the tray always shows, so a badge coming or going changes
// only the corner. The artwork used to be full size until a badge appeared,
// and then shrank to make room, so the whole icon jumped every time.
HICON WithBadge(HICON base, const BadgeColour* colour) {
    ICONINFO info = {};
    if (!GetIconInfo(base, &info)) return nullptr;

    constexpr int kSource = 64;   // the size the artwork is redrawn from
    HICON marked = nullptr;
    BITMAP shape = {};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(shape), &shape) &&
        shape.bmBitsPixel == 32 && shape.bmWidth > 0 &&
        shape.bmWidth <= kSource && shape.bmHeight == shape.bmWidth) {
        const int side = shape.bmWidth;
        // The artwork at 88%, centred, so it sits where every other tray icon
        // does whether or not there is a badge. The badge is small and sits
        // against the top and right edges, over the artwork's rounded corner,
        // which is mostly empty anyway; the clear ring round it keeps the two
        // apart. Tuned by rendering every tray size on both taskbars.
        const int art = (side * 88) / 100;
        const int offset = (side - art) / 2;
        const double radius = side * 0.13;
        const double centreX = side - radius;
        const double centreY = radius;
        // A ring of clear pixels around the badge, so it and the artwork read
        // as two things rather than one shape.
        const double gap = radius <= 3.0 ? 0.75 : radius / 4.0;
        // The rim, at least most of a pixel at the smallest size so it shows.
        const double rim = radius <= 4.0 ? 0.8 : radius / 5.0;

        BITMAPINFO format = {};
        format.bmiHeader.biSize = sizeof(format.bmiHeader);
        format.bmiHeader.biWidth = side;
        // Negative, so the rows are top down and the arithmetic below reads the
        // way the icon looks.
        format.bmiHeader.biHeight = -side;
        format.bmiHeader.biPlanes = 1;
        format.bmiHeader.biBitCount = 32;
        format.bmiHeader.biCompression = BI_RGB;

        BITMAPINFO sourceFormat = format;
        sourceFormat.bmiHeader.biWidth = kSource;
        sourceFormat.bmiHeader.biHeight = -kSource;

        HICON large = static_cast<HICON>(
            LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                       kSource, kSource, LR_DEFAULTCOLOR));
        ICONINFO largeInfo = {};
        HDC screen = GetDC(nullptr);
        if (large && screen && GetIconInfo(large, &largeInfo) &&
            largeInfo.hbmColor) {
            DWORD source[kSource * kSource] = {};
            DWORD canvas[kSource * kSource] = {};
            if (GetDIBits(screen, largeInfo.hbmColor, 0, kSource, source,
                          &sourceFormat, DIB_RGB_COLORS)) {
                // The artwork, averaged down and centred.
                for (int y = 0; y < art; ++y) {
                    const int firstRow = y * kSource / art;
                    const int lastRow = (y + 1) * kSource / art;
                    for (int x = 0; x < art; ++x) {
                        const int firstColumn = x * kSource / art;
                        const int lastColumn = (x + 1) * kSource / art;
                        double sumAlpha = 0.0, sumRed = 0.0, sumGreen = 0.0,
                               sumBlue = 0.0;
                        int counted = 0;
                        for (int sy = firstRow; sy < lastRow; ++sy) {
                            for (int sx = firstColumn; sx < lastColumn; ++sx) {
                                const DWORD pixel = source[sy * kSource + sx];
                                const double a = ((pixel >> 24) & 0xFF) / 255.0;
                                sumAlpha += a;
                                sumRed += ((pixel >> 16) & 0xFF) / 255.0 * a;
                                sumGreen += ((pixel >> 8) & 0xFF) / 255.0 * a;
                                sumBlue += (pixel & 0xFF) / 255.0 * a;
                                ++counted;
                            }
                        }
                        if (!counted || sumAlpha <= 0.0) continue;
                        auto channel = [&](double sum, int shift) {
                            return static_cast<DWORD>(sum / sumAlpha * 255.0 + 0.5)
                                   << shift;
                        };
                        canvas[(offset + y) * side + offset + x] =
                            (static_cast<DWORD>(sumAlpha / counted * 255.0 + 0.5)
                             << 24) |
                            channel(sumRed, 16) | channel(sumGreen, 8) |
                            channel(sumBlue, 0);
                    }
                }

                for (int y = 0; colour && y < side; ++y) {
                    for (int x = 0; x < side; ++x) {
                        const double dx = x + 0.5 - centreX;
                        const double dy = y + 0.5 - centreY;
                        const double distance = std::sqrt(dx * dx + dy * dy);

                        // The gap first, taken out of whatever is already there.
                        double clear = radius + gap - distance + 0.5;
                        if (clear > 0.0) {
                            if (clear > 1.0) clear = 1.0;
                            const DWORD under = canvas[y * side + x];
                            const double kept =
                                ((under >> 24) & 0xFF) * (1.0 - clear);
                            canvas[y * side + x] =
                                (static_cast<DWORD>(kept + 0.5) << 24) |
                                (under & 0x00FFFFFF);
                        }

                        double coverage = radius - distance + 0.5;
                        if (coverage <= 0.0) continue;
                        if (coverage > 1.0) coverage = 1.0;
                        const DWORD under = canvas[y * side + x];
                        const double underAlpha = ((under >> 24) & 0xFF) / 255.0;
                        const double alpha =
                            coverage + underAlpha * (1.0 - coverage);
                        // How far into the fill this pixel is, as against the
                        // rim, with the same smooth edge as the outline.
                        double inner = radius - rim - distance + 0.5;
                        if (inner < 0.0) inner = 0.0;
                        if (inner > 1.0) inner = 1.0;
                        auto mix = [&](int shift, double fill, double edge) {
                            const double dot = fill * inner + edge * (1.0 - inner);
                            const double below = ((under >> shift) & 0xFF) / 255.0;
                            const double value =
                                (dot * coverage +
                                 below * underAlpha * (1.0 - coverage)) / alpha;
                            return static_cast<DWORD>(value * 255.0 + 0.5) << shift;
                        };
                        canvas[y * side + x] =
                            (static_cast<DWORD>(alpha * 255.0 + 0.5) << 24) |
                            mix(16, colour->red, colour->rimRed) |
                            mix(8, colour->green, colour->rimGreen) |
                            mix(0, colour->blue, colour->rimBlue);
                    }
                }

                void* bits = nullptr;
                HBITMAP badged = CreateDIBSection(screen, &format, DIB_RGB_COLORS,
                                                  &bits, nullptr, 0);
                if (badged && bits) {
                    std::memcpy(bits, canvas,
                                static_cast<size_t>(side) * side * sizeof(DWORD));
                    ICONINFO built = {};
                    built.fIcon = TRUE;
                    // The mask belongs to the icon this was copied from, so it
                    // is already the right size.
                    built.hbmMask = info.hbmMask;
                    built.hbmColor = badged;
                    marked = CreateIconIndirect(&built);
                }
                if (badged) DeleteObject(badged);
            }
        }

        if (largeInfo.hbmColor) DeleteObject(largeInfo.hbmColor);
        if (largeInfo.hbmMask) DeleteObject(largeInfo.hbmMask);
        if (large) DestroyIcon(large);
        if (screen) ReleaseDC(nullptr, screen);
    }

    // GetIconInfo hands back copies, and CreateIconIndirect took copies of its
    // own, so both of these belong to this function.
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return marked;
}

// Sends the icon to the shell again at whatever size is now correct, with a
// mark on it: red if the service is down, yellow while keep-awake is on. One
// corner holds one badge, and the red wins, because a service that is not
// running is the thing to act on; the tooltip names both. NIM_ADD after a
// taskbar restart,
// NIM_MODIFY otherwise - modifying an icon the shell has forgotten fails
// silently and leaves nothing in the tray, so a failed modify falls back to
// adding.
void RefreshTrayIcon(bool readd) {
    HICON fresh = LoadTrayIcon();
    if (!fresh) return;
    // Always through WithBadge, badge or not, so the artwork is the same size
    // either way and only the corner changes. See WithBadge.
    const BadgeColour* badge = g_serviceDown ? &kServiceDownBadge
                               : g_keepAwake  ? &kKeepAwakeBadge
                                              : nullptr;
    if (HICON marked = WithBadge(fresh, badge)) {
        DestroyIcon(fresh);
        fresh = marked;
    }
    // The tooltip says in words what the dot says in colour, since a small
    // coloured mark on its own is a puzzle rather than a message.
    wcscpy_s(g_icon.szTip,
             g_serviceDown && g_keepAwake
                 ? L"HDMI-CEC Display Control: the background service is not "
                   L"running, and the PC is being kept awake"
             : g_serviceDown
                 ? L"HDMI-CEC Display Control: the background service is not "
                   L"running"
             : g_keepAwake
                 ? L"HDMI-CEC Display Control: keeping the PC and TV awake"
                 : kAppName);
    HICON previous = g_icon.hIcon;
    g_icon.hIcon = fresh;
    if (!Shell_NotifyIconW(readd ? NIM_ADD : NIM_MODIFY, &g_icon) && !readd) {
        Shell_NotifyIconW(NIM_ADD, &g_icon);
    }
    if (previous) DestroyIcon(previous);
}

// How often the service is looked in on, and the short pause after asking for
// it to start, which is a service's own time to come up rather than a guess at
// one: the check simply happens again if it is still starting.
constexpr UINT_PTR kServiceWatchTimer = 1;
constexpr UINT_PTR kServiceSoonTimer = 2;

// How long after the last click before the TV is asked to confirm the level.
//
// A question and its answer take about as long on the bus as a click, and
// asking after every click put one between nearly every pair: each made the
// next press wait, which is the hitch, and the TV refused presses that arrived
// while it was answering, which lost the click. The count in VolumeFollow now
// moves the number with each step, so nothing needs asking while clicks are
// still coming. Half a second is longer than the gap in any run of fast clicks,
// and the answer after it confirms where the count ended up.
constexpr UINT_PTR kVolumeConfirmTimer = 3;
constexpr UINT kVolumeConfirmMs = 500;
constexpr UINT kServiceWatchMs = 30000;
constexpr UINT kServiceSoonMs = 2000;

// Asks the Service Control Manager, not the pipe.
//
// The SCM is the authority on whether the service is running, the question
// costs almost nothing, and it needs no privileges: reading a service's state
// is something any user may do. Anything it cannot answer counts as running,
// because an app that cries wolf about a healthy service is worse than one
// that says nothing. A service that is not installed at all does count as
// down, since that is exactly the case the mark is for.
bool ServiceIsRunning() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return true;
    bool running = true;
    if (SC_HANDLE service =
            OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<BYTE*>(&status), sizeof(status),
                                 &needed)) {
            running = status.dwCurrentState == SERVICE_RUNNING ||
                      status.dwCurrentState == SERVICE_START_PENDING;
        }
        CloseServiceHandle(service);
    } else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        running = false;
    }
    CloseServiceHandle(manager);
    return running;
}

// Redraws only on a change, so this can be called as often as is convenient.
void UpdateKeyboardVolume();   // with the keyboard hook, further down

void SetServiceDown(bool down) {
    if (down == g_serviceDown) return;
    g_serviceDown = down;
    RefreshTrayIcon(false);
    // The keyboard's volume keys are taken only while there is a service to
    // send them to. See UpdateKeyboardVolume.
    UpdateKeyboardVolume();
}

// Starts the service, asking for elevation only if it has to.
//
// The direct attempt is the ordinary path: the service grants interactive users
// the right to start it, and only that, so this usually just works with nothing
// to approve. The fallback covers an installation from before that grant
// existed, and there Windows' own sc.exe is run with "runas". It is named by
// its full path rather than left to be found, because that is a command being
// run with administrator rights and nothing earlier on the search path should
// get the chance to answer to the name.
//
// A refused prompt needs no handling. The user has just said no, and the mark
// on the icon still says what the situation is.
void LaunchService() {
    bool started = false;
    if (SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE service =
                OpenServiceW(manager, kServiceName, SERVICE_START)) {
            started = StartServiceW(service, 0, nullptr) != FALSE ||
                      GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
    }

    if (!started) {
        wchar_t command[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(command, MAX_PATH);
        if (!length || length >= MAX_PATH) return;
        wcscat_s(command, L"\\sc.exe");

        SHELLEXECUTEINFOW request = {sizeof(request)};
        request.fMask = SEE_MASK_NOASYNC;
        request.lpVerb = L"runas";
        request.lpFile = command;
        request.lpParameters = L"start CecService";
        request.nShow = SW_HIDE;
        ShellExecuteExW(&request);
    }

    // Either way, look again shortly: a service takes a moment to report that
    // it is running, and the icon should not be left claiming otherwise.
    SetTimer(g_window, kServiceSoonTimer, kServiceSoonMs, nullptr);
}

// ------------------------------------------------------------- keep-awake
//
// "Keep PC and TV awake": this PC does not sleep or turn its screen off on its
// own, and the service sends the TV a keep-alive every few minutes so that the
// TV's own auto power off does not switch it off under a still picture.
//
// The PC half is SetThreadExecutionState, the call PowerToys Awake is built on.
// It belongs to this thread and ends when the process does, so a tray that
// crashes cannot leave the PC held awake. Keeping the screen on is part of it on
// purpose: the service turns the TV off when the screen goes off, which would
// defeat the whole point.
//
// The TV half is a lease the service honours only while this app renews it, so
// the same is true there: an app that has gone stops the keep-alives within
// five minutes, with nothing having to notice. The toggle is not remembered
// after the app restarts, so it can never be left on by accident for days.
//
// Windows does not let an app hold off a screen saver, so one can still start.

constexpr UINT_PTR kKeepAwakeTimer = 4;
constexpr UINT kKeepAwakeRenewMs = 2 * 60 * 1000;

void SetKeepAwake(bool on) {
    g_keepAwake = on;
    SetThreadExecutionState(
        on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
           : ES_CONTINUOUS);
    if (on) {
        g_commands.Post("CMD keep-awake");
        SetTimer(g_window, kKeepAwakeTimer, kKeepAwakeRenewMs, nullptr);
    } else {
        KillTimer(g_window, kKeepAwakeTimer);
        g_commands.Post("CMD keep-awake-off");
    }
    // The tooltip says so.
    RefreshTrayIcon(false);
}

// ------------------------------------------------------- screen-off warning
//
// Turning the TV off when the screen goes off has to register with the TV
// first, because it ignores commands from a device it has stopped counting as
// present, and that takes about a second - a second of the TV showing its
// no-signal screen. Windows dims the display fifteen seconds before turning it
// off, and the service registers then, but the dim is set by a hidden "Dim
// display after" setting that Settings never changes: shorten the screen
// timeout and the dim time is left behind it, so the dim never comes.
//
// So this works the moment out for itself, from the same two things Windows
// uses: the time since the last keyboard or mouse input, and the power plan's
// timeouts. About ten seconds ahead it tells the service, once per idle spell.
// The sleep timeout counts as well, since a PC that sleeps first turns the TV
// off then instead.
//
// It is a guess, and made so that being wrong costs nothing. A video or a game
// holding the screen on, or a policy overriding the plan, means one announce
// the TV does not need, which changes nothing it shows. Windows restarting its
// count without any input means the service registers at screen-off, as it
// would have done anyway.
//
// One timer, set each time for the next moment worth looking, and never more
// than a minute out so a changed plan is noticed. While someone is using the
// PC that is about one wake a minute.

constexpr UINT_PTR kScreenOffTimer = 10;
constexpr DWORD kScreenOffLeadMs = 10000;
constexpr DWORD kScreenOffRecheckMs = 60000;

// Written out rather than taken from the SDK headers, whose power GUIDs need a
// library or initguid.h to link.
constexpr GUID kVideoSubgroup = {
    0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
constexpr GUID kVideoTimeout = {
    0x3c0bc021, 0xc8a8, 0x4e07, {0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e}};
constexpr GUID kVideoLockedTimeout = {
    0x8ec4b3a5, 0x6868, 0x48c2, {0xbe, 0x75, 0x4f, 0x30, 0x44, 0xbe, 0x88, 0xa7}};
constexpr GUID kSleepSubgroup = {
    0x238c9fa8, 0x0aad, 0x41ed, {0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20}};
constexpr GUID kSleepTimeout = {
    0x29f6c1db, 0x86da, 0x48c5, {0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda}};

bool g_sessionLocked = false;

// The input the last warning was for, so each idle spell gets one.
DWORD g_screenOffWarnedInput = 0;
bool g_screenOffWarned = false;

// Milliseconds of idle after which the screen goes off or the PC sleeps,
// whichever is sooner, or 0 when neither ever happens. Read fresh each time: a
// handful of registry reads, at most once a minute.
DWORD ScreenOffDueMs() {
    SYSTEM_POWER_STATUS power = {};
    const bool onBattery = GetSystemPowerStatus(&power) && power.ACLineStatus == 0;
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS) return 0;

    DWORD due = 0;
    auto consider = [&](const GUID& subgroup, const GUID& setting) {
        DWORD seconds = 0;
        const DWORD result =
            onBattery ? PowerReadDCValueIndex(nullptr, scheme, &subgroup, &setting,
                                              &seconds)
                      : PowerReadACValueIndex(nullptr, scheme, &subgroup, &setting,
                                              &seconds);
        // Zero is "never". The cap only keeps the multiplication in range.
        if (result != ERROR_SUCCESS || seconds == 0 || seconds > 0x100000) return;
        if (!due || seconds * 1000 < due) due = seconds * 1000;
    };
    consider(kVideoSubgroup, kVideoTimeout);
    consider(kSleepSubgroup, kSleepTimeout);
    // The lock screen has its own, shorter, screen timeout.
    if (g_sessionLocked) consider(kVideoSubgroup, kVideoLockedTimeout);
    LocalFree(scheme);
    return due;
}

void CheckScreenOff() {
    DWORD next = kScreenOffRecheckMs;
    LASTINPUTINFO input = {sizeof(input)};
    // Nothing to warn about while keep-awake holds the screen on, or with no
    // service to tell.
    const DWORD due = (g_keepAwake || g_serviceDown) ? 0 : ScreenOffDueMs();
    if (due && GetLastInputInfo(&input)) {
        if (input.dwTime != g_screenOffWarnedInput) {
            g_screenOffWarnedInput = input.dwTime;
            g_screenOffWarned = false;
        }
        const DWORD idle = GetTickCount() - input.dwTime;
        // Very short timeouts are warned halfway, so there is still a lead.
        const DWORD warnAt =
            due > 2 * kScreenOffLeadMs ? due - kScreenOffLeadMs : due / 2;
        if (idle < warnAt) {
            next = warnAt - idle;
        } else if (idle < due && !g_screenOffWarned) {
            g_screenOffWarned = true;
            g_commands.Post("CMD prepare-blank");
        }
    }
    SetTimer(g_window, kScreenOffTimer,
             std::clamp<DWORD>(next, 1000, kScreenOffRecheckMs), nullptr);
}

// ------------------------------------------------- screen woken by a person
//
// A television changing power state drops its HDMI signal and restores it, and
// Windows wakes the screen because a display reappeared. The service cannot
// tell that from someone moving the mouse, because Session 0 has no input to
// watch - so this watches for it here and says so.
//
// Only for a few seconds after the screen comes on, and only until something
// is seen. The rest of the time nothing runs at all.

// Quick to begin with, since someone who woke the screen themselves is already
// at the keyboard, then slow, because from then on this is only waiting for
// somebody to walk up to a PC whose screen a television turned on. It stops at
// the first sign of them, when the screen goes off again, or after half an
// hour, by which time the screen will have blanked on any ordinary setting.
constexpr UINT_PTR kInputWatchTimer = 11;
constexpr UINT kInputWatchFastMs = 250;
constexpr UINT kInputWatchSlowMs = 5000;
constexpr DWORD kInputWatchFastForMs = 15000;
constexpr DWORD kInputWatchForMs = 30 * 60 * 1000;
constexpr DWORD kInputIsRecentMs = 1500;

DWORD g_inputWatchStarted = 0;

// GUID_CONSOLE_DISPLAY_STATE, written out for the same reason as the power
// setting GUIDs above. Data is 0 off, 1 on, 2 dimmed.
constexpr GUID kConsoleDisplayState = {
    0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

void StopWatchingForInput() {
    if (g_inputWatchStarted) {
        g_inputWatchStarted = 0;
        KillTimer(g_window, kInputWatchTimer);
    }
}

void CheckForInput() {
    LASTINPUTINFO input = {sizeof(input)};
    const DWORD watched = GetTickCount() - g_inputWatchStarted;
    if (GetLastInputInfo(&input) &&
        GetTickCount() - input.dwTime < kInputIsRecentMs) {
        g_commands.Post("CMD user-active");
        StopWatchingForInput();
        return;
    }
    if (watched > kInputWatchForMs) {
        StopWatchingForInput();
    } else if (watched > kInputWatchFastForMs) {
        SetTimer(g_window, kInputWatchTimer, kInputWatchSlowMs, nullptr);
    }
}

// The service is told the screen came on, whoever did it, because that is what
// says a tray is here to answer the question at all. Without one - at the login
// screen, or with this app closed - it turns the TV on straight away instead.
void WatchForInput() {
    g_commands.Post("CMD screen-on");
    g_inputWatchStarted = GetTickCount();
    SetTimer(g_window, kInputWatchTimer, kInputWatchFastMs, nullptr);
}

// ---------------------------------------------------- keyboard volume keys
//
// An HTPC remote, an air mouse, or a keyboard on the arm of the sofa sends the
// same volume keys a desk keyboard does, and while the volume goes to the
// television those should change the television.
//
// The hook lives on a thread of its own with nothing else to do. A low-level
// keyboard hook has to answer within a timeout or Windows quietly drops it, and
// the UI thread can spend seconds waiting for a busy service. The hook decides
// whether the key is one of ours, posts it, and returns; everything else
// happens on the UI thread, which is where the panel's own buttons are handled.

std::atomic<bool> g_keyboardVolumeWanted{false};
std::atomic<bool> g_hookActive{false};
HANDLE g_hookThread = nullptr;
HANDLE g_hookReady = nullptr;
DWORD g_hookThreadId = 0;
constexpr UINT kHookApply = WM_APP + 1;   // to the hook thread, not a window

LRESULT CALLBACK VolumeKeyHook(int code, WPARAM wParam, LPARAM lParam) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (code == HC_ACTION && key && g_keyboardVolumeWanted.load()) {
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        const bool ours = key->vkCode == VK_VOLUME_UP ||
                          key->vkCode == VK_VOLUME_DOWN ||
                          key->vkCode == VK_VOLUME_MUTE;
        // Keys other software sent are taken too, and deliberately: Windows'
        // "injected" flag was once used to let them by, and on many keyboards
        // the volume keys and wheel are handled by the maker's software, which
        // sends them exactly that way. They reached Windows, and the TV never
        // heard them. This app sends no volume keys of its own - its PC-volume
        // mode sets Windows' volume directly - so there is no loop to avoid.
        if ((down || up) && ours && g_window) {
            PostMessageW(g_window, kVolumeKeyMessage,
                         static_cast<WPARAM>(key->vkCode), down ? 1 : 0);
            return 1;   // swallowed, so Windows' own volume stays where it is
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI KeyboardHookThread(LPVOID) {
    HHOOK hook = nullptr;
    MSG message = {};
    // Forces the message queue into existence before anyone posts to it.
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // Read from the flag rather than the message, so a post that arrives before
    // this thread was ready costs nothing: the state is picked up here anyway.
    //
    // A renewal replaces a hook that is already there. Windows removes a
    // low-level hook without a word if it is ever slow to answer - under heavy
    // load, say - and the keys then go quietly back to Windows until the setting
    // is turned off and on. Renewing it on the tray's half-minute watch puts
    // that right by itself. The swap is two calls, microseconds apart.
    auto apply = [&hook](bool renew) {
        const bool want = g_keyboardVolumeWanted.load();
        if (want && hook && renew) {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
        }
        if (want && !hook) {
            hook = SetWindowsHookExW(WH_KEYBOARD_LL, VolumeKeyHook,
                                     GetModuleHandleW(nullptr), 0);
        } else if (!want && hook) {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
        }
        g_hookActive = hook != nullptr;
    };

    // Said only once the queue above exists, so that nothing posted to this
    // thread can be thrown away for arriving too early. The first apply comes
    // after it for the same reason: whatever was asked for while this thread
    // was starting is in the flag by now, and is read here.
    if (g_hookReady) SetEvent(g_hookReady);
    apply(false);

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kHookApply) apply(message.wParam != 0);
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

// Asks for the level before any key is pressed, so the first press already
// has a number to show and a knob's first turn has a level to start from.
// Without it the first answer came only after a few taps, since the TV is
// slow to answer while presses are arriving.
//
// Once when the tray starts listening to the service, and once when the keys
// start going to the TV - a couple of seconds later in both cases, because
// those are the moments a service has just started and is busy announcing
// itself, and a question then goes unanswered. Never on a repeating timer:
// every question is a frame on the bus.
//
// A service that has only just started can leave the first question without
// an answer, because the TV is still busy taking in this PC's announcement.
// So silence is asked about again, a few times and a moment apart, and does
// not count against the set: the slider still appears only on a real answer.
constexpr UINT_PTR kWarmReadoutTimer = 8;
constexpr UINT kWarmReadoutMs = 2000;
constexpr UINT kWarmRetryMs = 1500;
constexpr int kWarmTries = 4;
int g_warmTriesLeft = 0;   // UI thread only

// The hook is wanted only while the setting is on and the volume is going to
// the television. In the fallback mode Windows handles these keys properly by
// itself, so there is nothing to take over.
//
// Nor while the service is not running. The hook swallows the keys, so with
// nothing to send them to they went nowhere at all: a stopped service, or the
// seconds of an upgrade, left the keyboard with no volume control of any kind,
// and no balloon, since volume is the one command that never raises one. Given
// back, they change Windows' volume, which is what they would do without this
// app.
void UpdateKeyboardVolume() {
    const bool want = ReadAppDword(L"KeyboardVolumeToTv", 0) != 0 &&
                      !VolumeGoesToPc() && !g_serviceDown;
    if (g_keyboardVolumeWanted.exchange(want) == want) return;
    if (want && g_window) {
        g_warmTriesLeft = kWarmTries;
        SetTimer(g_window, kWarmReadoutTimer, kWarmReadoutMs, nullptr);
    }
    // The flag is set before the nudge, and the thread reads the flag rather
    // than the message, so the two cannot cross. The wait is for a thread that
    // has only just been created: posting to one without a message queue fails
    // silently, which is precisely how this went unnoticed the first time.
    if (g_hookReady) WaitForSingleObject(g_hookReady, 2000);
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, kHookApply, 0, 0);
}

// Every volume request, from the panel's buttons and from the keyboard alike,
// so both are counted, timed, and routed by the same rules.
void SendVolumeCommand(const char* command) {
    // Whether the last press went to the service rather than to Windows.
    //
    // A release goes wherever its press went, whatever the routing says by
    // the time it is sent. Routing can change between the two - the service
    // reporting that this television's volume belongs to the PC, say - and the
    // release then went to Windows, which swallows it, while the television
    // went on repeating the key until its own timeout and the service kept a
    // hold open for 45 seconds. A release that reaches the service with
    // nothing held is harmless, which is the direction to be wrong in.
    static bool pressAtService = false;
    const bool release = !strcmp(command, "release");

    // Checked before the queue, not inside it: volume routed to this PC is the
    // one path that never needs the pipe, and going through the worker would
    // put a thread hop in front of a keystroke for no reason.
    if (!(release && pressAtService) && HandlePcVolume(command)) return;

    // Not a service command. The panel says this when a drag ends, so the
    // number lands where the set actually is.
    if (!strcmp(command, "volume-settled")) {
        AskForVolume();
        return;
    }
    if (!strcmp(command, "volume-poll")) {
        // A set judged not to report is left alone, unless the service knows
        // this very TV has reported before. Then the silence that gave up on it
        // was a moment, not a fact - most often the first seconds after the
        // service restarts, when it is busy announcing itself - and the open
        // panel keeps asking until the slider comes back by itself.
        if (MayAskVolume()) {
            AskForVolume();
        }
        return;
    }

    // Set before the press is queued and cleared after the release is, so no
    // ordinary question can land between the two.
    //
    // A press while one is already held is a re-send, which happens only once a
    // button has been down for 300ms: a hold, never a click. A question goes
    // right behind it, so the number follows the TV while the button is held.
    // Not for a set known not to report: there it would be a frame on the bus
    // for an answer that never comes.
    //
    // After the release, when to ask depends on what was let go. A click of
    // volume up or down is left to the count, and the TV is asked only once the
    // clicking pauses - see kVolumeConfirmMs. The end of a hold, and a mute, are
    // asked about at once: the count follows neither, and neither comes in a run
    // of fast clicks.
    static bool countedClick = false;
    if (!strncmp(command, "press-", 6)) {
        // No level to count from, so nothing would be on screen until the
        // clicking paused - which, for someone tapping away, could be many
        // seconds. Asked now, ahead of the press rather than between it and its
        // release, and only until the first answer lands. The one-question
        // rule keeps a run of taps from asking more than once at a time.
        if (!g_buttonHeld.load()) {
            g_lastPressAskedFirst =
                !g_follow.baseKnown && MayAskVolume() && AskForVolume();
        }
        const bool resent = g_buttonHeld.exchange(true);
        // The first press of volume up or down is a step, marked so the worker
        // counts it once it has gone out. A re-send is not a step: during a
        // hold the TV does its own repeating. See VolumeFollow.
        const char* mark = "";
        if (!resent) {
            // Another click, so the pause the confirmation waits for has not
            // come yet.
            KillTimer(g_window, kVolumeConfirmTimer);
            if (!strcmp(command, "press-volume-up")) mark = "+";
            if (!strcmp(command, "press-volume-down")) mark = "-";
            countedClick = *mark != '\0';
        } else {
            countedClick = false;
        }
        g_commands.Post(std::string(mark) + "CMD " + command);
        pressAtService = true;
        if (resent && MayAskVolume()) {
            AskForVolume(true);
        }
        return;
    }
    g_commands.Post(std::string("CMD ") + command);
    if (release) {
        pressAtService = false;
        g_buttonHeld = false;
        if (countedClick) {
            SetTimer(g_window, kVolumeConfirmTimer, kVolumeConfirmMs, nullptr);
        } else {
            AskForVolume();
        }
    }
}

// The volume key currently held down, and the re-send that keeps the TV
// repeating it. The same 300ms the panel uses, and for the same reason: a TV
// stops repeating about half a second after the last press it heard.
//
// A key-up can be missed - the desktop changing under a hook is the usual way -
// and a key believed held for ever would leave the TV repeating with nothing to
// stop it. So a hold also ends if the keyboard's own repeat goes quiet. That is
// judged from when the key last came down, and not by asking Windows whether it
// is down: a key this hook swallows never reaches Windows' record of which keys
// are held, so asking said "up" at the first tick, every hold ended after one
// step, and holding a key crawled. The grace covers the longest delay Windows
// allows before a held key starts repeating, which is a second.
int g_keyVolumeHeld = 0;
DWORD g_keyVolumeLastDown = 0;
constexpr UINT_PTR kKeyVolumeTimer = 5;
constexpr UINT kKeyVolumeRepeatMs = 300;
constexpr DWORD kKeyVolumeGraceMs = 1100;

// Long enough for the hook thread to have acted on a toggle, short enough that
// the answer still belongs to the click that caused it.
constexpr UINT_PTR kHookCheckTimer = 6;
constexpr UINT kHookCheckMs = 400;

const char* PressCommandFor(int vk) {
    switch (vk) {
        case VK_VOLUME_UP:   return "press-volume-up";
        case VK_VOLUME_DOWN: return "press-volume-down";
        case VK_VOLUME_MUTE: return "press-mute";
        default:             return nullptr;
    }
}

void ReleaseVolumeKey() {
    if (!g_keyVolumeHeld) return;
    g_keyVolumeHeld = 0;
    KillTimer(g_window, kKeyVolumeTimer);
    SendVolumeCommand("release");
}

// Volume keys arriving faster than the bus can carry them: a volume knob, or a
// touch strip, which turn one movement into a stream of taps.
//
// Each tap is a press and a release, and the bus manages about five of those a
// second. A knob spun briskly sends twenty or more, so the queue filled, taps
// were dropped, and the TV trailed the knob by seconds. So once taps come closer
// together than any finger manages, they stop being sent as keys at all. The
// app keeps its own target, one step per tap, and sends it as a level - the
// same <Set Audio Volume Level> the slider uses - no more often than the bus
// can take, and always the final one. Ordinary clicks and holds never get
// here: a gap between taps longer than kBurstGapMs, or a key held down, stays
// exactly as it was.
//
// A set that ignores the level command would make a knob do nothing, so the
// first answer after a burst is checked against the target, and if the two
// disagree the rest of the session goes back to sending taps.
constexpr UINT_PTR kBurstTimer = 7;
constexpr DWORD kBurstGapMs = 110;     // taps closer than this are a knob
constexpr DWORD kBurstQuietMs = 300;   // no tap for this long ends the burst
constexpr UINT kBurstSendMs = 150;     // at most one level frame this often
constexpr DWORD kBaseFreshMs = 10000;  // an answer this recent is a start
constexpr DWORD kBaseWaitMs = 2500;    // how long a burst waits for one

struct VolumeBurst {
    bool active = false;
    int target = 0;
    int lastSent = -1;
    DWORD lastTap = 0;
    DWORD lastSend = 0;
    int keyDown = 0;          // the tap's key while it is still down
    // No recent answer to start from yet: taps are added up in delta until one
    // arrives, and nothing is sent. See OnVolumeKey.
    bool awaitingBase = false;
    int delta = 0;
    DWORD start = 0;
    // Set when the burst ends, until its closing question goes out. See the
    // kVolumeAskedMessage handler.
    bool rebasePending = false;
    DWORD endTick = 0;
    int checkTarget = -1;     // the level the next answer should match
};
VolumeBurst g_burst;

// Level frames queued and not yet reported gone by the worker. A question only
// describes the burst's final level if it went out after all of them, and the
// worker reports both in the order they left. Kept outside VolumeBurst because
// a new burst can start while the last one's frames are still leaving.
int g_levelFramesOut = 0;

// Answers after a burst that did not match its target, in a row. Two means
// the set is not acting on the level command, and taps go back to being keys.
int g_levelMismatches = 0;
bool g_levelCommandWorks = true;

// The previous tap, whichever way it was sent: when it came down, which way it
// went, and the step count at the time, which tells whether its own step had
// been counted yet when a burst starts behind it.
DWORD g_lastTapTick = 0;
int g_lastTapStep = 0;
long g_lastTapSteps = 0;

void SendBurstLevel() {
    // Never before there is a level to start from: target is meaningless then,
    // and sending it would set the volume to whatever it happens to hold.
    if (g_burst.awaitingBase || g_burst.target == g_burst.lastSent) return;
    char command[32];
    snprintf(command, sizeof(command), "set-volume %d", g_burst.target);
    if (g_commands.Post(std::string("=CMD ") + command)) ++g_levelFramesOut;
    g_burst.lastSent = g_burst.target;
    g_burst.lastSend = GetTickCount();
}

// Sends whatever the last tap left owing, then asks where the TV ended up.
void EndBurst() {
    if (!g_burst.active) return;
    KillTimer(g_window, kBurstTimer);
    if (g_burst.awaitingBase) {
        // Nothing was sent, so there is nothing to confirm. The taps are
        // dropped rather than guessed at.
        g_burst = VolumeBurst();
        return;
    }
    SendBurstLevel();
    g_burst.active = false;
    g_burst.keyDown = 0;
    g_burst.rebasePending = true;
    g_burst.endTick = GetTickCount();
    g_burst.checkTarget = g_burst.target;
    AskForVolume();
}

// True while an answer may predate the level the burst left: during it, and
// until its closing question is on its way. Bounded, because that question can
// be lost, and a readout that ignored every answer after that would be stuck.
bool BurstOwnsReadout() {
    return g_burst.active ||
           (g_burst.rebasePending && GetTickCount() - g_burst.endTick < 2000);
}

// Where the volume panel shows itself for a key: just above the tray icon, or
// the corner of the screen where the icon usually is if the shell will not say.
POINT PanelAnchor() {
    NOTIFYICONIDENTIFIER id = {sizeof(id)};
    id.hWnd = g_window;
    id.uID = kTrayId;
    RECT icon = {};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon))) {
        return {(icon.left + icon.right) / 2, icon.top};
    }
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return {work.right - 1, work.bottom - 1};
}

void ShowVolumeForKey() {
    // Already up: only its countdown restarts. Checked first because a touch
    // strip sends a hundred taps a second, and both checks below are calls
    // into the shell that there is no reason to make a hundred times.
    if (g_volumePanel.visible()) {
        g_volumePanel.ShowPassive({});
        return;
    }
    // A set judged silent still gets the panel if it has reported before: that
    // judgement is most often the seconds after a service restart, and the
    // question each key now asks brings the number back.
    if (!MayAskVolume()) return;
    // Not over a game running exclusive full screen, where a window appearing
    // on top can knock it out of full screen. Borderless games are unaffected.
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        state == QUNS_RUNNING_D3D_FULL_SCREEN) {
        return;
    }
    g_volumePanel.ShowPassive(PanelAnchor());
}

void OnVolumeKey(int vk, bool down) {
    if (!down) {
        if (vk == g_burst.keyDown) g_burst.keyDown = 0;
        if (vk == g_keyVolumeHeld) ReleaseVolumeKey();
        return;
    }
    // On every key, repeats included, so a long hold keeps the panel up.
    ShowVolumeForKey();
    const DWORD now = GetTickCount();
    if (vk == g_keyVolumeHeld) {
        // The keyboard repeating itself: the hold goes on, and the TV is
        // already repeating, so nothing more is sent.
        g_keyVolumeLastDown = now;
        return;
    }
    const int step = vk == VK_VOLUME_UP ? 1 : vk == VK_VOLUME_DOWN ? -1 : 0;

    if (g_burst.active) {
        if (step && vk != g_burst.keyDown) {
            g_burst.keyDown = vk;
            g_burst.lastTap = now;
            if (g_burst.awaitingBase) {
                g_burst.delta += step;
                return;
            }
            g_burst.target = static_cast<int>(ClampLevel(g_burst.target + step));
            g_volumePanel.SetReadout(true, g_burst.target, g_follow.muted);
            if (now - g_burst.lastSend >= kBurstSendMs) SendBurstLevel();
            return;
        }
        // Mute, or a key now held rather than tapped: the burst is over, and
        // the key is handled the ordinary way.
        EndBurst();
    } else if (step && g_levelCommandWorks && g_lastTapStep &&
               now - g_lastTapTick < kBurstGapMs && !g_keyVolumeHeld &&
               MayAskVolume() &&
               !VolumeGoesToPc() && !g_volumePanel.dragging()) {
        KillTimer(g_window, kVolumeConfirmTimer);
        g_burst = VolumeBurst();
        g_burst.active = true;
        g_burst.keyDown = vk;
        g_burst.lastTap = now;
        g_burst.start = now;
        if (g_follow.baseKnown && now - g_lastAnswerTick < kBaseFreshMs) {
            // The previous tap went out as a click. If its step has not been
            // counted yet it is still on its way, and the target includes it.
            long level = FollowedLevel();
            if (g_follow.steps == g_lastTapSteps) level += g_lastTapStep;
            g_burst.target = static_cast<int>(ClampLevel(level + step));
            g_volumePanel.SetReadout(true, g_burst.target, g_follow.muted);
            SendBurstLevel();
        } else {
            // The TV's own remote may have moved the volume since the last
            // answer, and a level worked out from an old number would jump it.
            // So the taps are added up until the TV says where it is. A
            // question that went out ahead of the previous tap's press does not
            // include that tap, so it is added here; otherwise the question is
            // asked now, behind the tap, and its answer already includes it.
            g_burst.awaitingBase = true;
            g_burst.delta = step;
            if (g_lastPressAskedFirst) {
                g_burst.delta += g_lastTapStep;
            } else {
                AskForVolume();
            }
        }
        SetTimer(g_window, kBurstTimer, kBurstSendMs, nullptr);
        return;
    }

    g_lastTapTick = now;
    g_lastTapStep = step;
    g_lastTapSteps = g_follow.steps;

    ReleaseVolumeKey();                  // a different key: let the old one go
    const char* command = PressCommandFor(vk);
    if (!command) return;
    g_keyVolumeHeld = vk;
    g_keyVolumeLastDown = now;
    SendVolumeCommand(command);
    SetTimer(g_window, kKeyVolumeTimer, kKeyVolumeRepeatMs, nullptr);
}

// ---------------------------------------------------------------- hotkey
//
// One key combination that brings the television back to this PC.
//
// The point of it is that you do not need to see the screen to press it, which
// is the whole situation it exists for: the TV is showing something else, the
// PC is running fine behind it, and the tray icon is therefore unreachable.
//
// It sends tv-on rather than active-source, because the two cases people
// actually hit are "the TV is on another input" and "the TV is off", and tv-on
// covers both - it is <Image View On> followed by <Active Source>. The first is
// idempotent on a set that is already awake.
//
// Kept entirely on this side of the pipe. The binding is a per-user interface
// preference, not privileged state, so it lives in HKCU beside the autostart
// setting rather than being added to the service's protocol.

constexpr int kHotkeyId = 1;

// Ctrl+Alt+Shift+P. Three modifiers rather than two on purpose: this is a key
// that must never collide with something a person presses by accident.
constexpr UINT kDefaultHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr UINT kDefaultHotkeyVk = 'P';

UINT g_hotkeyMods = kDefaultHotkeyMods;
UINT g_hotkeyVk = kDefaultHotkeyVk;
bool g_hotkeyEnabled = true;
bool g_hotkeyRegistered = false;

DWORD ReadAppDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

void WriteAppDword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

void LoadHotkey() {
    g_hotkeyEnabled = ReadAppDword(L"HotkeyEnabled", 1) != 0;
    g_hotkeyMods = ReadAppDword(L"HotkeyModifiers", kDefaultHotkeyMods);
    g_hotkeyVk = ReadAppDword(L"HotkeyVirtualKey", kDefaultHotkeyVk);
    // A binding with no modifier would swallow a plain letter everywhere in
    // Windows. Anything stored without one is treated as corrupt.
    if (!g_hotkeyMods || !g_hotkeyVk) {
        g_hotkeyMods = kDefaultHotkeyMods;
        g_hotkeyVk = kDefaultHotkeyVk;
    }
}

// "Ctrl+Alt+Shift+P". The key name comes from GetKeyNameTextW rather than a
// table, so it is correct on a keyboard layout that disagrees with US English.
// The keys with no scan code of their own - media and browser keys, which a
// remote button is quite likely to send - are named here instead.
std::wstring DescribeHotkey(UINT mods, UINT vk) {
    std::wstring text;
    if (mods & MOD_CONTROL) text += L"Ctrl+";
    if (mods & MOD_ALT) text += L"Alt+";
    if (mods & MOD_SHIFT) text += L"Shift+";
    if (mods & MOD_WIN) text += L"Win+";

    switch (vk) {
        case VK_MEDIA_PLAY_PAUSE:  return text + L"Play/Pause";
        case VK_MEDIA_STOP:        return text + L"Stop";
        case VK_MEDIA_NEXT_TRACK:  return text + L"Next Track";
        case VK_MEDIA_PREV_TRACK:  return text + L"Previous Track";
        case VK_BROWSER_BACK:      return text + L"Browser Back";
        case VK_BROWSER_FORWARD:   return text + L"Browser Forward";
        case VK_BROWSER_HOME:      return text + L"Browser Home";
        case VK_VOLUME_UP:         return text + L"Volume Up";
        case VK_VOLUME_DOWN:       return text + L"Volume Down";
        case VK_VOLUME_MUTE:       return text + L"Mute";
        default:                   break;
    }

    wchar_t name[64] = {};
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    // Bit 25 asks for the key's own name rather than the left/right variant.
    // Bit 24 is the extended flag, without which the arrows are named as the
    // number pad keys that share their scan codes.
    LONG code = static_cast<LONG>(scan << 16) | (1 << 25);
    if (IsExtendedKey(vk)) code |= 1 << 24;
    if (scan && GetKeyNameTextW(code, name, static_cast<int>(std::size(name)))) {
        text += name;
    } else {
        wchar_t fallback[16] = {};
        swprintf_s(fallback, L"0x%02X", vk);
        text += fallback;
    }
    return text;
}

void UnregisterHotkeyIfAny() {
    if (!g_hotkeyRegistered) return;
    UnregisterHotKey(g_window, kHotkeyId);
    g_hotkeyRegistered = false;
}

// Returns whether the combination is now live. Failure is nearly always another
// program already owning the same keys, which is worth saying out loud rather
// than leaving as a shortcut that silently does nothing.
bool ApplyHotkey(bool complainOnFailure) {
    UnregisterHotkeyIfAny();
    if (!g_hotkeyEnabled) return true;

    // MOD_NOREPEAT so that holding the keys down sends one command rather than
    // a stream of them at the keyboard repeat rate.
    if (RegisterHotKey(g_window, kHotkeyId, g_hotkeyMods | MOD_NOREPEAT,
                       g_hotkeyVk)) {
        g_hotkeyRegistered = true;
        return true;
    }

    if (complainOnFailure) {
        const std::wstring message =
            L"Windows would not give this app " +
            DescribeHotkey(g_hotkeyMods, g_hotkeyVk) +
            L".\n\nAnother program is almost certainly using it already. "
            L"Choose a different combination.";
        MessageBoxW(nullptr, message.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
    return false;
}

// ------------------------------------------------------- rebinding dialog

constexpr wchar_t kCaptureClass[] = L"CecTrayHotkeyCapture";
HWND g_captureWindow = nullptr;

bool IsModifierKey(UINT vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:    case VK_RWIN:
            return true;
        default:
            return false;
    }
}

// The whole of a capture window's painting: the theme's background and a
// block of wrapped text. Both capture windows are text and a keyboard, so
// there is nothing else to draw.
void PaintCaptureText(HWND window, const std::wstring& text) {
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(window, &paint);
    RECT client = {};
    GetClientRect(window, &client);

    const bool dark = AppsUseDarkTheme();
    HBRUSH background =
        CreateSolidBrush(dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF3, 0xF3, 0xF3));
    FillRect(dc, &client, background);
    DeleteObject(background);

    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    HFONT font = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, GetDpiForWindow(window))) {
        font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x1A, 0x1A, 0x1A));

    const int padding = MulDiv(16, static_cast<int>(GetDpiForWindow(window)), 96);
    RECT text_area = {client.left + padding, client.top + padding,
                      client.right - padding, client.bottom - padding};
    DrawTextW(dc, text.c_str(), -1, &text_area, DT_WORDBREAK | DT_NOPREFIX);

    if (previous) SelectObject(dc, previous);
    if (font) DeleteObject(font);
    EndPaint(window, &paint);
}

// Registers a capture window's class the first time, and opens it centred on
// the monitor the pointer is on, themed, and in front with the keyboard.
// Returns null if the window could not be made.
// `owner`, when given, makes the new window owned by that one: it stays in
// front of it and is destroyed with it, which is what keeps the action box from
// outliving the list it belongs to.
HWND OpenCaptureWindow(const wchar_t* className, WNDPROC proc,
                       const wchar_t* title, int width96, int height96,
                       HWND owner = nullptr) {
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    if (!GetClassInfoExW(g_instance, className, &windowClass)) {
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = proc;
        windowClass.hInstance = g_instance;
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APPICON));
        windowClass.hIconSm = windowClass.hIcon;
        RegisterClassExW(&windowClass);
    }

    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiX = 96;
    }
    MONITORINFO info = {sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int width = MulDiv(width96, static_cast<int>(dpiX), 96);
    const int height = MulDiv(height96, static_cast<int>(dpiX), 96);
    const int x =
        info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y =
        info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;

    HWND window = CreateWindowExW(WS_EX_TOPMOST, className, title,
                                  WS_CAPTION | WS_SYSMENU, x, y, width, height,
                                  owner, nullptr, g_instance, nullptr);
    if (!window) return nullptr;

    const BOOL useDark = AppsUseDarkTheme() ? TRUE : FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark,
                          sizeof(useDark));
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    return window;
}

LRESULT CALLBACK CaptureProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            PaintCaptureText(
                window,
                L"Press the combination you want.\n\n"
                L"It needs Ctrl, Alt, Shift or Win in it, so it does not "
                L"take over an ordinary key.\n\n"
                L"Currently " + DescribeHotkey(g_hotkeyMods, g_hotkeyVk) +
                L".\nPress Esc to keep it.");
            return 0;

        // Alt combinations arrive as WM_SYSKEYDOWN, so both are handled or half
        // the useful bindings would be uncapturable.
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = static_cast<UINT>(wParam);
            if (vk == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            if (IsModifierKey(vk)) return 0;   // not a binding on its own

            // Windows does not reserve Alt+F4, so RegisterHotKey would happily
            // hand it over and then no window on the desktop could be closed
            // with it again. Alt+Tab is refused by the shell anyway; refusing
            // it here just makes the reason legible.
            if ((vk == VK_F4 || vk == VK_TAB) &&
                (GetKeyState(VK_MENU) & 0x8000)) {
                MessageBoxW(window,
                            L"Windows needs that one.\n\nPick something else.",
                            kAppName, MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) {
                mods |= MOD_WIN;
            }
            if (!mods) {
                MessageBoxW(window,
                            L"That key on its own would be captured from every "
                            L"other program.\n\nHold Ctrl, Alt, Shift or Win as "
                            L"well.",
                            kAppName, MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            const UINT wasMods = g_hotkeyMods;
            const UINT wasVk = g_hotkeyVk;
            g_hotkeyMods = mods;
            g_hotkeyVk = vk;
            g_hotkeyEnabled = true;
            if (!ApplyHotkey(true)) {
                // Put it back and stay open, so a collision costs one more
                // keypress rather than the setting.
                g_hotkeyMods = wasMods;
                g_hotkeyVk = wasVk;
                ApplyHotkey(false);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }

            WriteAppDword(L"HotkeyModifiers", g_hotkeyMods);
            WriteAppDword(L"HotkeyVirtualKey", g_hotkeyVk);
            WriteAppDword(L"HotkeyEnabled", 1);
            DestroyWindow(window);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            g_captureWindow = nullptr;
            // Whatever happened, the previously working binding goes back on.
            ApplyHotkey(false);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowHotkeyCapture() {
    if (g_captureWindow) {
        SetForegroundWindow(g_captureWindow);
        return;
    }

    // Released while capturing, or pressing the current combination would fire
    // the command instead of being read as the key being pressed.
    UnregisterHotkeyIfAny();

    g_captureWindow = OpenCaptureWindow(kCaptureClass, CaptureProc,
                                        L"Shortcut to return to this PC", 420, 230);
    if (!g_captureWindow) ApplyHotkey(false);
}

// ------------------------------------------------- remote button dialog
//
// Which key each button on the TV remote sends. Global rather than per TV: it
// is a preference about this PC's keys, and a remote whose buttons changed
// meaning with the television would be one nobody could learn.
//
// Chosen the way a person thinks about it, by pressing the button and then the
// key, rather than from a list of CEC names, which rarely match what is printed
// on a real remote. Kept in HKCU beside the hotkey, one value for each changed
// button, so a button nobody changed goes on following the service's default.

constexpr wchar_t kRemoteCaptureClass[] = L"CecTrayRemoteCapture";
constexpr wchar_t kRemoteValuePrefix[] = L"RemoteButton";

// The button whose row is armed and waiting for an action, or -1. UI thread
// only. What it sends by default is not kept beside it any more: the list has
// that for every button, not only the chosen one.
int g_captureButton = -1;
// From the last STATUS the menu read. With the setting off the service passes
// no buttons on, and a window waiting for one would look broken.
bool g_remoteKeysOn = true;

std::wstring RemoteValueName(UINT ui) {
    wchar_t name[32] = {};
    swprintf_s(name, L"%s%02X", kRemoteValuePrefix, ui & 0xFF);
    return name;
}

// Walks the values that are there rather than asking for all 256 buttons.
void LoadRemoteBinds() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppKey, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    const size_t prefix = wcslen(kRemoteValuePrefix);
    for (DWORD index = 0;; ++index) {
        wchar_t name[64] = {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD value = 0;
        DWORD size = sizeof(value);
        const LONG result =
            RegEnumValueW(key, index, name, &nameLength, nullptr, &type,
                          reinterpret_cast<BYTE*>(&value), &size);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS || type != REG_DWORD) continue;
        if (nameLength != prefix + 2 || wcsncmp(name, kRemoteValuePrefix, prefix)) {
            continue;
        }
        const UINT ui = wcstoul(name + prefix, nullptr, 16) & 0xFF;
        if (value & 0xFFFF) g_remoteBinds[ui] = value;
    }
    RegCloseKey(key);
}

void SaveRemoteBind(UINT ui, DWORD bind) {
    g_remoteBinds[ui & 0xFF] = bind;
    const std::wstring name = RemoteValueName(ui);
    if (bind) {
        WriteAppDword(name.c_str(), bind);
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kAppKey, name.c_str());
    }
}

// CEC 1.4 names, reworded to what a remote usually has printed on it. Buttons
// not here are named by their code, which is also what the service log shows.
std::wstring DescribeRemoteButton(UINT ui) {
    if (ui >= 0x20 && ui <= 0x29) return std::to_wstring(ui - 0x20);
    const wchar_t* name = nullptr;
    switch (ui) {
        case 0x00: name = L"OK"; break;
        case 0x01: name = L"Up"; break;
        case 0x02: name = L"Down"; break;
        case 0x03: name = L"Left"; break;
        case 0x04: name = L"Right"; break;
        case 0x09: name = L"Home"; break;
        case 0x0A: name = L"Settings"; break;
        case 0x0B: name = L"Contents"; break;
        case 0x0C: name = L"Favourites"; break;
        case 0x0D: name = L"Back"; break;
        case 0x10: name = L"Top Menu"; break;
        case 0x11: name = L"Options"; break;
        case 0x2A: name = L"Dot"; break;
        case 0x2B: name = L"Enter"; break;
        case 0x2C: name = L"Clear"; break;
        case 0x30: name = L"Channel Up"; break;
        case 0x31: name = L"Channel Down"; break;
        case 0x32: name = L"Previous Channel"; break;
        case 0x35: name = L"Info"; break;
        case 0x36: name = L"Help"; break;
        case 0x37: name = L"Page Up"; break;
        case 0x38: name = L"Page Down"; break;
        case 0x44: case 0x60: name = L"Play"; break;
        case 0x45: case 0x64: name = L"Stop"; break;
        case 0x46: case 0x61: name = L"Pause"; break;
        case 0x47: case 0x62: name = L"Record"; break;
        case 0x48: name = L"Rewind"; break;
        case 0x49: name = L"Fast Forward"; break;
        case 0x4A: name = L"Eject"; break;
        case 0x4B: name = L"Skip Forward"; break;
        case 0x4C: name = L"Skip Back"; break;
        case 0x53: name = L"Guide"; break;
        case 0x71: name = L"Blue"; break;
        case 0x72: name = L"Red"; break;
        case 0x73: name = L"Green"; break;
        case 0x74: name = L"Yellow"; break;
        case 0x76: name = L"Data"; break;
        default: break;
    }
    if (name) return name;
    wchar_t text[32] = {};
    swprintf_s(text, L"Button %02X", ui & 0xFF);
    return text;
}

// -------------------------------------------------- the remote button editor
//
// Two columns: the television's buttons on the left, what each one does to this
// PC on the right. Pressing a button on the remote lights its row, so the page
// says which button is under your thumb without anything being labelled.
//
// The list is what this remote actually has, not every button CEC defines.
// A television only sends the ones it owns, so the rows are learned from use
// and remembered, and the forty-odd codes nobody's remote sends never appear.
//
// Clicking a row arms it, and so does pressing the button itself. The next key
// or mouse button becomes its action. Left click is deliberately not bindable:
// it is how a row is chosen, and a remote button that clicks wherever the
// pointer happens to be - which this app may be hiding - is a poor trade for
// the ambiguity it would cost.

constexpr int kRowHeight96 = 24;
constexpr DWORD kRemoteFlashMs = 450;
constexpr UINT_PTR kRemoteFlashTimer = 1;

// What the television has been seen to send, and the key each one sends by
// default. One byte per button: zero is never seen, anything else is the
// default virtual key plus one, so a button with no default at all is still
// remembered as having been pressed. Atomic because the receiving thread writes
// it and the window reads it.
std::atomic<uint8_t> g_remoteSeen[256];

// The row lit now, and when it was pressed. UI thread only.
int g_remoteFlash = -1;
DWORD g_remoteFlashTick = 0;
int g_remoteScroll = 0;

void LoadRemoteSeen() {
    uint8_t seen[256] = {};
    DWORD size = sizeof(seen);
    const bool ok =
        RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"RemoteSeen",
                     RRF_RT_REG_BINARY, nullptr, seen, &size) == ERROR_SUCCESS &&
        size == sizeof(seen);
    for (int i = 0; i < 256; ++i) g_remoteSeen[i] = ok ? seen[i] : 0;
}

void SaveRemoteSeen() {
    uint8_t seen[256] = {};
    for (int i = 0; i < 256; ++i) seen[i] = g_remoteSeen[i].load();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, L"RemoteSeen", 0, REG_BINARY, seen, sizeof(seen));
    RegCloseKey(key);
}

// Called from the receiving thread for every button that arrives. Writes only
// when something is new, which after the first few presses is never.
void NoteRemoteButton(UINT ui, UINT defaultVk) {
    const uint8_t value =
        static_cast<uint8_t>(defaultVk < 255 ? defaultVk + 1 : 255);
    if (g_remoteSeen[ui & 0xFF].exchange(value) != value) SaveRemoteSeen();
}

// Every button worth a row: the ones this television has sent, and any that
// somebody has given an action to even if it has not been pressed since.
std::vector<UINT> RemoteRows() {
    std::vector<UINT> rows;
    for (UINT ui = 0; ui < 256; ++ui) {
        if (g_remoteSeen[ui].load() || g_remoteBinds[ui].load()) {
            rows.push_back(ui);
        }
    }
    return rows;
}

// Whether a button does anything at all, which decides whether its action is
// written in ink or in grey. Asked of the values rather than of the words the
// line below produces, so that the two can never drift apart.
bool RemoteDoesSomething(UINT ui) {
    if (g_remoteBinds[ui].load()) return true;
    const uint8_t seen = g_remoteSeen[ui].load();
    return seen > 1;   // the stored value is the default key plus one
}

// What a button does to this PC, in words.
std::wstring RemoteAction(UINT ui) {
    const DWORD bind = g_remoteBinds[ui].load();
    if (bind & kRemoteMouseFlag) return DescribeRemoteMouse(bind & 0xFF);
    if (bind) return DescribeHotkey(bind >> 16, bind & 0xFFFF);
    const uint8_t seen = g_remoteSeen[ui].load();
    const UINT vk = seen ? static_cast<UINT>(seen - 1) : 0;
    return vk ? DescribeHotkey(0, vk) : std::wstring(L"Nothing");
}

std::wstring RemoteHeaderText() {
    if (!g_remoteKeysOn) {
        return L"\"TV remote controls this PC\" is off under Settings, so no "
               L"buttons are reaching this app.";
    }
    return L"Press a button on the TV remote to find it below. Click a row to "
           L"change what it does, or right-click one to put it back.";
}

// ----------------------------------------------------------- the action box
//
// A second window, open for the few seconds it takes to choose one action.
//
// It exists so the list behind it stays an ordinary window. While one window
// did both jobs, every key and click had to be either navigation or an answer,
// which left Esc, left click and the wheel unbindable because the list needed
// them. Here nothing else is competing: the box has one question and the only
// way out of it is the X, which arrives as a non-client click and so cannot be
// mistaken for somebody binding left click.

constexpr wchar_t kRemoteActionClass[] = L"CecRemoteAction";
std::atomic<HWND> g_remoteActionWindow{nullptr};

// Stores the chosen action and closes the box.
void TakeRemoteAction(HWND window, DWORD bind) {
    if (g_captureButton >= 0) SaveRemoteBind(static_cast<UINT>(g_captureButton),
                                             bind);
    DestroyWindow(window);
}

LRESULT CALLBACK RemoteActionProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            std::wstring text =
                g_captureButton >= 0
                    ? DescribeRemoteButton(static_cast<UINT>(g_captureButton))
                    : std::wstring(L"This button");
            text += L"\n\nPress the key, or click the mouse button or wheel, "
                    L"that it should send.\n\nThe mouse has to be over this "
                    L"window for a click to count. Close this window to leave "
                    L"the button as it is.";
            PaintCaptureText(window, text);
            return 0;
        }

        // Every key, including Esc: nothing here needs it for anything else.
        // Alt combinations arrive as WM_SYSKEYDOWN.
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = static_cast<UINT>(wParam);
            if (lParam & (1 << 30)) return 0;   // held past the first press
            if (IsModifierKey(vk)) return 0;

            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;

            // Choosing the key a button already sends is the same as changing
            // nothing, and is stored that way, so it keeps following the
            // default if that default ever changes.
            const uint8_t seen =
                g_captureButton >= 0 ? g_remoteSeen[g_captureButton & 0xFF].load()
                                     : 0;
            const UINT fallback = seen ? static_cast<UINT>(seen - 1) : 0;
            TakeRemoteAction(window,
                             (!mods && vk == fallback) ? 0 : ((mods << 16) | vk));
            return 0;
        }

        case WM_LBUTTONDOWN:
            TakeRemoteAction(window, kRemoteMouseFlag | kMouseLeft);
            return 0;
        case WM_RBUTTONDOWN:
            TakeRemoteAction(window, kRemoteMouseFlag | kMouseRight);
            return 0;
        case WM_MBUTTONDOWN:
            TakeRemoteAction(window, kRemoteMouseFlag | kMouseMiddle);
            return 0;
        case WM_XBUTTONDOWN:
            TakeRemoteAction(window,
                             kRemoteMouseFlag |
                                 (GET_XBUTTON_WPARAM(wParam) == XBUTTON2
                                      ? kMouseForward
                                      : kMouseBack));
            return TRUE;
        // Sent to whichever window has the keyboard, so this arrives wherever
        // the pointer happens to be.
        case WM_MOUSEWHEEL:
            TakeRemoteAction(window,
                             kRemoteMouseFlag |
                                 (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? kWheelUp
                                                                     : kWheelDown));
            return 0;

        // Alt or F10 on its own would otherwise open the window menu and take
        // the next key with it.
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY: {
            // The list is re-enabled before anything else, including on the
            // path where the action was not chosen. A disabled owner with no
            // box in front of it is a window nobody can use.
            HWND owner = GetWindow(window, GW_OWNER);
            if (owner) {
                EnableWindow(owner, TRUE);
                SetForegroundWindow(owner);
                InvalidateRect(owner, nullptr, TRUE);
            }
            g_remoteActionWindow = nullptr;
            g_captureButton = -1;
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowRemoteAction(HWND owner, UINT ui) {
    if (g_remoteActionWindow.load()) return;
    g_captureButton = static_cast<int>(ui);
    HWND box = OpenCaptureWindow(kRemoteActionClass, RemoteActionProc,
                                 L"Choose an action", 360, 210, owner);
    if (!box) {
        // Nothing was disabled, so there is nothing to put back.
        g_captureButton = -1;
        return;
    }
    g_remoteActionWindow = box;
    EnableWindow(owner, FALSE);
}

// ----------------------------------------------------------------- the list

void PaintRemoteList(HWND window) {
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(window, &paint);
    RECT client = {};
    GetClientRect(window, &client);

    const UINT dpi = GetDpiForWindow(window);
    const bool dark = AppsUseDarkTheme();
    const COLORREF back = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF3, 0xF3, 0xF3);
    const COLORREF ink = dark ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x1A, 0x1A, 0x1A);
    const COLORREF faint = dark ? RGB(0x9A, 0x9A, 0x9A) : RGB(0x5A, 0x5A, 0x5A);
    const COLORREF rule = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xD0, 0xD0, 0xD0);

    HBRUSH brush = CreateSolidBrush(back);
    FillRect(dc, &client, brush);
    DeleteObject(brush);

    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    HFONT font = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi)) {
        font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    const int pad = MulDiv(14, static_cast<int>(dpi), 96);
    const int row = MulDiv(kRowHeight96, static_cast<int>(dpi), 96);
    const int headingTop = pad + row * 2;
    const int listTop = headingTop + row;
    const int listBottom = client.bottom - pad - row;
    const int split = client.left + (client.right - client.left) * 45 / 100;

    SetTextColor(dc, ink);
    RECT header = {pad, pad, client.right - pad, headingTop};
    const std::wstring headerText = RemoteHeaderText();
    DrawTextW(dc, headerText.c_str(), -1, &header, DT_WORDBREAK | DT_NOPREFIX);

    // The two columns, named, with a rule under them and one between them, so
    // that which side is the remote and which is this PC needs no reading.
    SetTextColor(dc, faint);
    RECT leftHead = {pad, headingTop, split, listTop};
    RECT rightHead = {split, headingTop, client.right - pad, listTop};
    DrawTextW(dc, L"TV Remote Buttons", -1, &leftHead,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    DrawTextW(dc, L"PC Actions", -1, &rightHead,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    HBRUSH ruleBrush = CreateSolidBrush(rule);
    RECT under = {pad, listTop - 1, client.right - pad, listTop};
    FillRect(dc, &under, ruleBrush);
    RECT down = {split - MulDiv(8, static_cast<int>(dpi), 96), headingTop,
                 split - MulDiv(8, static_cast<int>(dpi), 96) + 1, listBottom};
    FillRect(dc, &down, ruleBrush);
    DeleteObject(ruleBrush);

    const std::vector<UINT> rows = RemoteRows();
    const int fit = (listBottom - listTop) / (row ? row : 1);
    if (g_remoteScroll > static_cast<int>(rows.size()) - fit) {
        g_remoteScroll = static_cast<int>(rows.size()) - fit;
    }
    if (g_remoteScroll < 0) g_remoteScroll = 0;

    if (rows.empty()) {
        SetTextColor(dc, faint);
        RECT empty = {pad, listTop + pad, client.right - pad, listBottom};
        DrawTextW(dc,
                  L"No buttons yet. Press one on the TV remote and it will "
                  L"appear here.",
                  -1, &empty, DT_WORDBREAK | DT_NOPREFIX);
    }

    const DWORD now = GetTickCount();
    for (int slot = 0; slot < fit; ++slot) {
        const int index = g_remoteScroll + slot;
        if (index >= static_cast<int>(rows.size())) break;
        const UINT ui = rows[index];
        RECT line = {pad, listTop + slot * row, client.right - pad,
                     listTop + (slot + 1) * row};

        if (g_remoteFlash == static_cast<int>(ui) &&
            now - g_remoteFlashTick < kRemoteFlashMs) {
            HBRUSH fill = CreateSolidBrush(dark ? RGB(0x33, 0x44, 0x55)
                                                : RGB(0xD8, 0xE8, 0xF6));
            FillRect(dc, &line, fill);
            DeleteObject(fill);
        }

        SetTextColor(dc, ink);
        RECT left = {line.left + pad / 2, line.top, split, line.bottom};
        const std::wstring name = DescribeRemoteButton(ui);
        DrawTextW(dc, name.c_str(), -1, &left,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        // Faint means it does nothing, which is what grey means everywhere
        // else. It used to mean "this is the default", and read as disabled.
        const std::wstring action = RemoteAction(ui);
        SetTextColor(dc, RemoteDoesSomething(ui) ? ink : faint);
        RECT right = {split, line.top, line.right - pad / 2, line.bottom};
        DrawTextW(dc, action.c_str(), -1, &right,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    SetTextColor(dc, faint);
    RECT footer = {pad, listBottom, client.right - pad, client.bottom - pad / 2};
    DrawTextW(dc,
              L"Delete puts every button back. Esc closes this window.", -1,
              &footer,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (previous) SelectObject(dc, previous);
    if (font) DeleteObject(font);
    EndPaint(window, &paint);
}

// The button whose row is at a point, or -1.
int RemoteRowAt(HWND window, POINT point) {
    RECT client = {};
    GetClientRect(window, &client);
    const UINT dpi = GetDpiForWindow(window);
    const int pad = MulDiv(14, static_cast<int>(dpi), 96);
    const int row = MulDiv(kRowHeight96, static_cast<int>(dpi), 96);
    const int listTop = pad + row * 3;
    const int listBottom = client.bottom - pad - row;
    if (!row || point.y < listTop || point.y >= listBottom) return -1;

    const int index = g_remoteScroll + (point.y - listTop) / row;
    const std::vector<UINT> rows = RemoteRows();
    if (index < 0 || index >= static_cast<int>(rows.size())) return -1;
    return static_cast<int>(rows[index]);
}

LRESULT CALLBACK RemoteCaptureProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            PaintRemoteList(window);
            return 0;

        // A button arrived from the television. It lights its row and adds
        // itself to the list if this is the first time. It deliberately does
        // not open the action box: a window appearing because somebody pressed
        // a button on a remote would be startling, and finding a button is a
        // different intention from changing it.
        case kRemoteButtonMessage:
            g_remoteFlash = static_cast<int>(wParam) & 0xFF;
            g_remoteFlashTick = GetTickCount();
            SetTimer(window, kRemoteFlashTimer, kRemoteFlashMs, nullptr);
            InvalidateRect(window, nullptr, TRUE);
            return 0;

        case WM_TIMER:
            if (wParam == kRemoteFlashTimer) {
                KillTimer(window, kRemoteFlashTimer);
                g_remoteFlash = -1;
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int ui = RemoteRowAt(window, point);
            if (ui >= 0) ShowRemoteAction(window, static_cast<UINT>(ui));
            return 0;
        }

        // Putting one button back, which is the only way to undo a binding on
        // a button that has no usual key to choose instead.
        case WM_RBUTTONDOWN: {
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int ui = RemoteRowAt(window, point);
            if (ui >= 0 && g_remoteBinds[ui].load()) {
                SaveRemoteBind(static_cast<UINT>(ui), 0);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
            g_remoteScroll -= GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            InvalidateRect(window, nullptr, TRUE);
            return 0;

        case WM_KEYDOWN: {
            const UINT vk = static_cast<UINT>(wParam);
            if (lParam & (1 << 30)) return 0;
            if (vk == VK_ESCAPE) {
                DestroyWindow(window);
            } else if (vk == VK_DELETE &&
                       MessageBoxW(window,
                                   L"Reset every remote button to its usual "
                                   L"key?",
                                   kAppName,
                                   MB_YESNO | MB_ICONQUESTION) == IDYES) {
                for (UINT ui = 0; ui < 256; ++ui) {
                    if (g_remoteBinds[ui].load()) SaveRemoteBind(ui, 0);
                }
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            g_remoteCaptureWindow = nullptr;
            g_remoteFlash = -1;
            g_remoteScroll = 0;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowRemoteCapture() {
    if (HWND open = g_remoteCaptureWindow.load()) {
        SetForegroundWindow(open);
        return;
    }
    g_remoteScroll = 0;
    g_remoteCaptureWindow = OpenCaptureWindow(
        kRemoteCaptureClass, RemoteCaptureProc, L"TV remote buttons", 480, 460);
}

// -------------------------------------------------------- idle pointer
//
// "Hide the mouse pointer when idle", and its right-click options. The work is
// in PointerHider; this is the settings, all per user in HKCU, and the wiring.

tray::PointerHider g_pointerHider;
constexpr UINT_PTR kPointerTimer = 9;

// The choices offered, and the values stored. Anything else read back from the
// registry is treated as the default rather than trusted.
constexpr DWORD kPointerSeconds[] = {2, 3, 5, 10, 30};
constexpr DWORD kPointerTolerances[] = {0, 5, 15, 40};
constexpr UINT kPointerSecondsBase = 1000;
constexpr UINT kPointerToleranceBase = 2000;

DWORD ReadChoice(const wchar_t* name, const DWORD* choices, size_t count,
                 DWORD fallback) {
    const DWORD value = ReadAppDword(name, fallback);
    for (size_t i = 0; i < count; ++i) {
        if (choices[i] == value) return value;
    }
    return fallback;
}

tray::PointerHider::Options LoadPointerOptions() {
    tray::PointerHider::Options options;
    options.enabled = ReadAppDword(L"HidePointer", 0) != 0;
    options.idleMs = 1000 * ReadChoice(L"HidePointerSeconds", kPointerSeconds,
                                       std::size(kPointerSeconds), 5);
    options.tolerance = static_cast<int>(ReadChoice(
        L"HidePointerTolerance", kPointerTolerances, std::size(kPointerTolerances), 5));
    return options;
}

void ApplyPointerOptions() {
    g_pointerHider.Configure(g_window, kPointerTimer, LoadPointerOptions());
}

// A crash would otherwise leave the pointer invisible until the next start.
// Nothing else is attempted from here.
LONG WINAPI ShowPointerOnCrash(EXCEPTION_POINTERS*) {
    g_pointerHider.Show();
    return EXCEPTION_CONTINUE_SEARCH;
}

// A small native menu, like the tray's own, opened by right-clicking the item.
void ShowPointerOptions() {
    const tray::PointerHider::Options current = LoadPointerOptions();
    HMENU menu = CreatePopupMenu();
    HMENU after = CreatePopupMenu();
    HMENU movement = CreatePopupMenu();
    if (!menu || !after || !movement) {
        if (menu) DestroyMenu(menu);
        if (after) DestroyMenu(after);
        if (movement) DestroyMenu(movement);
        return;
    }
    for (const DWORD seconds : kPointerSeconds) {
        wchar_t label[32] = {};
        swprintf_s(label, L"%lu seconds", seconds);
        AppendMenuW(after, MF_STRING, kPointerSecondsBase + seconds, label);
    }
    CheckMenuRadioItem(after, kPointerSecondsBase + kPointerSeconds[0],
                       kPointerSecondsBase + kPointerSeconds[std::size(kPointerSeconds) - 1],
                       kPointerSecondsBase + current.idleMs / 1000, MF_BYCOMMAND);
    // Pixels at 100% scaling. PointerHider scales them with the display, so
    // the distance is the same on a TV at 200% as on a laptop at 100%.
    for (const DWORD pixels : kPointerTolerances) {
        wchar_t label[32] = {};
        swprintf_s(label, L"%lu px", pixels);
        AppendMenuW(movement, MF_STRING, kPointerToleranceBase + pixels, label);
    }
    CheckMenuRadioItem(movement, kPointerToleranceBase + kPointerTolerances[0],
                       kPointerToleranceBase +
                           kPointerTolerances[std::size(kPointerTolerances) - 1],
                       kPointerToleranceBase + static_cast<UINT>(current.tolerance),
                       MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(after), L"Hide after");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(movement),
                L"Movement tolerance");

    POINT cursor = {};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_LEFTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0,
        g_window, nullptr));
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);   // and the submenus with it

    // Asked for after the menu has been and gone, so the next one is drawn
    // from something current without anybody having waited for it.
    g_commands.Post("STATUS-REFRESH");

    if (chosen >= kPointerToleranceBase) {
        WriteAppDword(L"HidePointerTolerance", chosen - kPointerToleranceBase);
    } else if (chosen > kPointerSecondsBase && chosen < kPointerToleranceBase) {
        WriteAppDword(L"HidePointerSeconds", chosen - kPointerSecondsBase);
    } else {
        return;   // dismissed
    }
    ApplyPointerOptions();
}

// ------------------------------------------------------------------ menu

void AppendToggle(HMENU menu, UINT id, const wchar_t* text, bool checked) {
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id, text);
}

void ShowMenu() {
    // Drawn from the last answers rather than by asking for new ones. Asking
    // from here meant two pipe round trips before anything appeared, each of
    // which could wait on whatever the CEC link was doing - a second for an
    // announce, more for a standby being verified - and a menu that arrives
    // late arrives wherever the pointer has wandered to.
    //
    // Asked first, because everything below depends on it and because kept
    // answers say nothing about a service that has stopped since. This one
    // asks the Service Control Manager rather than the pipe, which is both
    // authoritative and almost free.
    SetServiceDown(!ServiceIsRunning());

    // Topped up through the worker when what was kept is old enough to be
    // wrong, and never by talking to the pipe from this thread.
    //
    // Three things go stale here and only asking finds them: which input is on
    // screen, which a switch from another machine or the television's own remote
    // changes silently; a service that has only just started and had not found
    // the cable yet, which reads as "no display" and used to sit there until the
    // next half-minute watch; and a setting changed elsewhere. The wait is
    // bounded and usually zero, because the refresh that the pointer's arrival
    // started has normally landed by the time the click does.
    const std::string kept = g_serviceDown ? std::string() : LastStatus();
    const bool keptSaysNoDisplay =
        !kept.empty() && kept.rfind("OK attached=1", 0) != 0;
    if (!g_serviceDown && (kept.empty() || keptSaysNoDisplay ||
                           AnswersAge() > kAnswersFreshMs)) {
        // Through the refresher rather than the command worker, which is the
        // whole point of this change: the worker may be several seconds into a
        // switch, and this question has nothing to do with that one.
        WantFreshAnswers();
        WaitForFreshAnswers();
    }
    const std::string status = g_serviceDown ? std::string() : LastStatus();
    const bool connected = !status.empty();
    const bool attached = status.rfind("OK attached=1", 0) == 0;

    HMENU menu = CreatePopupMenu();
    if (!connected) {
        // One item, at the top, saying what is wrong and fixing it when
        // clicked. It was two - a greyed line stating the problem and a
        // "Launch Service" item further down that was always there - which
        // left the one thing worth doing furthest from the one thing worth
        // reading.
        //
        // The two cases are still told apart, because they call for different
        // things: a stopped service wants starting, while one that is running
        // and did not answer in time is busy and wants a moment. Both are
        // offered the same click, since starting a service that is already
        // running does nothing at all.
        MENUITEMINFOW item = {sizeof(item)};
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
        item.fType = MFT_STRING;
        item.fState = MFS_ENABLED;
        item.wID = kMenuLaunchService;
        // An ordinary item, deliberately.
        //
        // Red text was tried and is not worth what it costs: colouring one
        // item means drawing it in this app, and a menu holding an item drawn
        // by the app loses Windows 11's rounded styling for the whole popup,
        // arrows and all. The red dot on the tray icon carries the alarm; this
        // line only has to say what is wrong and be clickable.
        item.dwTypeData = const_cast<wchar_t*>(
            g_serviceDown ? L"Service is not running" : L"Service is busy");
        // The red dot the tray icon is wearing at that moment, in the column
        // a menu keeps for marks. Windows draws the bitmap, so the popup keeps
        // its own styling, and this is as far as emphasis goes without the app
        // drawing the item itself: the grey of a disabled item reads as
        // strongly but cannot be clicked, and bold, which is how a menu marks
        // its default item, Windows 11 does not draw at all.
        if (HBITMAP mark = ServiceDownMark()) {
            item.fMask |= MIIM_BITMAP;
            item.hbmpItem = mark;
        }
        InsertMenuItemW(menu, 0, TRUE, &item);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (!attached && StatusFlag(status, "looking")) {
        // Still looking, which at a cold boot is the ordinary case and not a
        // fault. The pipe server answers a good second before the graphics
        // driver will say which port can carry CEC, and up to two minutes on a
        // slow one, so the old wording told somebody whose television was about
        // to work perfectly that no television had been found. Greyed and not
        // clickable: there is nothing to read yet and nothing to do but wait.
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuNoDisplayHelp,
                    L"Connecting to TV...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (!attached) {
        // Clickable, and it opens the page at the answer.
        //
        // Greyed was honest about there being nothing to switch to and useless
        // to the person most likely to ever see it: someone who has just
        // installed this with an adapter that cannot carry CEC, to whom the
        // line reads as "this app does not work". Two reasons account for
        // nearly all of these, CEC switched off in the television's own
        // settings, and the wrong adapter, and both are on that page with the
        // adapters that do work named.
        AppendMenuW(menu, MF_STRING, kMenuNoDisplayHelp,
                    L"No CEC TV found, click for help");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    const UINT enabled = attached ? MF_STRING : (MF_STRING | MF_GRAYED);
    // What the television is plugged into, at the top because it is the item
    // most worth reaching. One entry per HDMI port, the live one marked with
    // the menu's own radio dot, which Windows draws and which therefore looks
    // right in every theme, at every scale, and in high contrast.
    g_inputs = connected ? LastInputs() : std::vector<InputEntry>();
    if (!g_inputs.empty()) {
        HMENU inputs = CreatePopupMenu();
        UINT id = kMenuFirstInput;
        for (const InputEntry& one : g_inputs) {
            MENUITEMINFOW item = {sizeof(item)};
            item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
            item.fType = MFT_STRING | MFT_RADIOCHECK;
            item.fState = (one.onScreen ? MFS_CHECKED : MFS_UNCHECKED) |
                          ((attached && !one.audio) ? MFS_ENABLED
                                                    : MFS_DISABLED);
            // A sleeping device cannot be the one on screen, so the crescent
            // and the dot never want the same space. Nor can an audio system,
            // which is why all three share the one column: it is never showing
            // the picture, and a greyed row with a note on it says what it is
            // without a word of explanation.
            if (HBITMAP mark = one.audio ? AudioMark()
                                         : (one.asleep ? AsleepMark() : nullptr)) {
                item.fMask |= MIIM_BITMAP;
                item.hbmpItem = mark;
            }
            item.wID = id++;
            item.dwTypeData = const_cast<wchar_t*>(one.label.c_str());
            InsertMenuItemW(inputs, GetMenuItemCount(inputs), TRUE, &item);
        }
        AppendMenuW(menu, MF_POPUP | (attached ? 0 : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(inputs), L"Switch Input");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    // Everything about things being on or off, together. Each is useful and
    // none is used often: the automatic behaviour covers the ordinary day, and
    // these are for the evening that ends with the PC still working.
    HMENU power = CreatePopupMenu();
    AppendMenuW(power, enabled, kMenuTvOn, L"Turn TV on");
    AppendMenuW(power, enabled, kMenuTvStandby, L"Turn TV off");
    // Never greyed: keeping this PC awake needs no service or TV at all.
    AppendToggle(power, kMenuKeepAwake, L"Keep PC and TV awake", g_keepAwake);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(power), L"Power");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Grouped by what each setting is about, with a divider between groups:
    // when the TV turns off, what controls the PC from the sofa, where volume
    // goes, and the app itself. The service's settings are left out while it
    // cannot be reached, since their ticks would be guesses.
    HMENU settings = CreatePopupMenu();
    if (connected) {
        AppendToggle(settings, kMenuStandbyShutdown, L"Turn TV off on Shutdown",
                     StatusFlag(status, "shutdown"));
        // There is no restart item: a restart always leaves the TV on, since
        // the service turns it back on the moment it starts anyway.
        //
        // Sleep and the screen are two items on a PC with ordinary sleep, and
        // one on a Modern Standby PC, where sleeping is the screen turning off
        // and Windows sends nothing else - so a second item there would be a
        // switch that does nothing. The service makes the same choice in
        // SleepFollowed, so the one item really does cover both.
        if (StatusFlag(status, "s0")) {
            AppendToggle(settings, kMenuStandbyDisplayOff,
                         L"TV follows PC's Screen / Sleep state",
                         StatusFlag(status, "blank"));
        } else {
            AppendToggle(settings, kMenuStandbySleep,
                         L"TV follows PC's Sleep state",
                         StatusFlag(status, "sleep"));
            AppendToggle(settings, kMenuStandbyDisplayOff,
                         L"TV follows PC's Screen state",
                         StatusFlag(status, "blank"));
        }
        AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
        g_remoteKeysOn = StatusFlag(status, "keys");
    }
    // Retried here, because whatever was holding the combination at login may
    // have exited since, and opening this menu is the moment someone is asking
    // about it.
    if (g_hotkeyEnabled && !g_hotkeyRegistered) ApplyHotkey(false);

    // The combination itself is not in the label - it is in the edit dialog,
    // which is one right-click away and shows it in full. A menu item wide
    // enough to hold "Return to this PC with Ctrl+Alt+Shift+P" pushed
    // everything else out of shape for information nobody reads twice.
    std::wstring hotkeyLabel = L"Return to PC with Hotkey";
    // A tick beside a shortcut that Windows never granted is a lie, and the
    // failure is otherwise invisible - which is exactly how a dead hotkey gets
    // mistaken for a TV that will not switch.
    if (g_hotkeyEnabled && !g_hotkeyRegistered) {
        hotkeyLabel += L" (another app has it)";
    }
    // The rebind lives on this item's right-click rather than on a second line
    // of its own - see WM_MENURBUTTONUP below. Advertised in the label, because
    // a right-click that does something is worth nothing if nobody tries it.
    hotkeyLabel += L"   (right-click to edit)";
    AppendToggle(settings, kMenuHotkeyEnabled, hotkeyLabel.c_str(),
                 g_hotkeyEnabled);
    if (connected) {
        // Edited the same way as the hotkey, and labelled the same way.
        AppendToggle(settings, kMenuForwardRemoteKeys,
                     L"TV remote controls this PC   (right-click to edit)",
                     g_remoteKeysOn);
    }
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);

    AppendToggle(settings, kMenuHidePointer,
                 L"Hide the mouse pointer when idle   (right-click to edit)",
                 ReadAppDword(L"HidePointer", 0) != 0);
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);

    if (g_outputElsewhere.load()) {
        // Said here because every item below it is quietly not in force.
        AppendMenuW(settings, MF_STRING | MF_GRAYED, 0,
                    L"Sound is playing on another device, so volume is left to "
                    L"Windows");
    }
    AppendToggle(settings, kMenuKeyboardVolume,
                 L"Keyboard volume keys change the TV",
                 ReadAppDword(L"KeyboardVolumeToTv", 0) != 0);
    if (connected) {
        AppendToggle(settings, kMenuVolumeToPc,
                     L"Volume buttons change this PC's volume, not the TV's",
                     StatusFlag(status, "pcvol"));
    }
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);

    AppendToggle(settings, kMenuStartWithWindows, L"Start with Windows",
                 StartsWithWindows());
    if (connected) {
        // Said on the item, like the three others that hide something behind a
        // right-click: an action nobody knows about is worth nothing.
        AppendToggle(settings, kMenuLogging,
                     L"Keep a log   (right-click to open)",
                     StatusFlag(status, "log"));
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"Settings");

    AppendMenuW(menu, MF_STRING, kMenuGuide, L"Guide...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    // Required so the menu dismisses when the user clicks elsewhere.
    POINT cursor = {};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    // TPM_LEFTBUTTON, deliberately, where this used to pass TPM_RIGHTBUTTON.
    // That flag makes the right button a second way to *select* an item, which
    // is why right-clicking anything here did the same as left-clicking it and
    // left no room for a right-click to mean something else.
    TrackPopupMenu(menu, TPM_LEFTBUTTON, cursor.x, cursor.y, 0, g_window,
                   nullptr);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ------------------------------------------------------------------ guide
//
// A rich edit control filling the whole window.
//
// Two things drove that choice. A plain edit control cannot bold anything, so
// the questions could not be told apart from the answers; and inset inside its
// parent it left a frame of window background around itself, which read as a
// stray border. Filling the client area removes the frame, and the text is
// inset instead by the control's own formatting rectangle, so the scrollbar
// still sits at the edge where it belongs.
//
// Rich edit also brings the scrolling, the mouse wheel, the keyboard and text
// selection, none of which then has to be written or maintained here.

constexpr wchar_t kGuideClass[] = L"CecGuideWindow";

// At 96 DPI; scaled to whichever monitor the window opens on.
constexpr int kGuideBaseWidth = 660;
constexpr int kGuideBaseHeight = 600;
constexpr int kGuideBasePadding = 20;

HWND g_guideWindow = nullptr;
HWND g_guideText = nullptr;
HBRUSH g_guideBrush = nullptr;
bool g_faqDark = false;

// Not GetSysColor: Windows leaves the classic system colours light even when
// apps are in dark mode, so asking it would give white either way.
COLORREF GuideBackColour() {
    return g_faqDark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xFF, 0xFF, 0xFF);
}
COLORREF GuideTextColour() {
    return g_faqDark ? RGB(0xE8, 0xE8, 0xE8) : RGB(0x1A, 0x1A, 0x1A);
}

// The same key the shell reads. Absent on older builds, where light is right.
// Lets Windows draw this app's menus dark when the system is dark.
//
// A plain Win32 menu is drawn by Windows, and Windows draws it light unless
// the process has said it follows the system theme. The call that says so has
// no header and no name in any SDK: it lives in uxtheme by ordinal, which is
// how every app with dark context menus does it, Explorer included. So it is
// asked for by ordinal, from a Windows new enough to have it, and if anything
// is missing the menus stay light and nothing else changes.
//
// The alternative is drawing the menu here, which was tried for one item and
// costs the whole popup its Windows 11 styling. Translucency is not on offer
// at all: the system's own acrylic menus are not the same kind of window.
enum class AppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };

bool AppsUseDarkTheme();

using PlainThemeFn = void(WINAPI*)();
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

SetPreferredAppModeFn g_setAppMode = nullptr;
AllowDarkModeForWindowFn g_allowDarkWindow = nullptr;
PlainThemeFn g_refreshColourPolicy = nullptr;
PlainThemeFn g_flushMenuThemes = nullptr;

void FollowSystemThemeInMenus() {
    static bool looked = false;
    if (!looked) {
        looked = true;
        // 1809 is where these appeared. Older builds keep light menus.
        if (!IsWindows10OrGreater()) return;
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme) return;
        g_setAppMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        g_allowDarkWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        g_refreshColourPolicy = reinterpret_cast<PlainThemeFn>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(104)));
        g_flushMenuThemes = reinterpret_cast<PlainThemeFn>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    }
    if (!g_setAppMode) return;

    // Told outright which way to go, rather than left to follow the system.
    //
    // Asking it to follow did nothing on Windows 11 build 26200, with the
    // window marked as well. This app already reads the setting for its own
    // windows and refreshes when it changes, so it says plainly which theme it
    // wants and the result is the same either way.
    g_setAppMode(AppsUseDarkTheme() ? AppMode::ForceDark : AppMode::ForceLight);
    if (g_refreshColourPolicy) g_refreshColourPolicy();
    if (g_flushMenuThemes) g_flushMenuThemes();
}

// Saying it once for the process is not enough: a menu is drawn dark only when
// the window it belongs to has been marked as well, which is the part the
// first attempt at this missed.
void FollowSystemThemeInWindow(HWND window) {
    FollowSystemThemeInMenus();
    if (g_allowDarkWindow && window) g_allowDarkWindow(window, TRUE);
}

bool AppsUseDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
            &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

// Kept as a table rather than one long string so that rewording an answer is a
// single edit with no formatting to work around. A newline inside an answer
// starts a new paragraph.
// One page, in sections, ordered so that what people arrive for comes before
// what they look up. A section is a heading with nothing under it of its own;
// everything else is a heading and its text, which covers both the plain
// explanations and the questions, since on the page they look the same.
struct GuideEntry {
    const wchar_t* heading;
    const wchar_t* body;   // null for a section heading
};

const GuideEntry kGuide[] = {
    {L"What this app does", nullptr},

    {L"The TV follows the PC.",
     L"It turns off when the PC sleeps, shuts down, or turns its screen off, "
     L"and comes back on when the PC wakes or the screen returns. A restart "
     L"leaves it on, so you can watch the PC start up. A restart Windows "
     L"Update makes by itself, usually at night, does not turn the TV on, and "
     L"it comes on as soon as you use the PC, even at the sign-in screen. A TV "
     L"you switched off yourself is left alone."},

    {L"Nothing leaves this PC.",
     L"There is no network code in the app at all. It does not check for "
     L"updates, report anything, or send any information about you, your PC, or "
     L"your TV anywhere. It runs a background service because turning a TV on "
     L"before anybody has signed in needs one, and for no other reason."},

    {L"Using it", nullptr},

    {L"Left-click the tray icon for volume.",
     L"Mute is a simple button, while Volume Up and Down can be pressed "
     L"repeatedly or held down, like on a remote. The row under the buttons "
     L"shows where sound is playing, and clicking it lists the other outputs "
     L"to switch to."},

    {L"Right-click it for everything else.",
     L"Switch Input, the TV power commands, Settings, and this page. Four of "
     L"the settings have more behind a right-click on the item itself: the "
     L"keyboard shortcut, the TV remote buttons, hiding the mouse pointer, and "
     L"the service log, which opens in Notepad."},

    {L"Choosing when the TV turns off.",
     L"\"Turn TV off on Shutdown\" covers shutting down, \"TV follows PC's "
     L"Sleep state\" covers sleep and waking up, and \"TV follows PC's Screen "
     L"state\" covers the screen turning itself off after a while and coming "
     L"back. Each one can be turned off on its own under Settings. A restart "
     L"always leaves the TV on.\n"
     L"Windows turns the screen off on the way to sleep, so with the screen "
     L"one on, the TV can still turn off when the PC sleeps. On laptops that "
     L"sleep the modern way, sleep and the screen are the same thing, and "
     L"Settings shows a single item for both: \"TV follows PC's Screen / "
     L"Sleep state\"."},

    {L"CTRL+ALT+SHIFT+P brings the TV back to this PC.",
     L"From any input, and even if the TV is off. It works without seeing the "
     L"screen, which is the point of it. If the input the TV is on has nothing "
     L"connected, the TV finishes looking for a signal there first, which can "
     L"take ten seconds or more. To change the combination, right-click that "
     L"item under Settings."},

    {L"The TV remote can drive this PC.",
     L"If your TV forwards its remote to whichever input is showing, its "
     L"arrows, OK, back, and any play, pause, stop or skip buttons reach this "
     L"PC. Which of those a TV sends is up to the TV, and buttons it keeps for "
     L"itself, such as volume and home, never arrive."},

    {L"Choosing what each remote button does.",
     L"Right-click \"TV remote controls this PC\" under Settings. The window "
     L"lists your TV's buttons on the left and what this PC does for each one "
     L"on the right. Press a button on the remote and its line lights up, "
     L"which is also how to find out which buttons your TV sends at all.\n"
     L"Click a line to change it. A small window opens and whatever you do "
     L"next becomes the new action: press a key, click a mouse button, or roll "
     L"the wheel. That means a remote button can left-click, right-click, go "
     L"back or forward, or scroll, as well as send a key such as Escape or "
     L"space. A button set to scroll keeps scrolling while you hold it; the "
     L"others act once per press, so a held button cannot fire a click over "
     L"and over.\n"
     L"A line shown in grey does nothing at the moment, either because your "
     L"TV does not send that button or because nothing is set for it. To undo "
     L"a change, right-click that line and it goes back to its usual key. "
     L"Press Delete to put every button back at once."},

    {L"Switching between devices", nullptr},

    {L"Switch Input lists what is plugged into the TV.",
     L"Each HDMI port appears with the name the device gives. Choosing one "
     L"asks that device to put itself on screen, which is how a TV changes "
     L"input: a TV does not take orders about its own inputs, but a device is "
     L"always allowed to speak for itself.\n"
     L"Four marks can appear on a line. A dot means that input is the one on "
     L"screen now. A crescent means the device has gone to sleep, and choosing "
     L"it usually wakes it. A music note means a soundbar or receiver, which "
     L"is greyed because the picture cannot come from it. And \"(use remote)\" "
     L"means that device will not switch itself over when asked, which is "
     L"explained below."},

    {L"An input says \"use remote\". Which remote?",
     L"Either of two, and both work. Press any button on that device's own "
     L"remote, which is usually the quickest, and the device will take the TV "
     L"by itself. Or press the input or source button on the TV's remote and "
     L"pick that HDMI port by hand. If the device has no remote of its own, "
     L"such as a games console or a stick you drive from a controller, use the "
     L"TV remote.\n"
     L"The reason is the device, not this app and not the cable. A few devices "
     L"join the HDMI cabling and answer questions about themselves, yet ignore "
     L"a request to put themselves on screen. Apple TV is one of them. Nothing "
     L"can be sent that will change that, so the note is there to save you "
     L"trying.\n"
     L"It appears the first time that device is asked and does nothing, and it "
     L"disappears by itself if the device ever does take the screen, so a "
     L"device that was merely busy clears its own note. It is also forgotten "
     L"when the PC restarts, so it may take one try to come back."},

    {L"A soundbar or receiver is listed but cannot be chosen.",
     L"That is deliberate. It occupies an HDMI port, so it belongs in the "
     L"list, but the picture cannot come from it and asking a TV to switch "
     L"there does nothing. It is shown so you can see it was found, and "
     L"because it is where the sound is going: when one is present the volume "
     L"buttons change the soundbar rather than the TV."},

    {L"Only devices with CEC turned on can appear.",
     L"A device that does not speak CEC cannot be named, listed, or switched "
     L"to by anything. Switching to a sleeping device usually wakes it, but a "
     L"sleeping PC is the exception: it has nothing running to answer."},

    {L"Closing a laptop lid counts as the screen going off.",
     L"Laptops that sleep the modern way have no separate sleeping: they turn "
     L"the screen off and drift down on their own. So on those, \"TV follows "
     L"PC's Screen / Sleep state\" under Settings is what makes the TV follow "
     L"a closed lid, and turning it off leaves the TV on when you close it."},

    {L"Two PCs on one TV.",
     L"If both run this app, each one lists the other in Switch Input, and "
     L"choosing it puts that PC on screen. It has to be awake with its screen "
     L"on, because a sleeping PC has nothing running to answer, and a "
     L"crescent shows when it is asleep. On either PC, the shortcut and "
     L"\"Turn TV on\" bring the TV back to that PC."},

    {L"If something does not work", nullptr},

    {L"Give it a few seconds first.",
     L"After the PC starts, and after the app is installed or updated, "
     L"finding the TV takes a moment: usually about five seconds, and up to "
     L"two minutes if the graphics driver is slow to wake. While it is "
     L"looking, the menu says \"Connecting to TV\". Only once it has given up "
     L"does it say no TV was found, and that is the point at which the rest "
     L"of this section applies."},

    {L"I chose an input and nothing happened.",
     L"Open the menu again and read that line. If it now says \"(use remote)\", "
     L"that device will not switch itself over and the entry above says what "
     L"to do instead. If it looks unchanged, nothing on that port answered at "
     L"all, which usually means the port is empty, the device there has CEC "
     L"turned off, or it is a PC that is asleep or has its screen off.\n"
     L"A notification explains both cases as they happen, but Windows hides "
     L"notifications while Do Not Disturb or focus assist is on, and that is "
     L"switched on by default at some times of day. The menu always shows it, "
     L"so the menu is the place to look."},

    {L"The dot says an input is on screen, but the TV shows something else.",
     L"Most TVs never say which input they are showing, so the dot follows "
     L"the last device that put itself on screen. If the TV ignored that, the "
     L"dot stays wrong until something else switches.\n"
     L"A TV that keeps ignoring one device, often after a lot of switching "
     L"back and forth, can usually be cleared by unplugging it at the wall "
     L"for thirty seconds. This has been seen on Amazon Fire TV models."},

    {L"The TV does not react to anything, or the menu says "
     L"\"No CEC TV found\".",
     L"CEC has to be turned on in the TV's own settings, and it is usually off "
     L"on a new TV. Most makers rename it: SIMPLINK on LG, Anynet+ on Samsung, "
     L"BRAVIA Sync on Sony, HDMI-CEC on Fire TV, and just CEC on TCL and Vizio. "
     L"This is the most common reason nothing works, so check it first."},

    {L"The volume bar shows a level but will not drag.",
     L"Some sound systems report their volume and will not be set to a number. "
     L"On those the bar has no handle and is there to be read, not moved, and "
     L"the three buttons do the work. A handle you could drag that sprang back "
     L"every time would be worse than none."},

    {L"The volume works but the TV will not turn off.",
     L"Usually another device on the HDMI cabling is at fault, not the TV. A "
     L"device that has stopped listening, such as an adapter left plugged into "
     L"a PC that is asleep, or a soundbar in a bad state, blocks the messages "
     L"this PC uses to tell the TV where it is. The TV then still answers "
     L"volume, which is sent to it directly, and refuses anything to do with "
     L"power, because it no longer knows what this PC is. Unplug or switch off "
     L"the other device and it works again straight away. The log names it: "
     L"look for \"the CEC bus is refusing broadcasts\"."},

    {L"There is a red dot on the tray icon.",
     L"It means the background service is not running, so nothing can reach "
     L"the TV and the controls will do nothing. Right-click the icon and "
     L"click \"Service is not running\" at the top of the menu. Windows asks "
     L"for permission first, because starting a service needs it."},

    {L"There is a yellow dot on the tray icon.",
     L"It means \"Keep PC and TV awake\" is on, so this PC will not sleep or "
     L"turn its screen off on its own. Untick it in the menu when you are done. "
     L"If the service has stopped as well, the dot is red instead, because that "
     L"is the one to fix first."},

    {L"The TV keeps switching to its own home screen instead of showing the PC.",
     L"Smart TVs go back to their own launcher when they lose the signal or "
     L"wake up. Look in the TV's settings for something like \"resume last "
     L"input\" and turn it on. The TV then behaves like a monitor rather than a "
     L"smart TV with a PC attached."},

    {L"Stopping and starting the background service by hand.",
     L"Rarely needed, and worth knowing for two cases: reading a log from a "
     L"clean start, and clearing an adapter that has got into a bad state. "
     L"Administrator rights are required, so open Windows Terminal, PowerShell, "
     L"or Command Prompt by right-clicking it and choosing \"Run as "
     L"administrator\". Without that, each of these fails with an access "
     L"denied error. In PowerShell or Windows Terminal:\n"
     L"Restart-Service CecService\n"
     L"Stop-Service CecService\n"
     L"Start-Service CecService\n"
     L"Get-Service CecService\n"
     L"In Command Prompt the same four are:\n"
     L"net stop CecService && net start CecService\n"
     L"net stop CecService\n"
     L"net start CecService\n"
     L"sc query CecService\n"
     L"A restart is the one to reach for, because it cannot leave the service "
     L"off. While it is stopped nothing reaches the TV and the tray icon wears "
     L"a red dot, so start it again when you are done. It also starts itself "
     L"with Windows, so a reboot will do if you forget."},

    {L"Windows said \"Windows protected your PC\" when installing or updating.",
     L"That is SmartScreen. It appears for any app that has not yet been "
     L"downloaded by many people, or that is not signed with a paid "
     L"certificate, and this app is free and not signed yet. Click \"More "
     L"info\" and then \"Run anyway\". Windows then asks for permission and "
     L"shows the publisher as unknown, for the same reason; the installer "
     L"needs that permission because it sets up the background service.\n"
     L"Each new version can show the prompt again, because Windows judges "
     L"every version on its own."},

    {L"Will any DisplayPort to HDMI adapter work?",
     L"No. Most of them cannot carry CEC at all, and it is not something the "
     L"box or the listing will tell you. What decides it is the chip inside. "
     L"This app was built on the Chrontel CH7218, and the USB-C cable below "
     L"was bought because its manual named that same chip, then worked the "
     L"first time it was plugged in. Both of these are known to work.\n"
     L"UGREEN DP134 8K@60Hz Active DisplayPort to HDMI Unidirectional Adapter, "
     L"for a PC with a DisplayPort socket. This is the one most people want.\n"
     L"https://www.amazon.com/dp/B0FQCF62CD\n"
     L"USB C to HDMI 2.1 Cable 8K@60Hz 4K@120Hz Type-C to HDMI Cord, for a PC "
     L"or laptop with USB-C video out instead.\n"
     L"https://www.amazon.com/dp/B0D73MQ57G"},

    {L"The volume buttons in the app do nothing.",
     L"Some TVs accept a volume command and then ignore it, and the app "
     L"cannot tell the difference. Turn on \"Volume buttons change this PC's "
     L"volume, not the TV's\" under Settings. The buttons then change "
     L"Windows' volume instead. The sound in the room changes the same way, "
     L"and it happens instantly."},

    {L"Why is changing the PC volume not the default?",
     L"Plenty of TVs do respond to the volume commands, and if you have a "
     L"soundbar then the TV's volume is the one worth changing. Changing "
     L"Windows' volume is the fallback for when that does not work, not the "
     L"better option."},

    {L"I use this PC with more than one TV. Do I have to change the settings each time?",
     L"No. The app tells TVs apart, and the choice of \"Volume buttons change "
     L"this PC's volume, not the TV's\" is kept separately for each one. A TV "
     L"the app has not seen before starts with the choice you had before, and "
     L"changing it then only affects that TV."},

    {L"The TV remote does not control the PC.",
     L"Three things all have to be true. The TV has to forward its remote at "
     L"all, which not every TV does. The PC has to be the input currently on "
     L"screen. And the input must not be set up in the TV as a particular brand "
     L"of player: that setup sends infrared instead of using the HDMI cable, so "
     L"the buttons go to a device that is not there. Leave the input "
     L"unconfigured.\n"
     L"Which buttons arrive is up to the TV. Usually the arrows, OK and back, "
     L"and often play, pause, stop and skip. Buttons the TV keeps for itself, "
     L"such as volume and home, never arrive. To see which buttons your TV "
     L"sends, right-click \"TV remote controls this PC\" under Settings "
     L"and press them: each one is named as it arrives."},

    {L"My keyboard's volume keys change Windows, not the TV.",
     L"Turn on \"Keyboard volume keys change the TV\" under Settings, and the "
     L"volume keys on a keyboard, an air mouse, or an HTPC remote change the TV "
     L"instead. This only applies while volume is set to change the TV, since in "
     L"the other mode Windows already does the right thing. Windows' own volume "
     L"popup will not appear, because the key never reaches it."},

    {L"The mouse pointer keeps disappearing.",
     L"If it goes while you type, change \"Hide pointer while typing\" in "
     L"Windows: Settings, Bluetooth & devices, Mouse, Additional mouse "
     L"settings, Pointer Options.\n"
     L"If it goes when the mouse is still, \"Hide the mouse pointer when idle\" "
     L"is on. The pointer comes back the moment the mouse moves or a button is "
     L"pressed. Right-click the item to change how long it waits and the "
     L"movement tolerance, how far the mouse has to move before the pointer "
     L"comes back. If an air mouse keeps bringing it back while you are not "
     L"touching it, raise the tolerance."},

    {L"The volume buttons change Windows instead of the TV.",
     L"Windows is playing sound on something other than the TV, such as "
     L"headphones, a speaker, or another monitor. While it is, the app leaves "
     L"volume to Windows, and gives it back to the TV as soon as the TV is "
     L"the sound output again. To choose the TV, click the speaker icon next "
     L"to the clock and pick it from the list. If the TV is already chosen, "
     L"check that \"Volume buttons change this PC's volume, not the TV's\" is "
     L"off under Settings."},

    {L"The TV turns itself off after a few hours, even though the PC is on.",
     L"Many TVs switch themselves off when nobody has pressed a button on the "
     L"remote for a while, whatever is on screen. Look in the TV's own settings "
     L"for Auto Power Off, Eco, or an inactivity or sleep timer, and turn it off "
     L"or make it longer. That is the surest fix. Some TVs only use that timer "
     L"on their own home screen and apps, or when there is no signal, so with the "
     L"PC on screen it may never switch the TV off at all.\n"
     L"\"Keep PC and TV awake\" in the menu also tries to prevent it, by sending "
     L"the TV a button that normally does nothing every few minutes while this PC "
     L"is on screen. Not every TV counts that as someone using it, so check the "
     L"TV's setting first."},

    {L"What does \"Keep PC and TV awake\" do?",
     L"While it is ticked, this PC does not go to sleep or turn its screen off on "
     L"its own, and the TV is sent a button every few minutes so it does not "
     L"switch itself off. It stops when you untick it or close the app, and it is "
     L"not remembered after a restart, so it cannot be left on by accident. The "
     L"tray icon shows a yellow dot while it is on. A screen saver can still "
     L"start, because Windows does not let apps stop one."},

    {L"The TV takes a few seconds to turn off after the PC sleeps.",
     L"Most of that wait is the TV. The command is sent in well under a second. "
     L"How long a TV takes to act on it is not set by the standard, and ranges "
     L"from about half a second to three and a half."},

    {L"I woke the PC, slept it again straight away, and the TV stayed on.",
     L"TVs ignore a power off command for ten to fifteen seconds after being "
     L"turned on, so that they cannot be switched on and off rapidly. Leave a "
     L"little longer between the two and it works."},

    {L"The TV labels the input \"Playback_1\" or \"HDMI 1\" instead of the PC "
     L"name.",
     L"The TV decides when it will accept a name, and some keep the one they "
     L"already have until the next time they are switched off. The name is sent "
     L"every time the PC starts and whenever the TV asks for it, so it usually "
     L"corrects itself."},

    {L"Can I switch the TV to a different input from the PC?",
     L"Sometimes, and it depends on what is plugged in rather than on the PC. "
     L"The command asks whatever is on that port to put itself on screen, so "
     L"that device has to be awake and willing to answer. Streaming sticks, "
     L"consoles, and players usually are. Laptops and other computers usually "
     L"are not: a Mac, for example, only implements the part of HDMI-CEC that "
     L"turns the screen on and off. The exception is another PC running this "
     L"app, which answers while it is awake with its screen on. An empty port "
     L"has nothing there to answer at all."},

    {L"The shortcut worked, but the TV took a few seconds to switch.",
     L"If the input the TV is sitting on has nothing connected to it, the TV "
     L"keeps hunting for a signal there before it will accept anything else, "
     L"and only switches once it gives up. That can take ten seconds or more, "
     L"and the wait is the TV's own rather than the PC's. Coming from an input "
     L"with something live on it is much quicker."},

    {L"What happens if I choose Exit in the menu?",
     L"The TV still follows the PC. That part is a background service and keeps "
     L"running; Exit only closes the tray icon. To bring it back, open the app "
     L"from the Start menu, or sign out and back in if it is set to start with "
     L"Windows."},

    {L"What depends on your TV", nullptr},

    {L"Volume.",
     L"Some sets act on the commands, some acknowledge them and do nothing. "
     L"If yours does nothing, turn on \"Volume buttons change this PC's "
     L"volume, not the TV's\" under Settings: the buttons then change "
     L"Windows' own volume, which is the same result in the room and is "
     L"instant. Where a soundbar or receiver handles the sound, it may answer "
     L"instead of the TV."},

    {L"The remote buttons.",
     L"Some sets forward their remote to the input that is showing, some "
     L"forward only a few buttons, and some forward none at all. If your TV "
     L"offers to set the input up as a particular brand of player, decline: "
     L"that switches it to sending infrared at a device that is not there."},

    {L"The name on the input.",
     L"The TV may show \"Playback_1\" or \"HDMI 1\" before the PC name "
     L"appears. The TV decides when to accept a name, and some keep their own "
     L"until the next time they are switched off at the wall."},

    {L"The first command after a quiet spell.",
     L"It can take about a second, because the PC has to reintroduce itself to "
     L"the TV first. The ones after it are immediate."},

    {L"Running it from other programs", nullptr},

    {L"Game launchers, streaming hosts and scripts can control the TV.",
     L"Playnite, Steam, Sunshine's do and undo commands, AutoHotkey, Stream "
     L"Deck and Task Scheduler can all run this app with a command, for "
     L"example:\n"
     L"\"C:\\Program Files\\HDMI-CEC Display Control\\CecTray.exe\" tv-off"},

    {L"The commands.",
     L"tv-on, tv-off, switch-to-pc, input 1 to 4, volume-up, volume-down, "
     L"mute, set-volume 0 to 100, keep-awake on, and keep-awake off. tv-off "
     L"leaves a TV that is already off alone. Volume goes wherever this app's "
     L"volume buttons go, or to the TV when this app is not running, and "
     L"keep-awake needs this app running. The exit code is 0 when it worked, 1 "
     L"when the service or this app could not be reached, 2 for an unknown "
     L"command, and 3 when it was refused."},

    {L"The service log", nullptr},

    {L"Everything the service does is written down.",
     L"Turn on \"Keep a log\" under Settings, and right-click "
     L"that same item to open it. It lives at C:\\ProgramData\\CecService\\service.log, "
     L"and it is the first place to look when the TV does something "
     L"unexpected."},

    {L"What is in it.",
     L"Times, what the app asked the TV to do, and what the TV said back. It "
     L"also holds the name of this PC, which is the label the TV shows for this "
     L"input, and the make and model of the TV itself. Nothing about you or "
     L"what you do on the PC. It is a plain text file, so you can read the "
     L"whole thing before sending it to anybody."},

    {L"Support", nullptr},

    {L"Something not working, or an idea for it?",
     L"Write to business.pdm@proton.me. The log helps more than anything else, "
     L"and the part above says what is in it."},

    {L"If you would like to support the app.",
     L"It is free, and it stays free. If it saved you some trouble there is a "
     L"tip jar at https://ko-fi.com/pdm27, which is appreciated and never "
     L"expected."},
};

void AppendRtf(std::string& out, const wchar_t* text) {
    for (const wchar_t* at = text; *at; ++at) {
        switch (*at) {
            // The three characters RTF reserves. Everything here is ASCII, but
            // escaping is cheap and a stray brace would break the whole page.
            case L'\\': out += "\\\\"; break;
            case L'{':  out += "\\{";  break;
            case L'}':  out += "\\}";  break;
            case L'\n': out += "\\par\n"; break;
            default:
                if (*at < 128) {
                    out += static_cast<char>(*at);
                } else {
                    char escape[16];
                    snprintf(escape, sizeof(escape), "\\u%d?",
                             static_cast<int>(*at));
                    out += escape;
                }
                break;
        }
    }
}

// Bold questions and paragraph spacing come from here; the face, size and
// colour are applied afterwards, so that neither has to know about the other.
std::string BuildGuideRtf() {
    std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil Segoe UI;}}\\n";
    bool first = true;
    for (const auto& entry : kGuide) {
        // Space before each heading rather than blank lines between blocks: an
        // empty paragraph is a line that can be selected and scrolled past,
        // spacing is not.
        //
        // A section heading is larger and stands further from what came before,
        // and that is the whole of its formatting: on a page this dense, one
        // clear step in size does more than colours or rules would.
        if (!entry.body) {
            rtf += first ? "\\pard\\sb0\\sa160\\b\\fs32 " : "\\pard\\sb600\\sa160\\b\\fs32 ";
            AppendRtf(rtf, entry.heading);
            rtf += "\\b0\\fs22\\par\\n";
            first = false;
            continue;
        }
        rtf += first ? "\\pard\\sb0\\sa100\\b " : "\\pard\\sb400\\sa100\\b ";
        AppendRtf(rtf, entry.heading);
        rtf += "\\b0\\par\\n\\pard\\sb0\\sa140 ";
        AppendRtf(rtf, entry.body);
        rtf += "\\par\\n";
        first = false;
    }
    rtf += "}";
    return rtf;
}

struct RtfSource {
    const std::string* text;
    size_t at;
};

DWORD CALLBACK GuideRtfReader(DWORD_PTR cookie, LPBYTE buffer, LONG bytes,
                            LONG* read) {
    RtfSource* source = reinterpret_cast<RtfSource*>(cookie);
    size_t remaining = source->text->size() - source->at;
    size_t take = static_cast<size_t>(bytes) < remaining
                      ? static_cast<size_t>(bytes)
                      : remaining;
    memcpy(buffer, source->text->data() + source->at, take);
    source->at += take;
    *read = static_cast<LONG>(take);
    return 0;
}

// Face, size and colour, applied across everything already in the control.
//
// The mask is what makes this safe to run after the text is loaded: only the
// attributes named in it are touched, so the bold set by the RTF survives. It
// is also why the DPI and the theme can both be re-applied later without
// rebuilding the document.
void GuideApplyStyle(UINT dpi) {
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi)) {
        return;
    }
    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    wcscpy_s(format.szFaceName, metrics.lfMessageFont.lfFaceName);
    // lfHeight is negative character height in device units; yHeight is twips.
    format.yHeight =
        MulDiv(-metrics.lfMessageFont.lfHeight, 1440, static_cast<int>(dpi));
    format.crTextColor = GuideTextColour();
    SendMessageW(g_guideText, EM_SETCHARFORMAT, SCF_ALL,
                 reinterpret_cast<LPARAM>(&format));
    SendMessageW(g_guideText, EM_SETBKGNDCOLOR, 0,
                 static_cast<LPARAM>(GuideBackColour()));
}

void GuideLayout() {
    RECT client = {};
    GetClientRect(g_guideWindow, &client);
    MoveWindow(g_guideText, 0, 0, client.right, client.bottom, TRUE);

    // Padding inside the control rather than around it. The control's own
    // client rectangle already excludes the scrollbar, so the text is inset
    // from the edge while the scrollbar stays against it.
    const int padding = MulDiv(
        kGuideBasePadding, static_cast<int>(GetDpiForWindow(g_guideWindow)), 96);
    RECT inner = {};
    GetClientRect(g_guideText, &inner);
    RECT text = {padding, padding, inner.right - padding,
                 inner.bottom - padding};
    SendMessageW(g_guideText, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&text));
}

void GuideApplyTheme() {
    g_faqDark = AppsUseDarkTheme();
    if (g_guideBrush) DeleteObject(g_guideBrush);
    g_guideBrush = CreateSolidBrush(GuideBackColour());
    const BOOL useDark = g_faqDark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_guideWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark,
                          sizeof(useDark));
    // The scrollbar belongs to the text control, not to the window, and it is
    // drawn from a theme the control picks up when it is created. So the
    // control is marked in its own right, given Explorer's scrollbar theme -
    // which resolves to the dark one once the control is marked - and told the
    // theme has changed, since it will not go looking by itself.
    if (g_guideText) {
        FollowSystemThemeInWindow(g_guideText);
        SetWindowTheme(g_guideText, L"Explorer", nullptr);
        SendMessageW(g_guideText, WM_THEMECHANGED, 0, 0);
    }
}

WNDPROC g_guideTextProc = nullptr;

LRESULT CALLBACK GuideTextProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    const LRESULT result =
        CallWindowProcW(g_guideTextProc, window, message, wParam, lParam);
    // A page of text should not have a blinking insertion point in it. The
    // control keeps the focus, because that is what makes Page Up and Page Down
    // scroll, so the caret is hidden after the control has created it - and
    // again every time focus comes back, since it is recreated each time.
    if (message == WM_SETFOCUS) HideCaret(window);
    return result;
}

LRESULT CALLBACK GuideProc(HWND window, UINT message, WPARAM wParam,
                         LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            if (g_guideText) GuideLayout();
            return 0;

        // The control covers the whole client area, so this only ever shows
        // during a resize. Painting it the same colour is what stops a pale
        // edge flashing around the text.
        case WM_ERASEBKGND:
            if (g_guideBrush) {
                RECT client = {};
                GetClientRect(window, &client);
                FillRect(reinterpret_cast<HDC>(wParam), &client, g_guideBrush);
                return 1;
            }
            break;

        // Someone switching Windows between light and dark while the window is
        // open. Cheap to follow, and a page left in the wrong colours is the
        // sort of thing that only ever gets noticed by the person reading it.
        case WM_SETTINGCHANGE:
            if (lParam && g_guideText &&
                _wcsicmp(reinterpret_cast<const wchar_t*>(lParam),
                         L"ImmersiveColorSet") == 0) {
                FollowSystemThemeInWindow(window);
                GuideApplyTheme();
                GuideApplyStyle(GetDpiForWindow(window));
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        // Follow the display rather than assume it. Same lesson as the volume
        // panel: anything sized from a scale has to be resized when it changes.
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            GuideApplyStyle(HIWORD(wParam));
            GuideLayout();
            return 0;
        }

        // A link in the page was clicked. The control reports the two
        // character positions it spans, and the text between them is the
        // address itself.
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (!header || header->code != EN_LINK) break;
            auto* link = reinterpret_cast<ENLINK*>(lParam);
            // EN_LINK also arrives for the pointer passing over one, which is
            // how the control asks whether to change the cursor.
            if (link->msg != WM_LBUTTONUP || !g_guideText) return 0;
            wchar_t url[512] = {};
            const LONG length = link->chrg.cpMax - link->chrg.cpMin;
            if (length <= 0 || length >= static_cast<LONG>(_countof(url))) {
                return 0;
            }
            TEXTRANGEW range = {};
            range.chrg = link->chrg;
            range.lpstrText = url;
            SendMessageW(g_guideText, EM_GETTEXTRANGE, 0,
                         reinterpret_cast<LPARAM>(&range));
            // Only the two schemes this page ever carries. Every address in it
            // is the app's own, so this is not defending against anything so
            // much as refusing to be the thing that opens whatever was found in
            // a document.
            if (wcsncmp(url, L"https://", 8) == 0 ||
                wcsncmp(url, L"http://", 7) == 0) {
                ShellExecuteW(nullptr, L"open", url, nullptr, nullptr,
                              SW_SHOWNORMAL);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            g_guideWindow = nullptr;
            g_guideText = nullptr;
            if (g_guideBrush) { DeleteObject(g_guideBrush); g_guideBrush = nullptr; }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Puts a heading at the top of the page, for a caller opening the guide about
// one particular thing.
//
// Without it, somebody who clicked "No CEC TV found" arrives at the top of
// a long page and has to go looking for the answer they have just asked for,
// which is the same dead end one step further along.
//
// Scrolled by line rather than by caret: EM_SCROLLCARET brings a position only
// just into view, which in practice means against the bottom edge, and a
// heading at the bottom of the window reads as the end of the answer above it.
void GuideScrollTo(const wchar_t* heading) {
    if (!heading || !g_guideText) return;
    FINDTEXTEXW find = {};
    find.chrg.cpMin = 0;
    find.chrg.cpMax = -1;
    find.lpstrText = heading;
    const LONG at = static_cast<LONG>(SendMessageW(
        g_guideText, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find)));
    if (at < 0) return;
    // Measured against the line actually at the top rather than assuming the
    // page starts there: EM_LINESCROLL moves by a number of lines from wherever
    // the control is, and this is also called on a window that is already open
    // and scrolled somewhere else entirely.
    const LONG line = static_cast<LONG>(
        SendMessageW(g_guideText, EM_EXLINEFROMCHAR, 0, at));
    const LONG first = static_cast<LONG>(
        SendMessageW(g_guideText, EM_GETFIRSTVISIBLELINE, 0, 0));
    SendMessageW(g_guideText, EM_LINESCROLL, 0, line - first);
}

void ShowGuide(const wchar_t* jumpTo = nullptr) {
    // One window, reused. A second menu click raises the one already open
    // rather than stacking another on top of it.
    if (g_guideWindow) {
        if (IsIconic(g_guideWindow)) ShowWindow(g_guideWindow, SW_RESTORE);
        SetForegroundWindow(g_guideWindow);
        GuideScrollTo(jumpTo);
        return;
    }

    // Registers the RICHEDIT50W window class as a side effect of loading, and
    // is deliberately never freed: the class has to outlive every window using
    // it, and this process may open the page again.
    static HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit) {
        MessageBoxW(nullptr, L"The guide could not be opened.", kAppName,
                    MB_OK | MB_ICONWARNING);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass = {sizeof(windowClass)};
        windowClass.lpfnWndProc = GuideProc;
        windowClass.hInstance = g_instance;
        windowClass.lpszClassName = kGuideClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;   // WM_ERASEBKGND does it
        windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APPICON));
        windowClass.hIconSm = windowClass.hIcon;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    // Sized and centred against the monitor it is about to appear on, asked of
    // the monitor rather than of a window that does not exist yet.
    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiX = 96;
    }
    MONITORINFO info = {sizeof(info)};
    GetMonitorInfoW(monitor, &info);

    const int width = MulDiv(kGuideBaseWidth, static_cast<int>(dpiX), 96);
    const int height = MulDiv(kGuideBaseHeight, static_cast<int>(dpiX), 96);
    const int x =
        info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y =
        info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;

    g_guideWindow = CreateWindowExW(0, kGuideClass, L"HDMI-CEC Display Control Guide",
                                  WS_OVERLAPPEDWINDOW, x, y, width, height,
                                  nullptr, nullptr, g_instance, nullptr);
    if (!g_guideWindow) return;
    FollowSystemThemeInWindow(g_guideWindow);
    GuideApplyTheme();

    g_guideText = CreateWindowExW(
        0, MSFTEDIT_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_guideWindow, nullptr, g_instance, nullptr);
    if (!g_guideText) {
        DestroyWindow(g_guideWindow);
        return;
    }

    g_guideTextProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_guideText, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GuideTextProc)));
    // Again, now that there is a control to theme: the first call happened
    // before it existed.
    GuideApplyTheme();

    // Addresses in the text become links. They are written as plain text in
    // the table above, so that rewording an answer never means touching any
    // formatting, and the control is left to find them. The mask is what makes
    // a click reach this window at all; without it the address is underlined,
    // looks like a link, and does nothing.
    SendMessageW(g_guideText, EM_AUTOURLDETECT, AURL_ENABLEURL, 0);
    SendMessageW(g_guideText, EM_SETEVENTMASK, 0, ENM_LINK);

    const std::string rtf = BuildGuideRtf();
    RtfSource source = {&rtf, 0};
    EDITSTREAM stream = {};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&source);
    stream.pfnCallback = GuideRtfReader;
    SendMessageW(g_guideText, EM_STREAMIN, SF_RTF,
                 reinterpret_cast<LPARAM>(&stream));

    GuideApplyStyle(dpiX);
    GuideLayout();
    // Streaming leaves the insertion point at the end, which scrolls the
    // control to the bottom of a page nobody has read yet.
    SendMessageW(g_guideText, EM_SETSEL, 0, 0);
    SendMessageW(g_guideText, EM_SCROLLCARET, 0, 0);

    ShowWindow(g_guideWindow, SW_SHOW);
    SetForegroundWindow(g_guideWindow);
    SetFocus(g_guideText);   // so Page Up and Page Down scroll it
    // After the window is on screen, never before. A control that has not been
    // shown does not know which line is at the top of it, so scrolling it does
    // nothing at all - which is why "click for help" landed at the top of the
    // page rather than at the section it names.
    GuideScrollTo(jumpTo);
}

void OpenLog() {
    wchar_t programData[MAX_PATH] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring path = (length && length < MAX_PATH)
                            ? std::wstring(programData) + L"\\CecService\\service.log"
                            : L"C:\\CecService\\service.log";
    ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

// Hover warm-ups are throttled to this. The service treats 15s of silence as
// grounds to re-announce, so anything in that ballpark is enough to have the
// refresh already finished by the time a button is pressed.
constexpr DWORD kPrepareIntervalMs = 3000;
DWORD g_lastPrepareTick = 0;

void OnCommand(UINT id) {
    switch (id) {
        case kMenuTvOn:          g_commands.Post("CMD tv-on"); break;
        case kMenuKeepAwake:     SetKeepAwake(!g_keepAwake); break;
        case kMenuTvStandby:     g_commands.Post("CMD tv-standby"); break;
        case kMenuLogging:
            ToggleSetting("LoggingEnabled", "log");
            break;
        case kMenuStandbyShutdown:
            ToggleSetting("StandbyOnShutdown", "shutdown");
            break;
        case kMenuStandbySleep:
            ToggleSetting("StandbyOnSleep", "sleep");
            break;
        case kMenuStandbyDisplayOff:
            ToggleSetting("StandbyOnDisplayOff", "blank");
            break;
        case kMenuForwardRemoteKeys:
            ToggleSetting("ForwardRemoteKeys", "keys");
            break;
        case kMenuVolumeToPc:
            // Updated here as well as from STATUS, so the very next press obeys
            // the change rather than waiting for the menu to be opened again.
            // Set to the value asked for rather than flipped, so the two can
            // never disagree about which way the click went.
            g_volumeToPc = ToggleSetting("VolumeControlsPc", "pcvol");
            // The number now comes from somewhere else, so the old one is not
            // merely stale, it is about a different thing.
            g_volumePanel.SetReadout(false, 0, false);
            ForgetVolumeSupport();
            UpdateKeyboardVolume();
            break;
        case kMenuHotkeyEnabled:
            g_hotkeyEnabled = !g_hotkeyEnabled;
            // Stored only once it is known to work, so a collision does
            // not leave the setting claiming something that is not live.
            if (ApplyHotkey(true)) {
                WriteAppDword(L"HotkeyEnabled", g_hotkeyEnabled ? 1 : 0);
            } else {
                g_hotkeyEnabled = false;
            }
            break;
        case kMenuHotkeyChange: ShowHotkeyCapture(); break;
        case kMenuRemoteKeysChange: ShowRemoteCapture(); break;
        case kMenuHidePointer:
            WriteAppDword(L"HidePointer", ReadAppDword(L"HidePointer", 0) ? 0 : 1);
            ApplyPointerOptions();
            break;
        case kMenuHidePointerOptions: ShowPointerOptions(); break;
        case kMenuKeyboardVolume:
            WriteAppDword(L"KeyboardVolumeToTv",
                          ReadAppDword(L"KeyboardVolumeToTv", 0) ? 0 : 1);
            UpdateKeyboardVolume();
            // Checked a moment later rather than assumed. Windows can refuse a
            // keyboard hook, and a setting that ticks itself on and then does
            // nothing is the hardest kind of fault to report.
            SetTimer(g_window, kHookCheckTimer, kHookCheckMs, nullptr);
            break;
        case kMenuStartWithWindows: ToggleStartWithWindows(); break;
        case kMenuGuide:           ShowGuide(); break;
        // The whole section rather than one answer in it. Somebody who has
        // just installed this and seen no TV found does not yet know which of
        // the reasons is theirs, and the first of them is that it has not
        // finished looking.
        case kMenuNoDisplayHelp:
            ShowGuide(L"If something does not work");
            break;
        case kMenuOpenLog:       OpenLog(); break;
        case kMenuLaunchService: LaunchService(); break;
        case kMenuExit:          DestroyWindow(g_window); break;
        default:
            // One of the HDMI inputs, from the list built when the menu opened.
            // The service works out how to reach it; a failure comes back as an
            // error and is reported, since a television that does not switch
            // says nothing about why.
            if (id >= kMenuFirstInput && id <= kMenuLastInput) {
                const size_t index = id - kMenuFirstInput;
                if (index < g_inputs.size()) {
                    // Digits and dots, so narrowing is exact.
                    std::string port;
                    for (const wchar_t c : g_inputs[index].port) {
                        port.push_back(static_cast<char>(c));
                    }
                    g_commands.Post("CMD switch-to " + port);
                }
            }
            break;
    }

}

// ------------------------------------------------------------ command line
//
// "CecTray.exe tv-off" and the like, for anything that can run a program: game
// launchers, Sunshine's do and undo commands, AutoHotkey, Stream Deck, Task
// Scheduler. The listed commands and exit codes are in Important Info.
//
// Handled before anything else in WinMain, so a command never starts a second
// tray, touches the icon, or waits on the single-instance check. It is the same
// executable rather than a separate console tool on purpose: a console program
// started by a launcher or a scheduled task flashes a console window over
// whatever is on the TV, and this one opens no window at all. The cost is that
// a terminal does not wait for it or show text, so the result is the exit code.
//
// Most commands are the service's own, sent the way the tray sends them, and
// work whether or not the tray is running. Keep-awake goes to the running tray
// instead, carried in WM_COPYDATA, because that lease is held by the tray's
// process. Volume asks the tray first, since where volume goes - the TV, or this
// PC while its setting is on or sound plays elsewhere - is the tray's decision.
// The tray changes this PC's volume itself, but when volume goes to the TV it
// only says so, and the command sends it to the service and waits for the
// answer. Handing it to the tray's queue instead returned success at once, even
// when the TV, switched off, never acknowledged it. With no tray running, volume
// goes to the TV, which is where it goes without one.

enum CommandLineExit : int {
    kExitDone = 0,
    kExitUnreachable = 1,   // the service, or for keep-awake the tray, is not running
    kExitUnknown = 2,       // not a command, or a bad argument
    kExitRefused = 3,       // the service answered with an error
};

// What the tray answers a volume command with when volume goes to the TV: the
// command line sends it to the service itself. See RunCommandLine.
constexpr LRESULT kTrayVolumeGoesToTv = 2;

// Marks this app's own WM_COPYDATA, so data some other program sends the
// window is never read as a command. "CECC" in ASCII.
constexpr ULONG_PTR kCommandDataTag = 0x43454343;

// On the running tray's UI thread. Returns 1 if the command was carried out, 0
// if it is not one the tray knows, and kTrayVolumeGoesToTv for volume that is
// the TV's to change.
LRESULT RunForCommandLine(const std::string& command) {
    if (command == "keep-awake on" || command == "keep-awake off") {
        const bool on = command == "keep-awake on";
        if (g_keepAwake != on) SetKeepAwake(on);
        return 1;
    }
    // A tap, as the panel's buttons make one: a press and its release.
    const char* press = command == "volume-up"     ? "press-volume-up"
                        : command == "volume-down" ? "press-volume-down"
                        : command == "mute"        ? "press-mute"
                                                   : nullptr;
    const bool setLevel = command.compare(0, 11, "set-volume ") == 0;
    if (!press && !setLevel) return 0;
    if (!VolumeGoesToPc()) return kTrayVolumeGoesToTv;
    if (press) {
        SendVolumeCommand(press);
        SendVolumeCommand("release");
    } else {
        SendVolumeCommand(command.c_str());
    }
    return 1;
}

// The tray's answer, or -1 when no tray is running or it did not answer.
LRESULT SendToTray(const std::string& command) {
    HWND tray = FindWindowW(kWindowClass, nullptr);
    if (!tray) return -1;
    // lpData is never written through; the cast only satisfies the struct.
    COPYDATASTRUCT data = {
        kCommandDataTag, static_cast<DWORD>(command.size()),
        const_cast<void*>(static_cast<const void*>(command.data()))};
    DWORD_PTR result = 0;
    // Bounded, and abandoned if the tray is hung, so a script is never held up
    // by it for more than a few seconds.
    if (!SendMessageTimeoutW(tray, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                             SMTO_ABORTIFHUNG, 5000, &result)) {
        return -1;
    }
    return static_cast<LRESULT>(result);
}

int SendToService(const std::string& request) {
    const std::string reply = SendRequest(request);
    if (reply.empty()) return kExitUnreachable;
    return reply.rfind("OK", 0) == 0 ? kExitDone : kExitRefused;
}

// Returns -1 when there is no command, and the process goes on to be the tray.
int RunCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return -1;
    // Lower case, ASCII only; anything else cannot be one of the commands.
    std::vector<std::string> words;
    bool ascii = true;
    for (int i = 1; i < count; ++i) {
        std::string word;
        for (const wchar_t* c = arguments[i]; *c; ++c) {
            if (*c > 0x7E || *c < 0x20) ascii = false;
            word += static_cast<char>(*c >= L'A' && *c <= L'Z' ? *c + 32 : *c);
        }
        words.push_back(word);
    }
    LocalFree(arguments);
    if (words.empty()) return -1;
    if (!ascii) return kExitUnknown;

    // "--tv-off" and "/tv-off" are forgiven, since both are habits.
    std::string verb = words[0];
    while (!verb.empty() && (verb[0] == '-' || verb[0] == '/')) verb.erase(0, 1);
    const std::string argument = words.size() > 1 ? words[1] : std::string();
    const bool one = words.size() == 1;
    const bool two = words.size() == 2;
    const auto isNumber = [](const std::string& text, int low, int high) {
        if (text.empty() || text.size() > 3 ||
            text.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const int value = atoi(text.c_str());
        return value >= low && value <= high;
    };

    if (one && verb == "tv-on") return SendToService("CMD tv-on");
    if (one && verb == "tv-off") return SendToService("CMD tv-standby");
    if (one && verb == "switch-to-pc") return SendToService("CMD active-source");
    if (two && verb == "input" && isNumber(argument, 1, 4)) {
        return SendToService("CMD input " + argument);
    }
    if (two && verb == "keep-awake" && (argument == "on" || argument == "off")) {
        const LRESULT result = SendToTray("keep-awake " + argument);
        return result < 0 ? kExitUnreachable : result == 1 ? kExitDone : kExitRefused;
    }
    std::string volume;
    if (one && (verb == "volume-up" || verb == "volume-down" || verb == "mute")) {
        volume = verb;
    } else if (two && verb == "set-volume" && isNumber(argument, 0, 100)) {
        // Written back from the number, so "050" is sent as "50".
        volume = "set-volume " + std::to_string(atoi(argument.c_str()));
    }
    if (!volume.empty()) {
        const LRESULT tray = SendToTray(volume);
        if (tray == 1) return kExitDone;
        // A tray that exists but did not answer in time is not overruled: it
        // may be about to send this to Windows, and the TV must not get it too.
        if (tray < 0 && FindWindowW(kWindowClass, nullptr)) return kExitUnreachable;
        if (tray == 0) return kExitRefused;
        return SendToService("CMD " + volume);
    }
    return kExitUnknown;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        // A command from "CecTray.exe <command>". See RunCommandLine.
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!data || data->dwData != kCommandDataTag || !data->lpData ||
                data->cbData == 0 || data->cbData > 64) {
                return 0;
            }
            return RunForCommandLine(
                std::string(static_cast<const char*>(data->lpData), data->cbData));
        }
        case kTrayMessage:
            // Left click goes straight to the thing people use repeatedly;
            // right click keeps the full menu.
            if (LOWORD(lParam) == WM_MOUSEMOVE) {
                // Warm the CEC registration on hover rather than on the click.
                //
                // A stale registration costs about a second to refresh, and
                // starting that on the click that opens the panel means it is
                // still running when the first button is pressed - which is
                // the residual delay after the app has sat unused. The pointer
                // arrives at the icon before the click does, so this hides
                // most of it behind a movement that was happening anyway.
                //
                // WM_MOUSEMOVE repeats the whole time the pointer rests on the
                // icon, hence the interval: without it this would post a
                // command per mouse movement.
                const DWORD now = GetTickCount();
                if (!g_volumePanel.visible() &&
                    (!g_lastPrepareTick ||
                     now - g_lastPrepareTick >= kPrepareIntervalMs)) {
                    g_lastPrepareTick = now;
                    // Which television is there, and so where the volume
                    // buttons go, may have changed while the PC slept. STATUS
                    // does not touch the hardware, so asking costs nothing, and
                    // asking here means the answer is in before the click.
                    g_commands.Post("STATUS-REFRESH");
                    g_commands.Post("CMD prepare");
                    RefreshReadout();
                }
            } else if (LOWORD(lParam) == WM_LBUTTONUP) {
                // Warm the link only when a panel actually opened. Asking for
                // it on the click that closes the panel is wasted work.
                if (g_volumePanel.Toggle()) {
                    // Someone is about to use volume, so where it goes is made
                    // certain first. See kServiceWatchTimer.
                    RequestOutputCheck();
                    g_commands.Post("CMD prepare");
                    RefreshReadout();
                }
            } else if (LOWORD(lParam) == WM_RBUTTONUP) {
                g_volumePanel.Hide();
                ShowMenu();
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;

        case kVolumeReadoutMessage: {
            // The thumb belongs to the pointer during a drag. And an answer
            // left over from before switching to this PC's own volume is about
            // a television that is no longer the thing being changed. During a
            // knob's burst the target is what is shown, and an answer may be
            // from before its last level left.
            if (g_burst.active && g_burst.awaitingBase && wParam <= 100 &&
                !VolumeGoesToPc()) {
                // The level a waiting burst starts from, with its taps on top.
                g_volumeSupport = VolumeSupport::kYes;
                g_volumeMisses = 0;
                g_lastAnswerTick = GetTickCount();
                g_follow.muted = lParam != 0;
                g_burst.awaitingBase = false;
                g_burst.target = static_cast<int>(
                    ClampLevel(static_cast<long>(wParam) + g_burst.delta));
                g_volumePanel.SetReadout(true, g_burst.target, g_follow.muted);
                SendBurstLevel();
                return 0;
            }
            if (g_volumePanel.dragging() || VolumeGoesToPc() ||
                BurstOwnsReadout()) {
                return 0;
            }
            if (wParam == kVolumeRefused) {
                GiveUpOnVolume();
                return 0;
            }
            if (wParam == kVolumeUnknown) {
                // Silence while the level is still being fetched ahead of any
                // key: asked again shortly, and not held against the set.
                if (g_warmTriesLeft > 0 && !g_follow.baseKnown) {
                    SetTimer(window, kWarmReadoutTimer, kWarmRetryMs, nullptr);
                    return 0;
                }
                // Silence. The slider stays on its last level unless there
                // is a long run of it - see g_volumeSupport.
                const int limit = (g_volumeSupport == VolumeSupport::kYes ||
                                   g_tvReportsVolume.load())
                                      ? kKnownMissLimit
                                      : kUnknownMissLimit;
                if (g_volumeSupport != VolumeSupport::kNo &&
                    ++g_volumeMisses >= limit) {
                    GiveUpOnVolume();
                }
                return 0;
            }
            g_volumeSupport = VolumeSupport::kYes;
            g_volumeMisses = 0;
            g_lastAnswerTick = GetTickCount();
            g_warmTriesLeft = 0;
            if (g_burst.checkTarget >= 0) {
                // The first answer after a burst. A point either way allows
                // for a set that rounds levels to its own scale.
                const int miss = static_cast<int>(wParam) - g_burst.checkTarget;
                if (miss > 1 || miss < -1) {
                    if (++g_levelMismatches >= 2) g_levelCommandWorks = false;
                } else {
                    g_levelMismatches = 0;
                }
                g_burst.checkTarget = -1;
            }
            g_volumePanel.SetReadout(
                true, FollowReport(static_cast<int>(wParam), lParam != 0),
                lParam != 0);
            return 0;
        }

        // A press has reached the TV. The mute flag stays as last reported; a
        // volume key that unmutes the set shows up with the next answer.
        case kVolumeStepMessage: {
            const long step = wParam ? 1 : -1;
            g_follow.steps += step;
            g_follow.sinceQuestion += step;
            if (g_follow.sinceQuestion < g_follow.lowestSince) {
                g_follow.lowestSince = g_follow.sinceQuestion;
            }
            if (g_follow.sinceQuestion > g_follow.highestSince) {
                g_follow.highestSince = g_follow.sinceQuestion;
            }
            if (g_follow.trusted && g_follow.baseKnown &&
                g_volumeSupport == VolumeSupport::kYes &&
                !g_volumePanel.dragging() && !VolumeGoesToPc() &&
                !BurstOwnsReadout()) {
                g_volumePanel.SetReadout(true, static_cast<int>(FollowedLevel()),
                                         g_follow.muted);
            }
            return 0;
        }

        case kVolumeAskedMessage:
            g_follow.sinceQuestion = 0;
            g_follow.lowestSince = 0;
            g_follow.highestSince = 0;
            // The first question to leave after a burst's last level. From
            // here the count starts again at the burst's target, with every
            // step so far behind it, so the answer is taken as it is rather
            // than weighed against clicks that the level command overrode.
            if (g_burst.rebasePending && !g_burst.active && g_levelFramesOut == 0) {
                g_burst.rebasePending = false;
                g_follow.baseKnown = true;
                g_follow.base = g_burst.checkTarget;
                g_follow.stepsAtBase = g_follow.steps;
            }
            return 0;

        case kVolumeLevelSentMessage:
            if (g_levelFramesOut > 0) --g_levelFramesOut;
            return 0;

        case kAudioOutputMessage: {
            g_outputCheckPending = false;
            const bool elsewhere = OutputIsElsewhere();
            if (elsewhere != g_outputElsewhere.load()) {
                // Anything still held is let go first, while it still goes
                // where it was pressed: a release sent after the switch would
                // go to Windows, and the TV would carry on repeating the key.
                // The panel stays open, since the switch may have been made
                // from its own picker.
                ReleaseVolumeKey();
                EndBurst();
                g_volumePanel.ReleaseHeld();
                g_outputElsewhere = elsewhere;
                SendMessageW(window, kVolumeRoutingMessage, 0, 0);
            }
            // An output came, went, or became the default: an open picker
            // shows it.
            g_volumePanel.OutputsChanged();
            return 0;
        }

        // The volume buttons now go somewhere else, because the service says
        // this television has its own setting. Handled exactly as the menu item
        // that changes it by hand is: the number on screen was about the other
        // target, not merely out of date.
        // Both halves of the same fact. The slider loses its thumb, and the
        // keyboard stops trying to reach a level by sending one, which is what
        // g_levelCommandWorks already meant when two readouts in a row had come
        // back wrong. A refusal says it in one frame instead.
        case kLevelsRefusedMessage: {
            // Both ways. A television that takes levels gets its slider back,
            // which matters on the only path that changes the answer: moving
            // the PC to a different set.
            const bool refused = g_levelsRefused.load();
            g_volumePanel.SetLevelsSettable(!refused);
            g_levelCommandWorks = !refused;
            return 0;
        }

        case kVolumeRoutingMessage:
            // Anything still held is let go first, as for a change of sound
            // output: the routing that decided where this message goes has
            // already changed, and a key left held would carry on being
            // re-sent to the new target. The release itself goes where the
            // press went - see SendVolumeCommand.
            ReleaseVolumeKey();
            EndBurst();
            g_volumePanel.ReleaseHeld();
            // Volume has moved between the television and this PC. Levels work
            // again whenever they are this PC's own, whatever the bus refused.
            g_volumePanel.SetLevelsSettable(!g_levelsRefused.load());
            // And a television that answered two level commands wrongly gets
            // a fresh hearing on what may now be a different set, rather than
            // knobs sending taps for the rest of the session. A refusal it
            // states outright is still believed.
            g_levelCommandWorks = !g_levelsRefused.load();
            g_levelMismatches = 0;
            g_volumePanel.SetReadout(false, 0, false);
            ForgetVolumeSupport();
            // Which way the keyboard's volume keys should go has changed with
            // it, and in this PC's own mode Windows handles them itself.
            UpdateKeyboardVolume();
            if (g_volumePanel.visible()) RefreshReadout();
            return 0;

        case kHandOverFailedMessage: {
            if (wParam) {
                // The device is there and listening, so none of the three
                // reasons below can be it. Some devices, Apple TV among them,
                // only ever take the screen when their own remote asks: they
                // join the bus, answer for themselves, and ignore a request to
                // become the picture. Nothing to fix and nothing to retry, so
                // this says what to do instead.
                //
                // It deliberately names no device. Naming one meant capturing
                // the name when the item was clicked and reading it when the
                // answer came back, and those are eight seconds apart or more,
                // because a failed switch sends two requests and then queries
                // the bus. Anybody who clicks another input while waiting -
                // which is exactly what a person does when nothing happens -
                // had the name overwritten, and the balloon then told them to
                // reach for the remote of a device that had worked perfectly.
                // Seen in the field: an Apple TV failure at 13:24:56 named the
                // Steam Deck, which had taken the screen at 13:24:58.
                //
                // Carrying the name along with the answer instead would mean
                // marshalling a string onto the UI thread for a sentence that
                // adds nothing: whoever is reading this balloon pressed that
                // row a moment ago. Short is also what it has to be, since the
                // first version was cut off mid-sentence by the notification.
                // The failure first, then why, then what to do, because
                // somebody reading this has just watched nothing happen. The
                // earlier wording opened with "It is connected", which is
                // true, vague, and not what they want to know. Saying the
                // device does not respond to the signal carries the same fact
                // better: it was reached, and it will not act on being asked.
                //
                // Both remotes are named rather than "the remote". A device
                // with a remote of its own is the common case, but a console
                // or a stick that lives on a controller has none, and the
                // input button on the TV remote always works.
                ShowBalloon(L"Could not switch input",
                            L"That device does not respond to the switch "
                            L"signal. Use its own remote, or the TV remote, "
                            L"instead.",
                            NIIF_INFO);
                return 0;
            }
            ShowBalloon(L"The TV did not switch",
                        L"Nothing on that input answered. There may be no "
                        L"device there, or it may have CEC turned off. A PC "
                        L"with its screen off will not take the TV either.",
                        NIIF_INFO);
            return 0;
        }

        case kServiceGoneMessage:
            SetServiceDown(!ServiceIsRunning());
            ShowBalloon(L"Nothing happened",
                        L"The background service is not running, so that "
                        L"command went nowhere. Right-click the tray icon and "
                        L"start it from the top of the menu.",
                        NIIF_WARNING);
            return 0;

        case kServiceCheckMessage:
            SetServiceDown(!ServiceIsRunning());
            return 0;

        // The service timers ask the same question. The one-shot is the look
        // back after a start was asked for; the other is the standing watch.
        case kServiceListeningMessage:
            if (g_keyboardVolumeWanted.load()) {
                g_warmTriesLeft = kWarmTries;
                SetTimer(window, kWarmReadoutTimer, kWarmReadoutMs, nullptr);
            }
            return 0;

        case kVolumeKeyMessage:
            OnVolumeKey(static_cast<int>(wParam), lParam != 0);
            return 0;

        case WM_TIMER:
            if (wParam == kPointerTimer) {
                g_pointerHider.Tick();
                return 0;
            }
            if (wParam == kScreenOffTimer) {
                CheckScreenOff();
                return 0;
            }
            if (wParam == kInputWatchTimer) {
                CheckForInput();
                return 0;
            }
            if (wParam == kHookCheckTimer) {
                KillTimer(window, kHookCheckTimer);
                if (g_keyboardVolumeWanted.load() && !g_hookActive.load()) {
                    ShowBalloon(L"Volume keys",
                                L"Windows would not let this app take over the "
                                L"keyboard's volume keys. Close the app and "
                                L"start it again, and if it still will not, "
                                L"turn the setting off.",
                                NIIF_WARNING);
                }
                return 0;
            }
            if (wParam == kKeyVolumeTimer) {
                if (!g_keyVolumeHeld) {
                    KillTimer(window, kKeyVolumeTimer);
                } else if (GetTickCount() - g_keyVolumeLastDown > kKeyVolumeGraceMs) {
                    ReleaseVolumeKey();
                } else if (const char* command = PressCommandFor(g_keyVolumeHeld)) {
                    SendVolumeCommand(command);
                }
                return 0;
            }
            if (wParam == kWarmReadoutTimer) {
                KillTimer(window, kWarmReadoutTimer);
                // Not needed once a level is known, which a key pressed in the
                // meantime may already have brought.
                if (g_warmTriesLeft > 0 && !g_follow.baseKnown &&
                    g_keyboardVolumeWanted.load() && !VolumeGoesToPc() &&
                    MayAskVolume()) {
                    --g_warmTriesLeft;
                    AskForVolume();
                } else {
                    g_warmTriesLeft = 0;
                }
                return 0;
            }
            if (wParam == kBurstTimer) {
                if (!g_burst.active) {
                    KillTimer(window, kBurstTimer);
                } else if (g_burst.awaitingBase) {
                    // Still no answer. Given a while, then dropped.
                    if (GetTickCount() - g_burst.start >= kBaseWaitMs) EndBurst();
                } else if (GetTickCount() - g_burst.lastTap >= kBurstQuietMs) {
                    EndBurst();
                } else {
                    SendBurstLevel();   // the latest target, one frame a tick
                }
                return 0;
            }
            if (wParam == kKeepAwakeTimer) {
                g_commands.Post("CMD keep-awake");
                return 0;
            }
            if (wParam == kVolumeConfirmTimer) {
                KillTimer(window, kVolumeConfirmTimer);
                AskForVolume();
                return 0;
            }
            if (wParam == kServiceSoonTimer) KillTimer(window, kServiceSoonTimer);
            SetServiceDown(!ServiceIsRunning());
            // The sound output is looked at again on the same half-minute watch.
            // Windows' change notifications are what normally drive it, and
            // they stop without a word if its audio service restarts; this puts
            // volume back where it belongs within half a minute even then. A
            // local check, and nothing happens unless the answer changed.
            if (wParam == kServiceWatchTimer) {
                // Keeps what the menu draws from no more than half a minute
                // old, whether or not anybody has hovered over the icon.
                g_commands.Post("STATUS-REFRESH");
                RequestOutputCheck();
                // See KeyboardHookThread: a hook Windows dropped is put back.
                if (g_keyboardVolumeWanted.load() && g_hookThreadId) {
                    PostThreadMessageW(g_hookThreadId, kHookApply, 1, 0);
                }
            }
            return 0;

        // Right-click on a menu item. wParam is the item's position, lParam the
        // menu holding it - which is the submenu, so this works for items
        // nested under Settings without any extra bookkeeping.
        //
        // The menu is ended first and the command posted rather than called,
        // so the dialog opens after the menu's own modal loop has unwound
        // instead of underneath it.
        case WM_MENURBUTTONUP: {
            // Every setting that hides something behind a right-click, in one
            // place: the item clicked on the left, what it opens on the right.
            static const struct { UINT item; UINT opens; } kRightClick[] = {
                {kMenuHotkeyEnabled, kMenuHotkeyChange},
                {kMenuForwardRemoteKeys, kMenuRemoteKeysChange},
                {kMenuHidePointer, kMenuHidePointerOptions},
                {kMenuLogging, kMenuOpenLog},
            };
            const UINT clicked = GetMenuItemID(reinterpret_cast<HMENU>(lParam),
                                               static_cast<int>(wParam));
            for (const auto& entry : kRightClick) {
                if (entry.item != clicked) continue;
                EndMenu();
                PostMessageW(window, WM_COMMAND, entry.opens, 0);
                break;
            }
            return 0;
        }

        // Sends tv-on rather than active-source: the two situations this
        // is pressed in are "the TV is on another input" and "the TV is
        // off", and tv-on is <Image View On> followed by <Active Source>,
        // which covers both.
        case WM_HOTKEY:
            if (wParam == kHotkeyId) g_commands.Post("CMD tv-on");
            return 0;

        // Broadcasts, and receiving them at all is why this window is an
        // ordinary hidden top-level window rather than a message-only one. A
        // message-only window is invisible to broadcast messages, so an app
        // built on one never learns that the taskbar restarted or that the
        // display changed - it just quietly stops being right.
        case WM_DISPLAYCHANGE:
            // Possibly a different television, so what this one could report
            // is no longer known. See g_volumeSupport.
            ForgetVolumeSupport();
            RefreshTrayIcon(false);
            return 0;
        case WM_SETTINGCHANGE:
            // Light and dark switching arrives here. The menu theme is cached
            // per process, so it is asked to read the new one.
            FollowSystemThemeInWindow(window);
            RefreshTrayIcon(false);
            return 0;

        case WM_DPICHANGED:
            RefreshTrayIcon(false);
            return 0;

        // The same after sleep, which the television may not have spent
        // connected to this PC.
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC) ForgetVolumeSupport();
            // Never asleep with the pointer hidden: what wakes the PC may not
            // be the mouse.
            if (wParam == PBT_APMSUSPEND) g_pointerHider.Show();
            // Plugged in or unplugged, or awake again: the timeouts in force
            // may have changed, so the screen-off moment is worked out afresh.
            if (wParam == PBT_APMPOWERSTATUSCHANGE ||
                wParam == PBT_APMRESUMEAUTOMATIC) {
                CheckScreenOff();
            }
            // The screen coming on, which is either a person or the television
            // dropping its signal. Watched for a few seconds so the service can
            // be told which it was.
            if (wParam == PBT_POWERSETTINGCHANGE) {
                const auto* setting =
                    reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (setting &&
                    IsEqualGUID(setting->PowerSetting, kConsoleDisplayState) &&
                    setting->DataLength >= sizeof(DWORD)) {
                    const DWORD state =
                        *reinterpret_cast<const DWORD*>(setting->Data);
                    if (state == 1) WatchForInput();
                    if (state == 0) StopWatchingForInput();
                }
            }
            return TRUE;

        // Locked, switched away from, or connected to remotely: the pointer is
        // shown before whatever comes next needs it. Locking also brings in
        // the lock screen's own screen timeout.
        case WM_WTSSESSION_CHANGE:
            g_pointerHider.Show();
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK) {
                g_sessionLocked = wParam == WTS_SESSION_LOCK;
                CheckScreenOff();
            }
            return 0;

        case WM_ENDSESSION:
            if (wParam) g_pointerHider.Show();
            return 0;

        case WM_DESTROY:
            // Ended now rather than left to lapse, so choosing Exit lets the TV
            // go straight away instead of up to five minutes later. Sent
            // directly, because the command worker is about to stop.
            if (g_keepAwake) SendRequest("CMD keep-awake-off");
            g_pointerHider.Show();
            KillTimer(window, kPointerTimer);
            WTSUnRegisterSessionNotification(window);
            UnregisterHotkeyIfAny();
            Shell_NotifyIconW(NIM_DELETE, &g_icon);
            PostQuitMessage(0);
            return 0;
        default:
            if (message == g_taskbarCreated && g_taskbarCreated) {
                // The shell has forgotten every icon it held, this one
                // included, so it is added rather than modified.
                RefreshTrayIcon(true);
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // A command runs and exits before anything a tray needs is set up.
    const int commandResult = RunCommandLine();
    if (commandResult >= 0) return commandResult;

    // Per-monitor DPI, so text and buttons stay sharp on scaled displays and
    // when the panel opens on a monitor with a different scale factor. Must be
    // set before any window exists.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // For the audio endpoint API that reads Windows' own volume. Apartment
    // threaded, and left up for the life of the process: the shortcut writer
    // initialises COM again around itself, which is harmless because the count
    // is per thread and it balances its own call.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // A second copy would put a duplicate icon in the tray.
    //
    // It used to exit without a word, which reads as the shortcut doing
    // nothing at all: the icon is already there, but the notification area may
    // have tidied it out of sight, so nothing on screen answers the click. So
    // the second copy says where to look before it goes.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\CecTraySingleInstance");
    if (!single) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"HDMI-CEC Display Control is already running.\n\n"
                    L"Look for the blue power icon in the taskbar, beside the "
                    L"clock. If it is not there, click the arrow next to the "
                    L"clock to show hidden icons.\n\n"
                    L"Left-click the icon for volume, and right-click it for "
                    L"everything else.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Before anything could hide it again: a pointer the last run left hidden
    // is put back, and a crash from here on puts it back on the way out.
    g_pointerHider.RestoreLeftover(kAppKey);
    SetUnhandledExceptionFilter(ShowPointerOnCrash);

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon =
        LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);

    // A hidden top-level window, deliberately not a message-only one.
    //
    // HWND_MESSAGE looks like the right choice for a window that exists only to
    // receive the tray icon's notifications, and it was - until it was not. A
    // message-only window is excluded from broadcasts, so it never hears
    // TaskbarCreated when Explorer restarts, and the icon is then gone for the
    // rest of the session; and it never hears about a display change, so
    // anything sized from the current scale silently stays at the old one.
    //
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt-Tab, and ShowWindow
    // is never called, so it is every bit as invisible as before.
    g_instance = instance;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName,
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                               nullptr);
    if (!g_window) return 1;

    g_icon.cbSize = sizeof(g_icon);
    g_icon.hWnd = g_window;
    g_icon.uID = kTrayId;
    g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_icon.uCallbackMessage = kTrayMessage;
    FirstRunSetup();
    g_commands.Start();
    StartAnswerRefresher();
    LoadHotkey();
    LoadRemoteBinds();
    LoadRemoteSeen();

    // Known before the keyboard hook is decided, so volume keys never go to
    // the TV for a moment while headphones are the output. The TV is not named
    // yet, so this is judged by the kind of device until STATUS arrives.
    g_outputElsewhere = OutputIsElsewhere();
    StartOutputWatch();

    // Idle from the moment it starts, and the only thread allowed to hold a
    // keyboard hook. See UpdateKeyboardVolume.
    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThread = CreateThread(nullptr, 0, KeyboardHookThread, nullptr, 0,
                                &g_hookThreadId);
    UpdateKeyboardVolume();
    ApplyPointerOptions();
    WTSRegisterSessionNotification(g_window, NOTIFY_FOR_THIS_SESSION);

    g_remoteKeyStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_remoteKeyThread =
        CreateThread(nullptr, 0, ServiceEventThread, nullptr, 0, nullptr);
    g_volumePanel.Create(instance, SendVolumeCommand, VolumeGoesToPc);
    // One status read at startup so the volume buttons behave correctly before
    // the menu has ever been opened. On the worker, because it is a pipe round
    // trip and the UI thread must not wait on one.
    g_commands.Post("STATUS-REFRESH");

    // Through RefreshTrayIcon like every later change, so the first icon is
    // drawn the same way and does not change size when a badge first appears.
    RefreshTrayIcon(true);

    // A quiet watch on the service, so the mark appears without anyone having
    // to press something first and find out the hard way. One question to the
    // Service Control Manager every half minute costs far less than the work
    // this app already does while idle.
    // Menus are drawn by Windows, and it draws them light unless both the
    // process and the window have said they follow the system.
    FollowSystemThemeInWindow(g_window);
    SetServiceDown(!ServiceIsRunning());
    SetTimer(g_window, kServiceWatchTimer, kServiceWatchMs, nullptr);
    CheckScreenOff();
    // Delivers the current state immediately, which is harmless: the screen is
    // on, so at worst one report of input nobody asked about.
    RegisterPowerSettingNotification(g_window, &kConsoleDisplayState,
                                     DEVICE_NOTIFY_WINDOW_HANDLE);

    // Registered after the icon exists, because the icon is what carries the
    // complaint. No dialog at login - that would be rude for something nobody
    // asked about yet - but silence was worse: a shortcut Windows refused looked
    // identical to a television that would not switch, and cost a round of
    // testing to tell apart.
    if (!ApplyHotkey(false)) {
        ShowBalloon(L"Shortcut unavailable",
                    DescribeHotkey(g_hotkeyMods, g_hotkeyVk) +
                        L" is already taken by another program. Pick another "
                        L"under Settings.",
                    NIIF_WARNING);
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Escape closes the guide window. Handled here rather than in its window
        // procedure because the edit control has the focus and swallows the key
        // before the frame ever sees it, and subclassing the control for one
        // keystroke is more machinery than the job needs.
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
            g_guideWindow &&
            (message.hwnd == g_guideWindow || IsChild(g_guideWindow, message.hwnd))) {
            DestroyWindow(g_guideWindow);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_remoteKeyThread) {
        SetEvent(g_remoteKeyStop);
        WaitForSingleObject(g_remoteKeyThread, 3000);
        CloseHandle(g_remoteKeyThread);
    }
    StopAnswerRefresher();
    g_commands.Stop();
    StopOutputWatch();
    CoUninitialize();
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}
