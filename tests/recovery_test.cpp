// Small decisions that keep the service honest after something went wrong
// underneath it: which frame counts as the television's power answer, when a
// standby verify's answer is too late to believe, when the service goes
// looking for its port again after a display arrives, what a poll that the
// adapter never reported on means, which devices are asked their names, and
// when a remote button is a new press rather than a held one. Copied verbatim;
// GetTickCount, Attach, SendFrame, AskName and the display state are stubs.
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

typedef unsigned long DWORD;
typedef long LONG;

constexpr uint8_t kLogicalAddressTv = 0;
constexpr DWORD kAttachAfterArrivalMs = 60000;
constexpr DWORD kAttachAfterArrivalEveryMs = 5000;
constexpr DWORD kStandbyWaitOverrunMs = 5000;

DWORD g_now = 0;
DWORD GetTickCount() { return g_now; }
int g_logs = 0;
void Log(const char*, ...) { ++g_logs; }

// ------------------------------------------------- verbatim power answer
bool IsTvPowerReport(const std::vector<uint8_t>& msg) {
    return msg.size() == 3 && msg[1] == 0x90 &&
           static_cast<uint8_t>(msg[0] >> 4) == kLogicalAddressTv;
}

// ------------------------------------------------- verbatim held-up wait
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

// ------------------------------------------------- the search, verbatim
bool looking_ = false;
bool g_powerDown = false;
bool g_displayOff = false;
bool g_displayArrived = false;
bool PowerDownStarted() { return g_powerDown; }
bool DisplayStillOff() { return g_displayOff; }
std::string lastError_ = "no GPU backend reported a CEC-capable port";
DWORD attachUntilTick_ = 0;
DWORD nextAttachTick_ = 0;
bool g_portAnswers = false;
bool g_attached = false;   // the pump calls this only with no device
int g_attempts = 0;
bool Attach() {
    ++g_attempts;
    if (!g_portAnswers) return false;
    g_attached = true;
    attachUntilTick_ = 0;   // whoever attached, the search is over
    return true;
}
bool Exchange(bool& flag) { const bool was = flag; flag = false; return was; }

