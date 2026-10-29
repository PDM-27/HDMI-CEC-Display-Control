// CecDevice::WaitForStandby against a scripted television, which is hard rule 1
// as code: standby is believed whenever it is reported, and "on" only from a
// question asked once the set has had time to act. Also the hand-over between
// a sleep and a screen-off that begin together. Copied verbatim; the clock,
// the adapter, the television and the CEC lock are stubs, and time passes only
// when the code sleeps, sends or waits for the lock.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

typedef unsigned long DWORD;
typedef long LONG;

enum class PowerStatus : uint8_t {
    On = 0x00,
    Standby = 0x01,
    TransitioningToOn = 0x02,
    TransitioningToStandby = 0x03,
    Unknown = 0xFF,
};

constexpr uint8_t kLogicalAddressTv = 0;
constexpr uint8_t logicalAddress_ = 4;
constexpr DWORD kStandbyTransitionMs = 2500;

uint8_t Header(uint8_t source, uint8_t target) {
    return static_cast<uint8_t>((source << 4) | (target & 0x0F));
}
bool IsTvPowerReport(const std::vector<uint8_t>& msg) {
    return msg.size() == 3 && msg[1] == 0x90 &&
           static_cast<uint8_t>(msg[0] >> 4) == kLogicalAddressTv;
}

// ------------------------------------------------- the scripted world
DWORD g_now = 0;
DWORD g_start = 0;
DWORD GetTickCount() { return g_now; }
void Sleep(DWORD ms) { g_now += ms; }

// What the set says to a question asked this long after the command, or -1
// for no answer at all.
int (*g_tv)(DWORD askedAfter) = nullptr;
struct Reply { DWORD at; std::vector<uint8_t> frame; };
std::deque<Reply> g_inbox;
int g_asks = 0;

bool SendFrame(const std::vector<uint8_t>& frame) {
    g_now += 60;   // a frame and its transmit status
    if (frame.size() == 2 && frame[1] == 0x8F) {
        ++g_asks;
        const int state = g_tv(g_now - g_start);
        if (state >= 0) {
            g_inbox.push_back({g_now + 100, {0x04, 0x90, static_cast<uint8_t>(state)}});
        }
    }
    return true;
}
bool ReceiveFrame(std::vector<uint8_t>& msg) {
    if (g_inbox.empty() || g_inbox.front().at > g_now) return false;
    msg = g_inbox.front().frame;
    g_inbox.pop_front();
    return true;
}

// ------------------------------------------------- verbatim
PowerStatus WaitForStandby(int timeoutMs) {
    const DWORD started = GetTickCount();
    const DWORD deadline = started + static_cast<DWORD>(timeoutMs);
    PowerStatus last = PowerStatus::Unknown;
    bool asked = false;
    DWORD lastAsk = 0;
    while (true) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (!IsTvPowerReport(msg)) continue;
            const auto reported = static_cast<PowerStatus>(msg[2]);
            if (reported == PowerStatus::Standby ||
                reported == PowerStatus::TransitioningToStandby) {
                return reported;
            }
            if (asked && lastAsk - started >= kStandbyTransitionMs) {
                last = reported;
            }
        }
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) return last;
        if (!asked || GetTickCount() - lastAsk >= 2000) {
            lastAsk = GetTickCount();
            asked = true;
            SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x8F});
        }
        Sleep(150);
    }
}

// ------------------------------------------------- the sets
int Tcl(DWORD t) { return t < 3000 ? 0x00 : 0x01; }       // on, then off at 3s
int Ignores(DWORD) { return 0x00; }                        // refuses silently
int QuietAfterFirst(DWORD t) { return t < 500 ? 0x00 : -1; }
int Quick(DWORD t) { return t < 1000 ? 0x00 : 0x03; }      // transitioning at 1s
int Silent(DWORD) { return -1; }

