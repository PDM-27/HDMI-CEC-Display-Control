// ClaimPort plus the ListDevices dedup, against the field case that lost a
// device from the menu, NotePeer's confirmation of a new PC, and the rule that
// takes a silent "PC" for an ordinary device. All copied verbatim from
// CecService.cpp, with the device's SayHello recorded instead of sent.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Peer {
    uint8_t address = 0;
    uint16_t where = 0xFFFF;
    uint8_t type = 0xFF;
    std::string name;
    bool ours = false;
    bool present = false;
    unsigned long heardTick = 0;
    uint16_t contestPort = 0xFFFF;
    unsigned long contestTick = 0;
    uint16_t helloPort = 0xFFFF;
    unsigned long helloTick = 0;
    uint8_t nameAsks = 0;
    bool helloHeard = false;
    uint8_t silentRounds = 0;
    bool nameUnsure = false;
};

typedef unsigned long DWORD;
DWORD g_now = 1000;
DWORD GetTickCount() { return g_now; }
constexpr DWORD kPortContestMs = 5000;
constexpr DWORD kPeerConfirmMs = 5000;
typedef long LONG;

std::vector<Peer> peers_;
std::vector<std::string> log_;

std::string Fmt(uint16_t a) {
    char b[16];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF,
                  (a >> 4) & 0xF, a & 0xF);
    return b;
}
std::string DescribePeer(const Peer& one) {
    if (one.where != 0xFFFF) return Fmt(one.where);
    char t[40];
    std::snprintf(t, sizeof(t), "the device on CEC address %u", one.address);
    return t;
}

// ----------------------------------------------------- verbatim from the service
void ClaimPort(Peer& one, uint16_t where) {
    if (one.where == where) return;
    for (Peer& other : peers_) {
        if (&other == &one || other.where != where) continue;
        if (other.ours && !one.ours) {
            if (other.present) {
                log_.push_back(DescribePeer(one) + " also claims " + Fmt(where) +
                               ", held by a PC running this app; ignored");
                one.contestTick = 0;
                return;
            }
            const DWORD now = GetTickCount();
            if (one.contestPort != where || !one.contestTick) {
                one.contestPort = where;
                one.contestTick = now | 1;
                log_.push_back(DescribePeer(one) + " says it is on " + Fmt(where) +
                               ", believed if it says so again");
                return;
            }
            if (static_cast<long>(now - one.contestTick) <
                static_cast<long>(kPortContestMs)) {
                return;
            }
        }
        log_.push_back(DescribePeer(one) + " has taken " + Fmt(where) + " from " +
                       DescribePeer(other));
        other.where = 0xFFFF;
    }
    if (!one.ours && one.where != 0xFFFF && one.where != where) {
        one.nameUnsure = true;
        one.nameAsks = 0;
    }
    one.where = where;
    one.contestTick = 0;
}

Peer& DeviceAt(uint8_t address) {
    for (Peer& one : peers_) {
        if (one.address == address) return one;
    }
    Peer made;
    made.address = address;
    peers_.push_back(made);
    return peers_.back();
}

// The hellos this PC sends to confirm an address, in place of the device.
std::vector<uint8_t> hellosSent_;
const uint8_t kMyAddress = 8;

void NotePeer(uint8_t address, uint16_t where, const std::string& name,
              bool answered) {
    if (address == kMyAddress) return;
    if (address != 4 && address != 8 && address != 11 && address != 14) return;
    Peer& one = DeviceAt(address);
    if (!one.ours) {
        const DWORD now = GetTickCount();
        const bool asked = one.helloTick && one.helloPort == where &&
                           static_cast<LONG>(now - one.helloTick) <
                               static_cast<LONG>(kPeerConfirmMs);
        if (!answered || !asked) {
            if (!asked) {
                one.helloPort = where;
                one.helloTick = now | 1;
                hellosSent_.push_back(address);
            }
            return;
        }
    }
    one.helloTick = 0;
    one.ours = true;
    ClaimPort(one, where);
    if (one.name.size() < name.size() ||
        one.name.compare(0, name.size(), name) != 0) {
        one.name = name;
    }
    one.type = 4;
    one.ours = true;
    one.present = true;
    one.heardTick = GetTickCount();
    one.helloHeard = true;   // see StillSaysHello
}