void AttachIfDisplayReturned() {
    if (looking_ || PowerDownStarted() || DisplayStillOff()) return;
    const DWORD now = GetTickCount();
    if (Exchange(g_displayArrived)) {
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
    if (Attach()) g_displayArrived = true;
}

// ------------------------------------------------- the poll, verbatim
enum class PollAnswer : uint8_t { Present, Absent, Unknown };
struct TxResult { bool accepted = false; bool sent = false; bool nacked = false;
                  bool lineError = false; };
TxResult g_tx;   // what the adapter reports for the next frame
bool SendFrame(TxResult* tx) {
    *tx = g_tx;
    return tx->sent && !tx->nacked && !tx->lineError;
}
PollAnswer Poll() {
    TxResult tx;
    if (SendFrame(&tx)) {
        return PollAnswer::Present;
    }
    return tx.nacked ? PollAnswer::Absent : PollAnswer::Unknown;
}
bool audioSystem_ = false;
void DetectAudioSystem() {
    switch (Poll()) {
        case PollAnswer::Present: audioSystem_ = true;  break;
        case PollAnswer::Absent:  audioSystem_ = false; break;
        case PollAnswer::Unknown: break;
    }
}

// ------------------------------------------------- Windows Update, verbatim
bool IsWindowsUpdateShutdown(const std::wstring& sid, const std::wstring& code) {
    if (sid != L"S-1-5-18" || code.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long reason = std::wcstoul(code.c_str(), &end, 16);
    if (!end || *end != L'\0') return false;
    return (reason & 0x00FF0000UL) == 0x00020000UL;
}

// ------------------------------------------------- peer names, verbatim
// What a hello does to the name it finds, and which answer to <Give OSD Name>
// replaces a name cut short.
struct Peer {
    uint8_t address = 0;
    uint16_t where = 0xFFFF;
    std::string name;
    bool ours = false;
    bool present = false;
    uint8_t nameAsks = 0;
    bool nameUnsure = false;
};
void HelloName(Peer& one, const std::string& name) {
    if (one.name.size() < name.size() ||
        one.name.compare(0, name.size(), name) != 0) {
        one.name = name;
        one.nameAsks = 0;
    }
}
bool TakeFullName(Peer& one, const std::string& full) {
    if (full.size() > one.name.size() &&
        full.compare(0, one.name.size(), one.name) == 0) {
        one.name = full;
        return true;
    }
    return false;
}

// ------------------------------------------------- AskForNames, verbatim
// with the device's AskName answered from a script: what each address would
// say, and who was asked.
std::vector<Peer> peers_;
std::string g_names[16];
std::vector<uint8_t> g_asked;
struct Device {
    std::string AskName(uint8_t address, int) {
        g_asked.push_back(address);
        return g_names[address];
    }
} g_device;
Device* device_ = &g_device;
std::string FormatPhysicalAddress(uint16_t) { return "x"; }
namespace cec { using ::FormatPhysicalAddress; }

void AskForNames() {
    for (Peer& one : peers_) {
        if (!one.present || one.nameAsks >= 3) continue;
        const bool cutShort = one.ours && one.name.size() == 7;
        const bool unnamed = !one.ours &&
                             (one.name.empty() || one.nameUnsure) &&
                             one.where != 0xFFFF && one.where != 0;
        if (!cutShort && !unnamed) continue;
        ++one.nameAsks;
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

// ------------------------------------------------- remote repeats, verbatim
uint8_t g_heldUiCommand = 0;
bool g_holdingRemoteKey = false;
DWORD g_heldUiTick = 0;
constexpr DWORD kRemoteRepeatGapMs = 700;
// The decision at the top of OnUserControl, returning whether a press is the
// held button's repeat.
bool PressIsRepeat(uint8_t uiCommand, bool pressed) {
    if (!pressed) {
        g_holdingRemoteKey = false;
        return false;
    }
    const DWORD now = GetTickCount();
    const bool repeat = g_holdingRemoteKey && uiCommand == g_heldUiCommand &&
                        now - g_heldUiTick < kRemoteRepeatGapMs;
    g_heldUiCommand = uiCommand;
    g_heldUiTick = now;
    g_holdingRemoteKey = true;
    return repeat;
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}
void Reset(DWORD start) {
    g_now = start;
    looking_ = g_powerDown = g_displayOff = g_displayArrived = false;
    g_portAnswers = g_attached = false;
    attachUntilTick_ = nextAttachTick_ = 0;
    g_attempts = g_logs = 0;
}
// The pump, once a second, for the given number of seconds.
void Run(int seconds) {
    for (int i = 0; i < seconds; ++i) {
        if (!g_attached) AttachIfDisplayReturned();
        g_now += 1000;
    }
}

int main() {
    // 1. Which frames are the television's power answer.
    Expect(IsTvPowerReport({0x04, 0x90, 0x00}), "the TV saying it is on");
    Expect(IsTvPowerReport({0x04, 0x90, 0x01}), "the TV saying it is off");
    Expect(!IsTvPowerReport({0x84, 0x90, 0x00, 0x45, 0x43, 0x01}),
           "the field frame: a PC's hello behind a stale 0x90 is not it");
    Expect(!IsTvPowerReport({0x84, 0x90, 0x00}),
           "even at the right length, another device is not the TV");
    Expect(!IsTvPowerReport({0x04, 0x90, 0x00, 0x00}),
           "a long frame from the TV is not believed either");
    Expect(!IsTvPowerReport({0x04, 0x90}), "nor a short one");

    // 2. When a verify's answer is too late to act on.
    g_now = 1000;
    Expect(!WaitWasHeldUp(1000 - 10000, 10000), "a wait that ran its limit");
    g_now = 1000 + 13500;
    Expect(!WaitWasHeldUp(1000, 10000),
           "one slow frame past the limit is still a running wait");
    g_now = 1000 + 122000;
    Expect(WaitWasHeldUp(1000, 10000),
           "the field case: 122 seconds against 10 is not believed");
    g_now = 0xFFFFF000;
    const DWORD before = g_now;
    g_now += 12000;   // wraps past zero
    Expect(!WaitWasHeldUp(before, 10000), "across the tick count wrapping");

    // 3. The field case: the cable comes back with no device attached.
    Reset(5000);
    g_displayArrived = true;
    Run(3);
    Expect(g_attempts == 1, "the arrival is acted on at once, then spaced");
    g_portAnswers = true;
    Run(5);
    Expect(g_attempts == 2 && attachUntilTick_ == 0,
           "the next try five seconds later attaches and ends the search");
    Expect(g_displayArrived, "and hands the arrival on to the address recheck");

    // 4. A plain monitor: a minute of tries, one line, then nothing.
    Reset(5000);
    g_displayArrived = true;
    Run(70);
    Expect(g_attempts == 12, "a try every five seconds through the minute");
    Expect(g_logs == 1 && attachUntilTick_ == 0, "one line, and it stops");
    Run(120);
    Expect(g_attempts == 12, "and no more without another arrival");

    // 5. The arrival is kept while the screen is off, not thrown away.
    Reset(5000);
    g_displayArrived = true;
    g_displayOff = true;
    Run(30);
    Expect(g_attempts == 0 && g_displayArrived, "nothing while the screen is off");
    g_displayOff = false;
    g_portAnswers = true;
    Run(1);
    Expect(g_attempts == 1 && attachUntilTick_ == 0, "and it attaches when it returns");

    // 6. The startup loop owns the first attach.
    Reset(5000);
    g_displayArrived = true;
    looking_ = true;
    Run(10);
    Expect(g_attempts == 0 && g_displayArrived, "nothing while startup is looking");

    // 7. Somebody else attaches during the search: it is closed, not left to
    //    expire later and log a failure that never happened.
    Reset(5000);
    g_displayArrived = true;
    Run(1);
    g_portAnswers = true;
    Attach();   // a command from the tray, say
    Expect(attachUntilTick_ == 0, "another path's attach closes the search");

    // 8. A window opened just before the tick count wraps still runs a minute.
    Reset(0xFFFFFFFF - 20000);
    g_displayArrived = true;
    Run(70);
    Expect(g_attempts == 12 && g_logs == 1, "across the tick count wrapping");

    // 9. A poll's three outcomes, and the soundbar probe that read the third
    //    as a soundbar on a Fire TV that had none.
    g_tx = {true, true, false, false};
    Expect(Poll() == PollAnswer::Present, "an acknowledged poll is a device");
    g_tx = {true, false, true, false};
    Expect(Poll() == PollAnswer::Absent, "a refused poll is nobody");
    g_tx = {true, false, false, false};
    Expect(Poll() == PollAnswer::Unknown,
           "no transmit status is neither - the field case");
    g_tx = {true, true, false, true};
    Expect(Poll() == PollAnswer::Unknown, "nor is a line error");
    audioSystem_ = false;
    g_tx = {true, false, false, false};
    DetectAudioSystem();
    Expect(!audioSystem_, "a silent adapter does not invent a soundbar");
    g_tx = {true, true, false, false};
    DetectAudioSystem();
    g_tx = {false, false, false, false};
    DetectAudioSystem();
    Expect(audioSystem_, "nor forget a real one");
    g_tx = {true, false, true, false};
    DetectAudioSystem();
    Expect(!audioSystem_, "a refusal does");

    // 10. Which restarts were Windows Update's own, from the desktop's real
    //     events: only those leave the TV off at the next start.
    const wchar_t* user = L"S-1-5-21-1731598236-1851814663-2788096116-1001";
    Expect(IsWindowsUpdateShutdown(L"S-1-5-18", L"0x80020003"),
           "TrustedInstaller as SYSTEM, an upgrade: Windows Update");
    Expect(IsWindowsUpdateShutdown(L"S-1-5-18", L"0x80020010"),
           "SYSTEM, a service pack: Windows Update");
    Expect(!IsWindowsUpdateShutdown(user, L"0x80020010"),
           "a person clicking Restart now in the update prompt is a person");
    Expect(!IsWindowsUpdateShutdown(L"S-1-5-18", L"0x500ff"),
           "the sign-in screen's power button is a person too");
    Expect(!IsWindowsUpdateShutdown(user, L"0x0"), "the Start menu is a person");
    Expect(!IsWindowsUpdateShutdown(L"", L"0x80020003"), "no account, no verdict");
    Expect(!IsWindowsUpdateShutdown(L"S-1-5-18", L"0x8002000z"),
           "a reason that is not a number, no verdict");
    Expect(!IsWindowsUpdateShutdown(L"S-1-5-18", L""), "no reason, no verdict");

    // 11. Another PC's name: seven characters from the hello, the full name
    //     from asking, and the hello no longer cutting it back.
    Peer pc;
    HelloName(pc, "DESKTOP");
    Expect(pc.name == "DESKTOP", "the hello's seven characters to begin with");
    Expect(TakeFullName(pc, "DESKTOP-ABC123"), "the field case: the full name");
    HelloName(pc, "DESKTOP");
    Expect(pc.name == "DESKTOP-ABC123",
           "and the next hello, ten seconds later, keeps it");
    Peer torn;
    HelloName(torn, "STEAMDE");
    Expect(!TakeFullName(torn, "?") && torn.name == "STEAMDE",
           "a torn one-character name is refused (the old '?' bug)");
    Expect(!TakeFullName(torn, "DELL-5430") && torn.name == "STEAMDE",
           "another PC's name, torn into this answer, is refused");
    Expect(!TakeFullName(torn, "STEAMDE"), "the same seven again is nothing new");
    Peer renamed;
    HelloName(renamed, "DESKTOP");
    TakeFullName(renamed, "DESKTOP-ABC123");
    renamed.nameAsks = 1;
    HelloName(renamed, "OFFICE-");
    Expect(renamed.name == "OFFICE-" && renamed.nameAsks == 0,
           "a renamed PC is taken as the hello says, and asked again");

    // 12. Which devices are asked their names, one a round. The field case:
    //     a Google TV plugged in while the desktop ran was listed as
    //     "Playback device" until a restart, because only the bus map asked.
    peers_.clear();
    Peer gtv; gtv.address = 11; gtv.where = 0x2000; gtv.present = true;
    Peer bar; bar.address = 5; bar.where = 0xFFFF; bar.present = true;   // on ARC
    Peer dell; dell.address = 8; dell.where = 0x3000; dell.present = true;
    dell.ours = true; dell.name = "DELL-54";
    peers_ = {bar, gtv, dell};
    g_names[11] = "Chromecast";
    g_names[8] = "DELL-5430";
    g_names[5] = "Polk";
    g_asked.clear();
    AskForNames();
    Expect(g_asked.size() == 1 && g_asked[0] == 11 && peers_[1].name == "Chromecast",
           "the unnamed device on an input is asked, and named");
    Expect(peers_[0].name.empty(), "a soundbar with no input is not asked");
    AskForNames();
    Expect(g_asked.size() == 2 && g_asked[1] == 8 && peers_[2].name == "DELL-5430",
           "then the PC whose hello cut its name short, in the next round");
    AskForNames();
    Expect(g_asked.size() == 2, "and nothing more once nothing is owed");
    peers_.clear();
    Peer mute; mute.address = 4; mute.where = 0x1000; mute.present = true;
    peers_ = {mute};
    g_names[4] = "";
    for (int i = 0; i < 5; ++i) AskForNames();
    Expect(peers_[0].nameAsks == 3 && g_asked.size() == 5,
           "a device that never answers is asked three times, then left");
    peers_[0].nameAsks = 0;
    peers_[0].present = false;
    AskForNames();
    Expect(g_asked.size() == 5, "a device that is asleep is not asked");
    peers_[0].present = true;
    g_names[4] = "?";
    AskForNames();
    Expect(peers_[0].name.empty(), "a one-character answer is not a name");
    peers_.clear();
    Peer moved; moved.address = 4; moved.where = 0x3000; moved.present = true;
    moved.name = "Fire TV"; moved.nameUnsure = true;
    peers_ = {moved};
    g_names[4] = "Roku";
    AskForNames();
    Expect(peers_[0].name == "Roku" && !peers_[0].nameUnsure,
           "a device on a new input whose name was unsure is asked, and renamed");

    // 13. Remote buttons: a held one repeats, a pressed-again one does not.
    //     The field case, the LG on 2026-09-30: Select at 20:17:44 and no
    //     release, then Select again four seconds later. It was taken for the
    //     same button still held and dropped, since Select does not repeat.
    g_holdingRemoteKey = false;
    g_now = 1000000;
    Expect(!PressIsRepeat(0x00, true), "Select, pressed");
    g_now += 4000;
    Expect(!PressIsRepeat(0x00, true),
           "the field case: Select four seconds later, no release between, is "
           "a press");
    PressIsRepeat(0, false);
    g_now += 300;
    Expect(!PressIsRepeat(0x01, true), "Up, pressed");
    g_now += 450;
    Expect(PressIsRepeat(0x01, true), "held: the TV's repeat 450ms later");
    g_now += 500;
    Expect(PressIsRepeat(0x01, true), "and another 500ms after that");
    g_now += 100;
    Expect(!PressIsRepeat(0x02, true), "a different button is a press");
    PressIsRepeat(0, false);
    g_now += 200;
    Expect(!PressIsRepeat(0x02, true), "the same button after a release is a press");
    g_now = 0xFFFFFF00;
    Expect(!PressIsRepeat(0x0D, true), "Exit, just before the tick count wraps");
    g_now += 0x200;
    Expect(PressIsRepeat(0x0D, true), "held across the wrap: still a repeat");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