// ------------------------------------------------- TvNeedsStandby, verbatim
// The decision only: the lock is assumed held and the device present, and the
// question to the TV returns the scripted answer and is counted.
constexpr DWORD kOwnStandbyTrustMs = 10000;
DWORD lastStandbySentTick_ = 0;
PowerStatus g_answer = PowerStatus::Unknown;
int g_questions = 0;
PowerStatus QueryPowerStatus() { ++g_questions; return g_answer; }
bool TvNeedsStandby(bool believedOff) {
    if (believedOff && lastStandbySentTick_ &&
        GetTickCount() - lastStandbySentTick_ < kOwnStandbyTrustMs) {
        return false;
    }
    const PowerStatus status = QueryPowerStatus();
    if (status == PowerStatus::Standby ||
        status == PowerStatus::TransitioningToStandby) {
        return false;
    }
    if (status == PowerStatus::On ||
        status == PowerStatus::TransitioningToOn) {
        return true;
    }
    return !believedOff;
}
bool Need(bool believedOff, DWORD ownStandbyAgo, PowerStatus answer) {
    g_now = 100000;
    lastStandbySentTick_ = ownStandbyAgo ? g_now - ownStandbyAgo : 0;
    g_answer = answer;
    g_questions = 0;
    return TvNeedsStandby(believedOff);
}

// ------------------------------------------- the power-down's lock, verbatim
// The CEC lock as the display-off path holds it: taken until heldUntil, and
// committed to sending from committedFrom on (0 for never). Waiting for it
// moves the clock.
DWORD g_heldUntil = 0;
DWORD g_committedFrom = 0;
struct FakeLock {
    bool owned = false;
    template <class Duration>
    bool try_lock_for(Duration wait) {
        if (static_cast<LONG>(g_now - g_heldUntil) >= 0) return owned = true;
        g_now += static_cast<DWORD>(wait.count());
        return false;
    }
};
struct Committed {
    bool load() const {
        return g_committedFrom && static_cast<LONG>(g_now - g_committedFrom) >= 0;
    }
} displayCommitted_;
bool g_displayOwnsStandby = false;
bool DisplayOwnsStandby() { return g_displayOwnsStandby; }
constexpr DWORD kPowerDownLockWaitMs = 5000;
namespace std { namespace chrono {
struct milliseconds { long long n; explicit milliseconds(long long v) : n(v) {}
                      long long count() const { return n; } };
} }

bool LockForPowerDown(FakeLock& guard) {
    const DWORD start = GetTickCount();
    for (;;) {
        if (guard.try_lock_for(std::chrono::milliseconds(100))) return true;
        if (displayCommitted_.load()) return false;
        if (GetTickCount() - start >= kPowerDownLockWaitMs) return false;
    }
}
bool StandbyAlreadySent() {
    FakeLock guard;
    if (!LockForPowerDown(guard)) return true;
    return DisplayOwnsStandby();
}
// The sleep's whole decision: whether <Standby> goes out from it. The old
// StandbyAlreadySent took a lock it could not get within a second to mean the
// display-off path was sending; TvNeedsStandby then could not get the lock
// either and sent nothing.
bool SleepSends(bool oldRule) {
    bool believedOff;
    if (oldRule) {
        believedOff = static_cast<LONG>(g_now + 1000 - g_heldUntil) < 0
                          ? true
                          : DisplayOwnsStandby();
        if (believedOff) return false;   // and its 300ms lock fails too
    } else {
        believedOff = StandbyAlreadySent();
    }
    // TvNeedsStandby's own lock, 300ms, as the service takes it.
    FakeLock guard;
    if (!guard.try_lock_for(std::chrono::milliseconds(300))) return !believedOff;
    return TvNeedsStandby(believedOff);
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}
PowerStatus Run(int (*tv)(DWORD), int timeoutMs, DWORD start = 1000) {
    g_now = g_start = start;
    g_tv = tv;
    g_inbox.clear();
    g_asks = 0;
    return WaitForStandby(timeoutMs);
}