// ----------------------------------------------- the silent "PC", verbatim
constexpr uint8_t kSilentRoundsBeforeStranger = 6;
bool StillSaysHello(Peer& one) {
    if (one.helloHeard) {
        one.helloHeard = false;
        one.silentRounds = 0;
        return true;
    }
    return ++one.silentRounds < kSilentRoundsBeforeStranger;
}
bool mapWanted_ = false;
void ForgetItWasAPc(Peer& one) {
    log_.push_back("no longer taken for a PC running this app");
    one.ours = false;
    one.silentRounds = 0;
    one.name.clear();
    one.nameUnsure = false;
    one.nameAsks = 0;
    one.type = 0xFF;
    mapWanted_ = true;
}
// One neighbour round's poll of one device, as GreetPeersNow does it, with the
// poll's answer given: 'P' acknowledged, 'A' refused, 'U' unreported.
void PollRound(Peer& one, char answer) {
    if (answer == 'U') return;
    const bool answered = answer == 'P';
    if (answered) {
        one.heardTick = GetTickCount();
        if (one.ours && !StillSaysHello(one)) ForgetItWasAPc(one);
    } else {
        one.silentRounds = 0;
    }
    one.present = answered;
}

// NoteDeviceFrame's <Report Physical Address> case, at its exact length.
void NoteReport(uint8_t src, uint16_t where) {
    Peer& one = DeviceAt(src);
    one.present = true;
    if (!one.ours) ClaimPort(one, where);
}

// The dedup that hid the device, also verbatim in shape.
std::vector<Peer> ListDevices(uint16_t selfWhere) {
    std::vector<Peer> shown;
    Peer self;
    self.where = selfWhere;
    self.name = "SELF";
    self.present = true;
    shown.push_back(self);
    for (const Peer& one : peers_) {
        if (one.where == 0xFFFF || one.where == 0) continue;
        if (one.where == self.where) continue;
        bool already = false;
        for (Peer& listed : shown) {
            if (listed.where != one.where) continue;
            already = true;
            if (static_cast<long>(one.heardTick - listed.heardTick) > 0) listed = one;
        }
        if (!already) shown.push_back(one);
    }
    return shown;
}


// TidyPeerAddresses, verbatim apart from Log and the device accessor.
uint8_t g_mine = 4;
uint16_t TidyPeerAddresses() {
    const uint8_t mine = g_mine;
    uint16_t avoid = 0;
    for (size_t i = peers_.size(); i > 0; --i) {
        const Peer& one = peers_[i - 1];
        if (one.address == mine) {
            log_.push_back("dropping the neighbour on our own address");
            peers_.erase(peers_.begin() + static_cast<long>(i - 1));
            continue;
        }
        if (one.ours) avoid |= 1u << one.address;
    }
    return avoid;
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}
bool Listed(const std::vector<Peer>& rows, const char* name) {
    for (const Peer& p : rows) if (p.name == name) return true;
    return false;
}