int main() {
    // The field case: the suspend path's 2.5 seconds against the TCL, which
    // used to read "on" from the question at two seconds and resend.
    Expect(Run(Tcl, 2500) == PowerStatus::Unknown,
           "suspend wait on the TCL: unknown, so nothing is resent");
    Expect(Run(Ignores, 2500) == PowerStatus::Unknown,
           "suspend wait never says on, whatever the set does");

    // The paths with time to wait still see a set that really ignored it.
    Expect(Run(Ignores, 10000) == PowerStatus::On,
           "ten seconds: a set still on at four seconds is believed");
    Expect(Run(Ignores, 5000) == PowerStatus::On,
           "five seconds (tv-standby): also long enough to believe");
    Expect(Run(Tcl, 10000) == PowerStatus::Standby,
           "ten seconds on the TCL ends at standby, not before");

    // A set that answered on at once and then went quiet while powering down.
    Expect(Run(QuietAfterFirst, 10000) == PowerStatus::Unknown,
           "an early on followed by silence is not a reason to resend");
    Expect(Run(Silent, 10000) == PowerStatus::Unknown, "silence is unknown");

    // Standby is believed whenever it comes, however early.
    Expect(Run(Quick, 2500) == PowerStatus::TransitioningToStandby,
           "transitioning at one second ends the wait at once");
    Expect(g_now - g_start < 2500, "and does not wait out the rest");

    // The deadline across the tick count wrapping.
    Expect(Run(Ignores, 10000, 0xFFFFFFFF - 3000) == PowerStatus::On,
           "a wait that wraps past zero still runs its full length");
    Expect(g_asks >= 5, "and keeps asking every two seconds");

    // Whether a sleep or shutdown sends <Standby> at all.
    Expect(!Need(true, 3000, PowerStatus::On) && g_questions == 0,
           "own <Standby> 3s ago: trusted, not even asked (a set says on "
           "while it powers down)");
    Expect(!Need(true, 0, PowerStatus::Standby),
           "after an update restart, the set says off: nothing sent");
    Expect(Need(true, 0, PowerStatus::On),
           "believed off but the set says on (its own remote): sent");
    Expect(Need(true, 12000, PowerStatus::On),
           "own <Standby> 12s ago and the set says on: sent");
    Expect(!Need(true, 0, PowerStatus::Unknown),
           "believed off and the set will not say: as before, nothing sent");
    Expect(!Need(false, 0, PowerStatus::Standby),
           "believed on but the set says off (a power-on that never landed): "
           "nothing sent, so a TCL is not toggled on");
    Expect(Need(false, 0, PowerStatus::On), "believed on and on: sent");
    Expect(Need(false, 0, PowerStatus::Unknown),
           "believed on and the set will not say: as before, sent");
    Expect(!Need(false, 0, PowerStatus::TransitioningToStandby),
           "already on its way down: nothing sent");

    // The sleep pressed by hand: the screen-off and the suspend together. The
    // field case of 2026-10-19 19:54:54: the display-off path took the lock,
    // asked what was on screen and announced, and at two seconds saw the sleep
    // and stood aside. The TV was on.
    g_answer = PowerStatus::On;
    lastStandbySentTick_ = 0;
    g_displayOwnsStandby = false;
    g_now = 50000; g_heldUntil = g_now + 2000; g_committedFrom = 0;
    Expect(!SleepSends(true),
           "the field case under the old rule: nothing sent, the TV left on");
    g_now = 50000; g_heldUntil = g_now + 2000; g_committedFrom = 0;
    Expect(SleepSends(false) && g_now - 50000 >= 2000 && g_now - 50000 < 2200,
           "now: the sleep waits the two seconds out and sends <Standby>");

    // The other order: the display-off path had passed its last look before the
    // sleep began, so it is sending, and holds the lock through its verify.
    g_now = 50000; g_heldUntil = g_now + 10000; g_committedFrom = 50000;
    Expect(!SleepSends(false) && g_now - 50000 <= 400,
           "the display-off path already sending: the sleep sends nothing, at "
           "once (no second <Standby>)");

    // It commits while the sleep waits.
    g_now = 50000; g_heldUntil = g_now + 10000; g_committedFrom = 51500;
    Expect(!SleepSends(false) && g_now - 50000 < 2000,
           "it commits while the sleep waits: told so, and nothing doubled");

    // Something holding the lock that never decides.
    g_now = 50000; g_heldUntil = g_now + 60000; g_committedFrom = 0;
    Expect(!SleepSends(false) && g_now - 50000 >= 5000 && g_now - 50000 < 5500,
           "a hung holder: five seconds, then nothing sent rather than a toggle");

    // A free lock costs nothing.
    g_now = 50000; g_heldUntil = 0; g_committedFrom = 0;
    Expect(SleepSends(false) && g_now == 50000, "a free lock: decided at once");
    g_displayOwnsStandby = true;
    lastStandbySentTick_ = g_now - 2000;
    Expect(!SleepSends(false), "and a screen-off that already sent is trusted");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