int main() {
    // The field case. The Apple TV is on HDMI 3; a Dell running this app then
    // claims 4.0.0.0 on the entry the Apple TV had.
    peers_.clear();
    Peer apple; apple.address = 11; apple.where = 0x3000; apple.name = "Apple TV";
    apple.present = true; apple.heardTick = 100;
    Peer dell; dell.address = 8; dell.where = 0xFFFF; dell.name = "DELL-54";
    dell.ours = true; dell.present = true; dell.heardTick = 200;
    peers_ = {apple, dell};

    ClaimPort(peers_[1], 0x4000);
    Expect(peers_[0].where == 0x3000, "the Apple TV keeps its own port");
    std::vector<Peer> rows = ListDevices(0x2000);
    Expect(Listed(rows, "Apple TV"), "and is still in the menu");
    Expect(Listed(rows, "DELL-54"), "alongside the PC");

    // Now the corruption that actually happened: something gives the Apple TV
    // the Dell's port. The Dell holds it and is ours, so the claim is refused.
    log_.clear();
    ClaimPort(peers_[0], 0x4000);
    Expect(peers_[0].where == 0x3000, "a claim on a verified PC's port is refused");
    Expect(peers_[1].where == 0x4000, "and the PC keeps it");
    Expect(log_.size() == 1 && log_[0].find("ignored") != std::string::npos,
           "and it is logged");
    rows = ListDevices(0x2000);
    Expect(Listed(rows, "Apple TV") && Listed(rows, "DELL-54"),
           "so both are still listed, which is the bug fixed");

    // The legitimate case: a device really does move to a port someone else
    // held, and neither is ours. The old holder loses its port and will re-ask.
    peers_.clear();
    Peer chromecast; chromecast.address = 4; chromecast.where = 0x2000;
    chromecast.name = "Chromecast"; chromecast.present = true;
    Peer xbox; xbox.address = 11; xbox.where = 0xFFFF; xbox.name = "Xbox";
    xbox.present = true;
    peers_ = {chromecast, xbox};
    log_.clear();
    ClaimPort(peers_[1], 0x2000);
    Expect(peers_[1].where == 0x2000, "the newcomer takes the port");
    Expect(peers_[0].where == 0xFFFF, "the old holder's port becomes unknown");
    Expect(log_.size() == 1, "and the swap is logged");
    rows = ListDevices(0x4000);
    Expect(Listed(rows, "Xbox"), "the newcomer is listed");
    Expect(!Listed(rows, "Chromecast"),
           "the one with no port is not, until it says where it is");

    // Re-claiming the same port changes nothing and logs nothing.
    log_.clear();
    ClaimPort(peers_[1], 0x2000);
    Expect(log_.empty() && peers_[1].where == 0x2000, "a repeat is a no-op");

    // ---- the field scenario: a soundbar moved onto the Apple TV's port.
    peers_.clear();
    Peer polk; polk.address = 5; polk.where = 0x1000; polk.name = "Polk";
    polk.type = 5; polk.present = true; polk.heardTick = 10;
    Peer atv; atv.address = 11; atv.where = 0x3000; atv.name = "Apple TV";
    atv.present = true; atv.heardTick = 20;
    peers_ = {polk, atv};
    log_.clear();
    ClaimPort(peers_[0], 0x3000);           // the soundbar announces HDMI 3
    Expect(peers_[0].where == 0x3000, "the soundbar takes the port it moved to");
    Expect(peers_[1].where == 0xFFFF, "and the Apple TV's port becomes unknown");
    rows = ListDevices(0x4000);
    Expect(Listed(rows, "Polk"), "the soundbar is listed on its new port");
    Expect(!Listed(rows, "Apple TV"),
           "and the displaced device is not offered on a port it has left");

    // ---- the swap completes: the Apple TV turns up on the port the soundbar
    //      vacated, which nothing holds any more.
    log_.clear();
    ClaimPort(peers_[1], 0x1000);
    Expect(peers_[1].where == 0x1000 && peers_[0].where == 0x3000,
           "a straight swap of two ports ends with both correct");
    Expect(log_.empty(), "and needs no eviction, since neither port is held");
    rows = ListDevices(0x4000);
    Expect(Listed(rows, "Polk") && Listed(rows, "Apple TV"),
           "both are listed again");

    // ---- a device claiming a port a PC running this app holds is refused,
    //      because a hello cannot be forged by a torn frame.
    peers_.clear();
    Peer pc; pc.address = 8; pc.where = 0x2000; pc.name = "DELL"; pc.ours = true;
    pc.present = true;
    Peer roku; roku.address = 4; roku.where = 0x3000; roku.name = "Roku";
    roku.present = true;
    peers_ = {pc, roku};
    log_.clear();
    ClaimPort(peers_[1], 0x2000);
    Expect(peers_[1].where == 0x3000 && peers_[0].where == 0x2000,
           "a PC's port is not taken by a bare announcement");

    // ---- the field case of 2026-10-05: the Deck said goodbye, its cable was
    //      pulled, and a Google TV went into the same port. Its correct answer
    //      was refused every ten seconds while the menu offered a sleeping Deck.
    peers_.clear();
    Peer deck; deck.address = 8; deck.where = 0x2000; deck.name = "STEAMDECK";
    deck.ours = true; deck.present = false;
    Peer gtv; gtv.address = 11; gtv.name = "Google TV"; gtv.present = true;
    peers_ = {deck, gtv};
    log_.clear();
    g_now = 50000;
    ClaimPort(peers_[1], 0x2000);           // its answer to <Give Physical Address>
    ClaimPort(peers_[1], 0x2000);           // the same frame, seen a second time
    Expect(peers_[0].where == 0x2000 && peers_[1].where == 0xFFFF,
           "one frame, seen twice, does not move an absent PC's port");
    g_now += 10000;                         // the next round's answer
    ClaimPort(peers_[1], 0x2000);
    Expect(peers_[1].where == 0x2000 && peers_[0].where == 0xFFFF,
           "said again ten seconds later, the port moves to the Google TV");
    rows = ListDevices(0x1000);
    Expect(Listed(rows, "Google TV") && !Listed(rows, "STEAMDECK"),
           "so the menu shows the Google TV on HDMI 2, not a sleeping Deck");

    // ---- a single torn claim on an absent PC's port changes nothing.
    peers_.clear();
    Peer away; away.address = 8; away.where = 0x2000; away.name = "DELL";
    away.ours = true; away.present = false;
    Peer torn; torn.address = 4; torn.where = 0x3000; torn.name = "Roku";
    torn.present = true;
    peers_ = {away, torn};
    g_now = 90000;
    ClaimPort(peers_[1], 0x2000);
    g_now += 60000;
    ClaimPort(peers_[1], 0x3000);           // and then it says its real port
    Expect(peers_[0].where == 0x2000 && peers_[1].where == 0x3000,
           "a one-off claim, then the truth: the absent PC keeps its port");

    // ---- the PC that left comes back to its port: its hello wins as before.
    peers_.clear();
    Peer returned; returned.address = 8; returned.where = 0xFFFF;
    returned.name = "STEAMDECK"; returned.ours = true; returned.present = true;
    Peer holder; holder.address = 11; holder.where = 0x2000; holder.name = "Google TV";
    holder.present = true;
    peers_ = {returned, holder};
    ClaimPort(peers_[0], 0x2000);
    Expect(peers_[0].where == 0x2000 && peers_[1].where == 0xFFFF,
           "a PC running this app announcing a port takes it at once");

    // ---- the field case of 2026-10-06, on the Deck: DESK on CEC address 4
    //      and 1.0.0.0, a Chromecast on 11 and 2.0.0.0, and torn frames with
    //      one device's address in front of the other's payload.
    peers_.clear();
    hellosSent_.clear();
    Peer desk; desk.address = 4; desk.where = 0x1000; desk.name = "DESK";
    desk.ours = true; desk.present = true;
    Peer gcast; gcast.address = 11; gcast.where = 0x2000; gcast.name = "Chromecast";
    gcast.present = true;
    peers_ = {desk, gcast};
    g_now = 200000;
    NotePeer(11, 0x1000, "DESK", false);    // the Chromecast's poll, DESK's hello
    Expect(!peers_[1].ours && peers_[1].where == 0x2000 &&
               peers_[1].name == "Chromecast" && peers_[0].where == 0x1000,
           "a torn hello does not make the Chromecast a PC called DESK");
    Expect(hellosSent_.size() == 1 && hellosSent_[0] == 11,
           "it is asked directly instead");
    g_now += 3000;
    NotePeer(11, 0x1000, "DESK", false);    // the same torn hello again
    Expect(!peers_[1].ours && hellosSent_.size() == 1,
           "said again inside the window: still a stranger, and not asked twice");
    g_now += 180000;
    NotePeer(11, 0x1000, "DESK", false);    // three minutes later, as in the log
    Expect(!peers_[1].ours && peers_[1].where == 0x2000,
           "the three-minute repeat is no proof either");
    g_now += 7000;
    NotePeer(11, 0x1000, "DESK", true);     // a torn Here I Am, after the window
    Expect(!peers_[1].ours, "a Here I Am that answers nothing is not believed");
    NoteReport(4, 0x2000);                  // DESK's poll, the Chromecast's report
    Expect(peers_[0].where == 0x1000 && peers_[1].where == 0x2000,
           "a torn report does not move DESK onto the Chromecast's port");
    rows = ListDevices(0x3000);
    Expect(Listed(rows, "DESK") && Listed(rows, "Chromecast") && rows.size() == 3,
           "so the menu shows DESK, the Chromecast, and this PC");

    // ---- a real PC heard for the first time answers the hello it is sent.
    peers_.clear();
    hellosSent_.clear();
    g_now = 400000;                         // even, so the stored tick is ahead
    NotePeer(11, 0x2000, "DELL-54", false);
    Expect(!peers_[0].ours && hellosSent_.size() == 1, "asked before believed");
    NotePeer(11, 0x2000, "DELL-54", true);  // in the same millisecond
    Expect(peers_[0].ours && peers_[0].where == 0x2000 && peers_[0].name == "DELL-54",
           "its Here I Am makes it a PC running this app");

    // ---- an answer giving another port is not the one asked about.
    peers_.clear();
    hellosSent_.clear();
    g_now = 500000;
    NotePeer(14, 0x2000, "DELL-54", false);
    g_now += 400;
    NotePeer(14, 0x4000, "DELL-54", true);
    Expect(!peers_[0].ours && hellosSent_.size() == 2,
           "a different port is asked about afresh");
    g_now += 400;
    NotePeer(14, 0x4000, "DELL-54", true);
    Expect(peers_[0].ours && peers_[0].where == 0x4000, "and then believed");

    // ---- an answer that comes too late: asked again at the next hello.
    peers_.clear();
    hellosSent_.clear();
    g_now = 600000;
    NotePeer(4, 0x1000, "DESK", false);
    g_now += 6000;
    NotePeer(4, 0x1000, "DESK", true);
    Expect(!peers_[0].ours && hellosSent_.size() == 2, "late, so asked again");
    g_now += 500;
    NotePeer(4, 0x1000, "DESK", true);
    Expect(peers_[0].ours, "and the prompt answer is believed");

    // ---- a PC already known moves port on its hello alone, as before.
    g_now += 10000;
    NotePeer(4, 0x3000, "DESK", false);
    Expect(peers_[0].where == 0x3000 && hellosSent_.size() == 2,
           "a known PC's hello is taken at once");

    // ---- the Vizio's case: a PC asleep on CEC address 11, and an Apple TV
    //      taking 11 while it slept. The Apple TV answers the polls and never
    //      says hello; until now the menu offered the PC, awake, on its port.
    peers_.clear();
    log_.clear();
    mapWanted_ = false;
    Peer dozing; dozing.address = 11; dozing.where = 0x3000; dozing.name = "DELL-54";
    dozing.ours = true; dozing.present = false; dozing.type = 4;
    peers_ = {dozing};
    for (int round = 0; round < 5; ++round) PollRound(peers_[0], 'P');
    Expect(peers_[0].ours && !mapWanted_,
           "five silent rounds: still taken for the PC (a busy spell)");
    PollRound(peers_[0], 'P');
    Expect(!peers_[0].ours && peers_[0].name.empty() && peers_[0].type == 0xFF &&
               mapWanted_,
           "the sixth: an ordinary device, its name and port asked again");
    NoteReport(11, 0x1000);
    Expect(peers_[0].where == 0x1000,
           "and its own report now moves it to the Apple TV's port");

    // ---- a PC that says hello is never taken for anything else.
    peers_.clear();
    mapWanted_ = false;
    Peer live; live.address = 4; live.where = 0x1000; live.name = "DESK";
    live.ours = true; live.present = true;
    peers_ = {live};
    for (int round = 0; round < 30; ++round) {
        PollRound(peers_[0], 'P');
        NotePeer(4, 0x1000, "DESK", false);   // its hello, between rounds
    }
    Expect(peers_[0].ours && !mapWanted_, "thirty rounds with hellos: still a PC");

    // ---- quiet for five rounds, then heard: the count starts again.
    for (int round = 0; round < 5; ++round) PollRound(peers_[0], 'P');
    NotePeer(4, 0x1000, "DESK", true);
    for (int round = 0; round < 5; ++round) PollRound(peers_[0], 'P');
    Expect(peers_[0].ours, "a hello inside the minute starts the count again");

    // ---- asleep, or not reported on, is not silence.
    PollRound(peers_[0], 'A');
    for (int round = 0; round < 5; ++round) PollRound(peers_[0], 'P');
    PollRound(peers_[0], 'U');
    PollRound(peers_[0], 'U');
    Expect(peers_[0].ours,
           "rounds asleep, or with no answer, are not counted against it");

    // ---- a device on a new input is asked its name again; others are not.
    peers_.clear();
    Peer stick; stick.address = 4; stick.where = 0x2000; stick.name = "Fire TV";
    stick.present = true; stick.nameAsks = 3;
    Peer fresh; fresh.address = 8; fresh.name = "Roku"; fresh.present = true;
    Peer copy; copy.address = 11; copy.where = 0x1000; copy.name = "DELL-54";
    copy.ours = true; copy.present = true;
    peers_ = {stick, fresh, copy};
    ClaimPort(peers_[0], 0x3000);
    Expect(peers_[0].nameUnsure && peers_[0].nameAsks == 0 &&
               peers_[0].name == "Fire TV",
           "a device turning up on another input is asked its name again, and "
           "keeps it meanwhile");
    ClaimPort(peers_[1], 0x4000);
    Expect(!peers_[1].nameUnsure, "a device whose port was unknown is not");
    ClaimPort(peers_[2], 0x2000);
    Expect(!peers_[2].nameUnsure, "nor a PC running this app, whose hello names it");

    // ---- the address mask the allocator is given.
    //
    // A sleeping PC still owns its address. Requiring it to answer was the
    // whole bug: a machine that is asleep answers no poll, which is the very
    // reason addresses have to be stable per machine.
    peers_.clear();
    Peer sleeper; sleeper.address = 8; sleeper.where = 0x1000;
    sleeper.name = "STEAMDECK"; sleeper.ours = true; sleeper.present = false;
    Peer cast; cast.address = 11; cast.where = 0x3000;
    cast.name = "Chromecast"; cast.present = true;
    peers_ = {sleeper, cast};
    g_mine = 4;
    log_.clear();
    uint16_t mask = TidyPeerAddresses();
    Expect((mask & (1u << 8)) != 0, "a sleeping PC's address is still reserved");
    Expect((mask & (1u << 11)) == 0,
           "a device that is not this app is not reserved, since it answers polls");
    Expect(peers_.size() == 2, "and nothing was dropped");

    // ---- a neighbour sitting on our own address is dropped and not reserved.
    peers_.clear();
    Peer onUs; onUs.address = 4; onUs.where = 0x1000; onUs.name = "STEAMDECK";
    onUs.ours = true; onUs.present = true;
    peers_ = {onUs, cast};
    g_mine = 4;
    log_.clear();
    mask = TidyPeerAddresses();
    Expect(peers_.size() == 1 && peers_[0].name == "Chromecast",
           "a neighbour on our own address is dropped");
    Expect((mask & (1u << 4)) == 0, "and does not reserve the address we hold");
    Expect(log_.size() == 1, "and it is logged");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
