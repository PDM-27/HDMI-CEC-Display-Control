#include "CecDevice.h"

#include <windows.h>

#include <iterator>

#include "Edid.h"
#include "AmdBackend.h"
#include "IntelBackend.h"
#include "NvidiaBackend.h"

namespace cec {
namespace {

// CEC header byte: high nibble = source logical address, low nibble = target.
uint8_t Header(uint8_t source, uint8_t target) {
    return static_cast<uint8_t>((source << 4) | (target & 0x0F));
}

// <Report Power Status> from the television, at its exact length: header,
// opcode, one operand. Only the set's own answer says whether the set is on.
//
// Both readers used to take a 0x90 from anyone at three bytes or more, which is
// the torn-frame mistake this code has made three times over. The
// receive buffer puts a stale opcode in front of somebody else's payload, and
// the field logs have exactly this one: "8->4 90 Report Power Status 00 45 43
// 01 ...", another PC's hello wearing a 0x90 whose first operand, 00, reads as
// "on". Arriving during a standby verify, that is a resend of <Standby> - a
// toggle on the TCL - on the word of a frame that was never about the set.
bool IsTvPowerReport(const std::vector<uint8_t>& msg) {
    return msg.size() == 3 && msg[1] == 0x90 &&
           static_cast<uint8_t>(msg[0] >> 4) == kLogicalAddressTv;
}

// CEC only requires about 7 data-bit periods (~17ms) of idle between frames
// from the same initiator, so this could legally be far shorter - and briefly
// was 60ms, to shorten the window where the TV shows its placeholder label.
// That change coincided with the TV rendering a mangled name ("HT C" for
// "HTPC"), so it is back at a conservative value until there is evidence the
// two are unrelated. The visible win came from reordering the announce, not
// from shaving this.
constexpr int kInterFrameDelayMs = 200;

// Deliberately much shorter than kInterFrameDelayMs; see
// SendUserControl. Still comfortably above the ~17ms CEC minimum.
constexpr int kUserControlReleaseMs = 45;

// How many times a refused volume press is tried again, and how far apart.
//
// Measured: during fast clicking the TCL refuses a press now and then - a NACK,
// "CEC frame was not acknowledged" - and the adapter's own retries, which follow
// each other within milliseconds, do not outlast whatever it is busy with. The
// click is then simply lost. A retry a moment later reaches it, and a step that
// lands 50ms late is better than one that never lands. A refused frame was not
// taken, so trying again cannot double a step.
constexpr int kPressRetries = 2;
constexpr DWORD kPressRetryMs = 50;

// How long a set is given to act on <Standby> before an answer saying it is
// still on is believed. Hard rule 1: two and a half seconds or more. See
// WaitForStandby.
constexpr DWORD kStandbyTransitionMs = 2500;

}  // namespace

// The PC's name as shown in Settings > System > About. Used as the default
// label the TV displays for this input, so every tool that announces agrees on
// it - a tool with its own hardcoded name silently relabels the TV input for
// everything else.
const std::string& DefaultOsdName() {
    // Cached: the machine name cannot change without a reboot, and this used to
    // run on every CecDevice construction.
    static const std::string cached = []() -> std::string {
        wchar_t buffer[64] = {};
        DWORD size = 64;
        if (!GetComputerNameW(buffer, &size)) return "Playback";
        std::string ascii;
        for (DWORD i = 0; i < size && ascii.size() < 14; ++i) {
            if (buffer[i] >= 0x20 && buffer[i] <= 0x7E) {
                ascii += static_cast<char>(buffer[i]);
            }
        }
        return ascii.empty() ? "Playback" : ascii;
    }();
    return cached;
}

const char* PowerStatusName(PowerStatus status) {
    switch (status) {
        case PowerStatus::On:                     return "ON";
        case PowerStatus::Standby:                return "STANDBY";
        case PowerStatus::TransitioningToOn:      return "transitioning to on";
        case PowerStatus::TransitioningToStandby: return "transitioning to standby";
        default:                                  return "unknown";
    }
}

std::vector<std::unique_ptr<AuxBackend>> CreateAllBackends() {
    std::vector<std::unique_ptr<AuxBackend>> backends;
    backends.push_back(std::make_unique<NvidiaBackend>());
    backends.push_back(std::make_unique<AmdBackend>());
    backends.push_back(std::make_unique<IntelBackend>());
    // Order does not matter: every backend is opened and probed, and the one
    // physically wired to the adapter is the one that answers. See
    // AuxBackend.h for why that is empirical rather than a lookup.
    return backends;
}

bool CecDevice::WriteReg(uint32_t address, const std::vector<uint8_t>& data) {
    if (backend_.WriteDpcd(port_, address, data)) return true;
    lastError_ = backend_.LastError();
    return false;
}

bool CecDevice::IsCecCapable(bool& capable) {
    capable = false;
    std::vector<uint8_t> caps;
    if (!backend_.ReadDpcd(port_, kCecTunnelingCapability, 1, caps)) {
        lastError_ = backend_.LastError();
        return false;
    }
    capable = (caps[0] & 0x01) != 0;
    return true;
}

bool CecDevice::Initialise() {
    if (!WriteReg(kCecTunnelingControl, {0x01})) return false;
    return ClaimAddressMask((1u << logicalAddress_) |
                            (1u << kLogicalAddressBroadcast));
}

bool CecDevice::ClaimAddressMask(uint16_t mask) {
    // 16-bit mask, little-endian; bit N means "ACK for logical address N".
    return WriteReg(kCecLogicalAddressMask,
                    {static_cast<uint8_t>(mask & 0xFF),
                     static_cast<uint8_t>(mask >> 8)});
}

// How long to leave between the two polls that decide an address is free. Long
// enough to outlast another device's probe or announce, short enough that an
// allocation is still quick. See AllocateLogicalAddress.
constexpr int kAddressProbeGapMs = 250;

bool CecDevice::AllocateLogicalAddress(uint8_t preferred) {
    // The one held last time comes first, when the caller remembers one.
    //
    // A television keeps what it has learnt about each address, and this one
    // keeps it stubbornly: it went on believing an address belonged to the
    // laptop's HDMI port after the desktop had taken that address and announced
    // its own, and then ignored <Standby> from it, since as far as the set was
    // concerned the command came from an input it was not showing. Addresses
    // therefore want to be stable per machine rather than handed out first come
    // first served, which they are not when one PC is asleep at the moment the
    // other attaches and so leaves its address looking free.
    uint8_t candidates[] = {kLogicalAddressPlayback1, kLogicalAddressPlayback2,
                            kLogicalAddressPlayback3, kLogicalAddressFreeUse};
    for (size_t i = 1; i < std::size(candidates); ++i) {
        if (candidates[i] != preferred) continue;
        candidates[i] = candidates[0];
        candidates[0] = preferred;
        break;
    }

    // Both outputs are set on every path out of this function, including the
    // two below that give up before anything is asked. They used to return
    // false leaving lastError_ holding whatever an unrelated call had put
    // there, which the caller then printed as the reason - and leaving
    // probedTheBus_ stale, which is worse, because the caller decides whether
    // to try again from it. A stale "we did probe" means never retrying a
    // moment of noise.
    probedTheBus_ = false;
    // True unless the restore below fails. The two giving-up returns under this
    // leave the mask exactly as it was, because a failed register write is a
    // write that did not happen, so the address is still held.
    addressUsable_ = true;
    if (!WriteReg(kCecTunnelingControl, {0x01})) {
        lastError_ = "could not enable CEC tunneling to pick an address";
        return false;
    }

    // Keep the broadcast address claimed the whole way through. Only the
    // playback bit has to be dropped, and being completely unaddressed - even
    // briefly - is genuinely dangerous: if the restore then fails, the TV stops
    // seeing this device at all and ignores every command afterwards. That is
    // not theoretical, it happened during development and needed a manual
    // register write to recover.
    if (!ClaimAddressMask(1u << kLogicalAddressBroadcast)) {
        lastError_ = "could not free the logical address to probe with";
        return false;
    }

    // The mask is a preference, not a cage. If it rules out every candidate -
    // which needs as many other copies of this app as there are addresses, but
    // is reachable - then ignoring it and asking the bus is the only way to end
    // up with an address at all, and a shared address is far better than none.
    int usable = 0;
    for (const uint8_t candidate : candidates) {
        // Counting only the addresses this device could move *to*. Its own does
        // not count: it is never masked, so counting it would always make the
        // mask look survivable and the fallback below would never fire.
        if (candidate != logicalAddress_ &&
            !(avoidAddresses_ & (1u << candidate))) {
            ++usable;
        }
    }
    const uint16_t avoid = usable ? avoidAddresses_ : 0;

    uint8_t chosen = 0;
    size_t asked = 0;
    for (const uint8_t candidate : candidates) {
        // Known to belong to another copy of this app, which said so itself.
        // Never applied to the address this device already holds: that one has
        // to stay askable, or a stale entry would stop it keeping its own.
        // See set_avoidAddresses.
        if (candidate != logicalAddress_ && (avoid & (1u << candidate))) {
            continue;
        }
        // A polling message is a lone header byte addressed to itself.
        // ACK  -> some device already holds that address.
        // NACK -> nobody answered, so it is free.
        TxResult tx;
        SendFrame({Header(candidate, candidate)}, &tx);
        if (!tx.accepted) continue;   // could not probe; try the next
        ++asked;
        if (!tx.nacked) continue;     // somebody is there

        // Asked twice, a moment apart, before it is believed. One silence is
        // not proof an address is free: a device part way through announcing
        // itself, or probing for an address of its own, answers nothing for a
        // few frames. That window is what let two PCs take each other's
        // addresses and then chase each other round the bus. A second question
        // after a pause costs one frame on an allocation that happens rarely,
        // and an address that is genuinely free answers nothing both times.
        Sleep(kAddressProbeGapMs);
        TxResult again;
        SendFrame({Header(candidate, candidate)}, &again);
        if (again.accepted && !again.nacked) continue;   // it was only quiet

        chosen = candidate;
        break;
    }
    // Every candidate had to answer for "they are all taken" to be true. This
    // counted *any* successful probe, so one address that answered plus three
    // that could not be put on the wire was reported as a full bus - a claim
    // about four addresses made from evidence about one, and one that stops the
    // caller retrying.
    const bool conclusive = asked == std::size(candidates);

    if (chosen) logicalAddress_ = chosen;

    // Restore unconditionally and stubbornly. Picking the wrong address is a
    // minor problem; being left unaddressed is a total loss of function, so
    // this must not be allowed to fail quietly on one flaky write.
    const uint16_t mask =
        (1u << logicalAddress_) | (1u << kLogicalAddressBroadcast);
    bool restored = false;
    for (int attempt = 0; attempt < 5 && !restored; ++attempt) {
        restored = ClaimAddressMask(mask);
        if (!restored) Sleep(50);
    }
    addressUsable_ = restored;
    if (!restored) {
        // Left unaddressed, which is the one outcome worth retrying hardest:
        // the device is useless until this succeeds, and useless silently.
        lastError_ = "could not restore the logical address after probing";
        return false;
    }

    if (chosen) {
        probedTheBus_ = true;
        lastError_.clear();
        return true;
    }
    // Two different failures, and the caller reports whichever it was rather
    // than assuming the first. They want different responses: the first is the
    // bus being genuinely full, which nothing can fix, and the second is a
    // moment of noise, which fixes itself on the next attempt.
    probedTheBus_ = conclusive;
    lastError_ = conclusive ? "every logical address this app can use is taken"
                            : "could not probe the CEC bus for a free address";
    return false;
}

bool CecDevice::Announce(uint16_t physicalAddress) {
    physicalAddress_ = physicalAddress;
    const uint8_t hi = static_cast<uint8_t>(physicalAddress >> 8);
    const uint8_t lo = static_cast<uint8_t>(physicalAddress & 0xFF);
    const uint8_t bcast = Header(logicalAddress_, kLogicalAddressBroadcast);

    // Best effort across all of them: a single frame occasionally fails to get
    // a transmit status back, and aborting there used to skip everything after
    // it. Partial registration beats none.
    //
    // Order matters and is not the order these are usually listed in. The TV
    // invents a placeholder label ("Playback_1") the instant it sees
    // <Report Physical Address>, and keeps showing it until <Set OSD Name>
    // arrives - so the name goes second, immediately after the address, rather
    // than last behind <Device Vendor ID>. The vendor ID affects nothing the
    // user can see and goes last.
    bool allSent = true;

    // <Report Physical Address>, device type 4 (Playback Device). This is what
    // joins the topology, so it has to be first - and it is a broadcast, so it
    // is the one worth retrying. See SendBroadcast.
    allSent &= SendBroadcast({bcast, 0x84, hi, lo, 0x04});
    Sleep(kInterFrameDelayMs);

    // <Set OSD Name> - what the TV shows for this input. Retried once, because
    // this is the frame whose loss is most visible: the TV is left displaying
    // the placeholder, or "HDMI 1" if it never registered us at all. Resending
    // it is harmless - unlike the power commands, it is genuinely idempotent.
    if (!SendOsdName()) {
        Sleep(kInterFrameDelayMs);
        allSent &= SendOsdName();
    }
    Sleep(kInterFrameDelayMs);

    // <Device Vendor ID>. The value itself does not matter to the TV; what
    // matters is answering, since the TV asks for it as soon as we appear.
    allSent &= SendFrame({bcast, 0x87, 0x00, 0x00, 0x00});
    Sleep(kInterFrameDelayMs);

    return allSent;
}

void CecDevice::set_osdName(const std::string& name) {
    // A CEC frame is at most 16 bytes and this one spends 2 on header and
    // opcode, so the name gets 14. Non-printable and non-ASCII bytes are
    // dropped rather than passed through, since TVs render them unpredictably.
    std::string clean;
    for (const char c : name) {
        if (c >= 0x20 && c <= 0x7E) clean += c;
        if (clean.size() == 14) break;
    }
    if (!clean.empty()) osdName_ = clean;
}

bool CecDevice::DiscoverPhysicalAddress() {
    addressNote_.clear();
    addressFromWindows_ = false;
    std::vector<uint8_t> edid;
    std::string error;
    bool fromWindows = false;
    if (!ReadEdid(backend_, port_, edid, &error)) {
        // Windows' own copy, which needs nothing from the graphics driver.
        // Intel's control library refuses the I2C write that a DDC read starts
        // with, so on Intel this is the only route - and without an address
        // this PC announces a default one, which on a TV that already has
        // another PC on that port means two devices claiming to be the same
        // input. Kept as the second choice rather than the first because the
        // sink's own answer is about this port, while Windows' is about
        // whichever television it knows of.
        const std::string sinkError = error;
        if (!ReadEdidFromWindows(edid, &error)) {
            lastError_ = "EDID read failed: " + sinkError;
            addressNote_ = lastError_ + "; Windows could not help either: " + error;
            return false;
        }
        addressNote_ = "the sink would not answer (" + sinkError +
                       "), so the address comes from Windows' copy of the EDID";
        fromWindows = true;
        addressFromWindows_ = true;
    }
    uint16_t address = 0;
    if (!ParsePhysicalAddress(edid, address)) {
        lastError_ = "sink advertises no HDMI VSDB, so it has no CEC address";
        addressNote_ = lastError_;
        return false;
    }
    physicalAddress_ = address;

    // Only from an EDID that also carried a CEC address, which is to say a
    // television rather than a plain monitor, and only when it checks out. A
    // read that does not leaves the previous identity in place.
    DisplayIdentity identity;
    if (!ParseDisplayIdentity(edid, identity)) return true;

    // What Windows' copy is trusted for, and what it is not.
    //
    // It has been seen to name the same television differently: a set normally
    // read as TCL-9653-00010000 came back from Windows as SAM-9653-00010000 -
    // the same product and serial under another maker's letters. Believing that
    // stores a second television and splits one set's remembered settings in
    // two, so where the product and serial match, the name already in hand
    // wins.
    //
    // Where they do not match it is a different television, and saying so is
    // the whole point. Refusing every disagreement was wrong: on Intel the sink
    // never answers at all, so Windows' copy is the only identity there is, and
    // a laptop carried to another set would have gone on filing that set's
    // settings under the one at home. Caught in a log - a Dell moved from a TCL
    // to a Fire TV, Windows said AMZ-0000-00000000, and the service kept
    // calling it the TCL.
    const bool sameSet =
        identity_.key.size() > 4 && identity.key.size() > 4 &&
        identity_.key.compare(4, std::string::npos, identity.key, 4,
                              std::string::npos) == 0;
    if (fromWindows && !identity_.key.empty() && identity_.key != identity.key &&
        sameSet) {
        addressNote_ += "; Windows calls this set " + identity.key +
                        " while it has been reading as " + identity_.key +
                        ", and the product and serial match, so the name is "
                        "left as it was";
        return true;
    }
    if (fromWindows && !identity_.key.empty() && identity_.key != identity.key) {
        addressNote_ += "; this is a different television: " + identity_.key +
                        " before, " + identity.key + " now";
    }
    identity_ = std::move(identity);
    return true;
}

bool CecDevice::Attach(bool discoverAddress) {
    // A wrong physical address puts us at the wrong place in the CEC topology,
    // so prefer the sink's own EDID. Falling back to the previous value keeps
    // a monitor with an unreadable EDID from blocking power control entirely.
    // On a wake path the caller can skip this: the display has not changed, and
    // the EDID read costs several hundred milliseconds of visible latency.
    if (discoverAddress) DiscoverPhysicalAddress();
    if (!Initialise()) return false;
    DrainRx();
    // Pick an address that is actually free, preferring the one held before -
    // which on a service restart is the one the television already associates
    // with this PC's input. Cheap enough for the full attach; the wake path
    // deliberately reuses whatever this settled on.
    // The result is deliberately not fatal on its own: failing to find a free
    // address still leaves this device on the one it had, which is usually the
    // one the television already knows, and a clash will be noticed and acted
    // on later. Being left with no address at all is different, and is the one
    // case worth refusing the attach for, because nothing downstream can detect
    // or repair it. See addressUsable.
    AllocateLogicalAddress(preferredAddress_);
    if (!addressUsable_) return false;
    // A partly-sent announce is not a reason to throw the device away - the
    // caller would discard a working connection over one flaky frame.
    Announce(physicalAddress_);
    return true;
}

bool CecDevice::WakeDisplay(uint32_t* msToFirstCommand) {
    // Enabling tunneling and reclaiming the logical address is two DPCD writes
    // and is all that is strictly needed to put a frame on the wire, so do that
    // and fire <Image View On> immediately: the TV has almost always kept us in
    // its topology from before the sleep, and this gets it powering on roughly
    // a second earlier than waiting for the full announce would.
    const DWORD started = GetTickCount();
    if (!Initialise()) return false;
    SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x04});
    if (msToFirstCommand) {
        *msToFirstCommand = static_cast<uint32_t>(GetTickCount() - started);
    }

    // With the TV already powering on - which takes it seconds - re-reading the
    // EDID now costs nothing the user can perceive, and it means a display
    // swapped or re-plugged while the PC slept is picked up on the next wake
    // rather than needing a service restart. The cached address stays as the
    // fallback for a sink whose EDID cannot be read.
    DiscoverPhysicalAddress();

    // Then rejoin properly and repeat, which covers the case where the TV did
    // drop us. <Image View On> is idempotent, so the duplicate is harmless.
    // Carry on even if part of the announce failed: <Active Source> is what
    // switches the TV to this input, and skipping it over one dropped frame is
    // worse than sending it with an incomplete registration.
    Announce(physicalAddress_);
    // No ServiceQueries() here, but not because there is nothing to answer.
    //
    // An earlier note here said the TV sends nothing but a poll after a
    // re-announce. That was wrong, and a frame log on the TCL says so plainly:
    // about a second after this returns it polls, then asks
    // <Give Physical Address>, <Give OSD Name>, <Give Device Vendor ID> and
    // <Give Device Power Status> in sequence. The original measurement watched
    // the two seconds immediately after the announce and concluded from silence
    // that none was coming.
    //
    // Those questions are answered by the service's query pump, which is
    // listening the whole time and does not hold up the wake path to do it.
    // Blocking here would put that second back into the wake handler for no
    // benefit.
    return SendImageViewOn();
}

bool CecDevice::SendOsdName() {
    std::vector<uint8_t> osd = {Header(logicalAddress_, kLogicalAddressTv), 0x47};
    osd.insert(osd.end(), osdName_.begin(), osdName_.end());
    return SendFrame(osd);
}

bool CecDevice::SendUserControlReleased(uint8_t target) {
    return SendFrame({Header(logicalAddress_, target), 0x45});
}

void CecDevice::ForgetTelevision() {
    // Volume. audioSystem_ is re-probed on a timer and would have corrected
    // itself within a minute, but a minute of volume keys addressed to a
    // soundbar that is not on this bus is a minute of nothing happening.
    audioSystem_ = false;
    volumeTarget_ = kLogicalAddressTv;
    audioNews_ = AudioNews();

    // What is on screen, and what the old set said about routing. Without this
    // the menu's dot points at an input of a television that is no longer
    // there, and TvAnnouncesRouting claims the new set announces routing
    // because the old one did - which decides whether its silence after a
    // switch is read as a refusal.
    activePath_ = 0;
    activePathKnown_ = false;
    activePathAnswered_ = false;
    claimedTick_ = 0;
    requestedPath_ = 0;
    requestedKnown_ = false;
    requestedTick_ = 0;
    tvPath_ = 0;
    tvPathTick_ = 0;

    // The last refusal, and the other copy of this app that was on the old bus.
    lastAbortOpcode_ = 0;
    lastAbortTick_ = 0;
    peerSeen_ = 0xFF;

    // So whoever draws the dot is told it is unknown, rather than being left
    // holding the old answer until something on the new bus speaks.
    PublishScreenPath();
}

void CecDevice::DetectAudioSystem() {
    // A polling message is a lone header byte. An ACK means something holds
    // the audio system address; a NACK means nothing does and volume has to go
    // to the TV instead.
    //
    // Only a clean answer changes the decision. If the probe itself could not
    // be sent, the previous answer is better than a guess made from a failure.
    // That was the intent and not what the code did: it tested only that the
    // frame was handed to the adapter, so a probe whose status never came back
    // read as a soundbar. Seen on the Deck after two cable flaps - a Fire TV
    // with no soundbar was given one, and volume went to address 5, where
    // nothing listens, for two minutes.
    switch (Poll(kLogicalAddressAudioSystem)) {
        case PollAnswer::Present: audioSystem_ = true;  break;
        case PollAnswer::Absent:  audioSystem_ = false; break;
        case PollAnswer::Unknown: break;
    }
}

bool CecDevice::SendVolumePressed(uint8_t uiCommand) {
    pressRetries_ = 0;
    const uint8_t target = volumeTarget();
    volumeTarget_ = target;
    if (SendPressRetrying(uiCommand, target)) return true;

    // The audio system stopped answering. Fall back for this press and stop
    // assuming it is there, so the next one goes straight to the TV rather
    // than paying for the same failure again.
    if (target == kLogicalAddressAudioSystem) {
        audioSystem_ = false;
        volumeTarget_ = kLogicalAddressTv;
        return SendPressRetrying(uiCommand, kLogicalAddressTv);
    }
    return false;
}

bool CecDevice::SendPressRetrying(uint8_t uiCommand, uint8_t target) {
    // Mute is never retried. It is a toggle, and a refusal the adapter
    // misreported would turn it straight back.
    const int retries = uiCommand == kUiMute ? 0 : kPressRetries;
    for (int attempt = 0;; ++attempt) {
        TxResult tx;
        if (SendFrame({Header(logicalAddress_, target), 0x44, uiCommand}, &tx)) {
            return true;
        }
        // Only a refusal is worth another try. A line error or a silent
        // adapter will not be fixed by 50ms.
        if (!tx.nacked || attempt >= retries) return false;
        ++pressRetries_;
        Sleep(kPressRetryMs);
    }
}

bool CecDevice::SendVolumeReleased() {
    // Deliberately the recorded target, not the current one: if the press went
    // to the audio system and detection has since changed its mind, releasing
    // to the TV would leave the audio system repeating the key for ever.
    return SendUserControlReleased(volumeTarget_);
}

bool CecDevice::SendVolumeTap(uint8_t uiCommand) {
    const uint8_t target = volumeTarget();
    if (SendUserControl(uiCommand, target)) return true;
    if (target == kLogicalAddressAudioSystem) {
        audioSystem_ = false;
        return SendUserControl(uiCommand, kLogicalAddressTv);
    }
    return false;
}

bool CecDevice::SendUserControl(uint8_t uiCommand, uint8_t target) {
    const uint8_t toTarget = Header(logicalAddress_, target);

    // <User Control Pressed> means a key is being *held*: the TV repeats the
    // action until <User Control Released> arrives or its own repeat timeout
    // expires. So the release is not a formality, it is what stops the volume
    // running away - and it has to be prompt. The normal 200ms inter-frame gap
    // was long enough for the TV to start repeating before the release landed.
    if (!SendFrame({toTarget, 0x44, uiCommand})) return false;
    Sleep(kUserControlReleaseMs);
    return SendFrame({toTarget, 0x45});
}

bool CecDevice::SendSetStreamPath(uint16_t physicalAddress) {
    // Through SendBroadcast, like every other broadcast here, and it was the
    // one that was not.
    //
    // A broadcast is refused by a single device asserting the acknowledge bit,
    // and the initiator cannot tell which device or why - but the frame still
    // reaches everyone else. That is exactly what a log showed: the request
    // went out, the Chromecast heard it and claimed the screen, and the
    // television never acknowledged it and never acted. The click that
    // followed sent the same frame again and it worked. Sent once with no check
    // on the answer, that reads as the television refusing to switch inputs,
    // which is what it was taken for. Three tries, 200ms apart, is what the
    // announce and <Active Source> have always had.
    return SendBroadcast({Header(logicalAddress_, kLogicalAddressBroadcast), 0x86,
                          static_cast<uint8_t>(physicalAddress >> 8),
                          static_cast<uint8_t>(physicalAddress & 0xFF)});
}

bool CecDevice::SendActiveSource() {
    const bool sent =
        SendBroadcast({Header(logicalAddress_, kLogicalAddressBroadcast), 0x82,
                       static_cast<uint8_t>(physicalAddress_ >> 8),
                       static_cast<uint8_t>(physicalAddress_ & 0xFF)});
    // An adapter does not hear its own broadcasts, so this device's claim is
    // recorded here rather than when it comes back off the wire. It counts as a
    // statement, because it is one: this device is saying what is on screen.
    if (sent) {
        activePath_ = physicalAddress_;
        activePathKnown_ = true;
        claimedTick_ = GetTickCount();
        // An adapter does not hear its own broadcasts, so without this the one
        // claim nobody else can tell us about would be the one the menu missed.
        PublishScreenPath();
    }

    // Volunteered, not waited for. Becoming the active source is the moment the
    // remote becomes useful, and a TV that would have asked <Menu Request> is
    // told the answer before it does; one that never asks is told anyway, which
    // is the case this exists for. Harmless either way - it is a statement
    // about this device, and a set uninterested in it ignores it.
    Sleep(kInterFrameDelayMs);
    SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x8E, 0x00});
    return sent;
}

bool CecDevice::SendPowerOffKey() {
    return SendUserControl(0x6C, kLogicalAddressTv);
}

bool CecDevice::SendStandby(TxResult* result) {
    return SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x36},
                     result);
}

bool CecDevice::AnswerQuery(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) return false;   // a bare polling message

    const uint8_t src = static_cast<uint8_t>(frame[0] >> 4);
    const uint8_t dst = static_cast<uint8_t>(frame[0] & 0x0F);

    // <Request Active Source> is a broadcast, and answering it is how a TV that
    // has just been switched on learns that what it is showing is this PC.
    // Answered only while this device believes it is the input on screen, since
    // claiming otherwise would drag the TV away from whatever it is showing.
    if (dst == kLogicalAddressBroadcast && frame[1] == 0x85) {
        if (!OnScreen()) return false;
        SendActiveSource();
        return true;
    }

    // <Set Stream Path>, and <Routing Change> to this device's own address: the
    // standard way a television asks a device to become what is on screen, and
    // the half of Routing Control this app never implemented.
    //
    // It is why "switch input" appeared not to work. The television does not
    // switch inputs on being asked by a source - it passes the request to the
    // bus, and the device named is required to claim the screen itself, which
    // the TV then follows because that part is One Touch Play. A Chromecast
    // does this and switches; this app did not, so a request naming a PC
    // running it went nowhere. Seen plainly in one log:
    //
    //   pipe command 'input 2' sent
    //   [route] 0->F <Set Stream Path> 2.0.0.0
    //   [route] 8->F <Active Source> 2.0.0.0
    //
    // Both messages are broadcast, so this sits before the addressed-to-me
    // check below. The claim itself is the service's to make or refuse, since
    // only it knows whether this PC has a picture to show. Rate limited,
    // because a set that repeats the request - and this one repeats it twice -
    // would otherwise have this device claiming the screen over and over.
    // Broadcast only, both of them, and NoteRouting has already counted a copy
    // that arrived otherwise. Checked again here because this is the one place a
    // received frame makes this PC do something to the television.
    if (dst == kLogicalAddressBroadcast &&
        ((frame[1] == 0x86 && frame.size() == 4) ||
         (frame[1] == 0x80 && frame.size() == 6))) {
        const uint16_t path =
            frame[1] == 0x86
                ? static_cast<uint16_t>((frame[2] << 8) | frame[3])
                : static_cast<uint16_t>((frame[4] << 8) | frame[5]);
        const uint32_t now = GetTickCount();
        if (path == physicalAddress_ && physicalAddress_ != 0 &&
            onAppTakeScreen_ && (!lastClaimTick_ || now - lastClaimTick_ > 3000)) {
            lastClaimTick_ = now ? now : 1;
            onAppTakeScreen_();
        }
    }

    // The private channel between copies of this app, handled before the check
    // below because the hello that finds them is a broadcast - and a broadcast
    // never reaches that check. This is why two PCs saw each other once, at
    // attach, where the hello is addressed to one device at a time, and then
    // lost each other: every greeting after that was broadcast, nobody
    // answered, and each dropped the other for going quiet.
    if (frame[1] == 0xA0) {
        // Too short to be the message it claims to be, so it is not one.
        //
        // <Vendor Command With ID> carries a three-byte vendor ID and then at
        // least one byte of its own, which puts the floor at four operands.
        // Anything shorter is a torn frame, and the receive buffer produces
        // them: caught in a traffic log as 0xA0 with the single operand 14,
        // where the opcode was left over from this app's own vendor traffic and
        // the operand was the tail of the television's <Report Audio Status>.
        // Aborting that told a device which had said nothing that this one did
        // not understand it. The same reasoning as the list of opcodes further
        // down - the adapter's frames cannot be prevented here, but replying to
        // them with nonsense was ours.
        if (frame.size() < 6) {
            ++malformedFrames_;
            return false;
        }
        if (frame.size() < 7 || frame[2] != kAppVendorId[0] ||
            frame[3] != kAppVendorId[1] || frame[4] != kAppVendorId[2] ||
            frame[5] != kAppProtocolVersion) {
            // A stranger's vendor command. Answered as an unknown opcode when
            // it was addressed to this device, and ignored when it was not,
            // since a broadcast must never be answered with <Feature Abort>.
            if (dst == logicalAddress_) {
                SendFrame({Header(logicalAddress_, src), 0x00, frame[1], 0x00});
            }
            return dst == logicalAddress_;
        }
        if (frame[6] == kAppHello) {
            NotePeer(frame);
            SendHereIAm(src);
        } else if (frame[6] == kAppHereIAm) {
            peerSeen_ = src;
            NotePeer(frame);
        } else if (frame[6] == kAppBye) {
            if (onAppPeerGone_) onAppPeerGone_(src);
        } else if (frame[6] == kAppTakeScreen && onAppTakeScreen_) {
            // Only ever obeyed when addressed to this device. A broadcast
            // asking everyone to take the screen would have every PC on the
            // bus grabbing it at once.
            if (dst == logicalAddress_) {
                SendAppMessage(src, onAppTakeScreen_() ? kAppTaken : kAppRefused);
            }
        } else if ((frame[6] == kAppTaken || frame[6] == kAppRefused) &&
                   src == peerAsked_) {
            // Only from the PC that was asked, like every other answer here.
            // Taken from anyone, a reply that wandered in from another copy of
            // this app - or a torn frame wearing one's header - would end the
            // wait with somebody else's yes or no.
            peerAnswer_ = frame[6];
        }
        return true;
    }

    if (dst != logicalAddress_) return false;   // addressed to someone else

    const uint8_t toSrc = Header(logicalAddress_, src);
    const uint8_t bcast = Header(logicalAddress_, kLogicalAddressBroadcast);
    const uint8_t hi = static_cast<uint8_t>(physicalAddress_ >> 8);
    const uint8_t lo = static_cast<uint8_t>(physicalAddress_ & 0xFF);

    switch (frame[1]) {
        case 0x46: {   // <Give OSD Name> -> <Set OSD Name>
            // The name is what the TV labels this input with, and answering
            // this is the only reliable way it ever gets one: an unsolicited
            // <Set OSD Name> is a suggestion the TV may ignore, whereas this is
            // a reply to a question it asked. Never answer with nothing - a TV
            // given an empty name shows a placeholder instead.
            const std::string& name = osdName_.empty() ? DefaultOsdName() : osdName_;
            std::vector<uint8_t> reply = {toSrc, 0x47};
            reply.insert(reply.end(), name.begin(), name.end());
            SendFrame(reply);
            break;
        }
        case 0x8C:     // <Give Device Vendor ID> -> <Device Vendor ID>
            SendFrame({bcast, 0x87, 0x00, 0x00, 0x00});
            break;
        case 0x83:     // <Give Physical Address> -> <Report Physical Address>
            SendFrame({bcast, 0x84, hi, lo, 0x04});
            // Volunteer the name here, without being asked for it yet.
            //
            // A TV that asks this is enumerating, and on the TCL it asks for
            // the name about a second later - a second it spends displaying
            // "Playback_1". The same set ignores the <Set OSD Name> sent during
            // our own announce, so the name is not something it accepts at any
            // time; but it is accepting answers right now, in the middle of a
            // conversation it started, which is the best moment there is to
            // offer one. If it ignores this too, the request that follows is
            // answered as before and nothing is lost but one frame.
            Sleep(kInterFrameDelayMs);
            SendOsdName();
            break;
        case 0x8F:     // <Give Device Power Status> -> <Report Power Status>
            // The PC is by definition awake if it is answering at all.
            SendFrame({toSrc, 0x90, 0x00});
            break;
        case 0x9F:     // <Get CEC Version> -> <CEC Version> 1.4
            SendFrame({toSrc, 0x9E, 0x05});
            break;
        case 0x8D:     // <Menu Request> -> <Menu Status> activated
            // Device Menu Control, and the reason it is not optional here: this
            // exchange is what a TV uses to decide whether a device wants the
            // remote. Answering 0x00 means "my menu is active, send me the
            // buttons"; leaving it to the default case below answered
            // <Feature Abort>, which says the opposite as plainly as CEC can.
            //
            // A PC is always ready to be driven - there is no menu to enter or
            // leave - so this is unconditionally activated rather than tracking
            // a state the application does not have.
            SendFrame({toSrc, 0x8E, 0x00});
            break;
        // Remote keys, forwarded by the TV because this device is the active
        // source. Reported upwards and never answered: a reply here would put a
        // frame on the wire for every button the viewer presses, telling the TV
        // something it did not ask.
        case 0x44:     // <User Control Pressed>
            if (onUserControl_ && frame.size() >= 3) onUserControl_(frame[2], true);
            return false;
        case 0x45:     // <User Control Released>
            if (onUserControl_) onUserControl_(0, false);
            return false;

        // Statements, not questions, and none of them ours to acknowledge.
        //
        // <Report Audio Status>, whether it answers our <Give Audio Status> or
        // arrives unasked. Counted as news rather than acted on here: the
        // service decides what the tray is told, and this layer only records.
        case 0x7A:
            // Exactly one operand, and only from whoever owns volume on this
            // bus. Both halves of that were learnt from one capture on a bus
            // with a soundbar, an Apple TV and two PCs, where the torn frames
            // the receive buffer produces arrived looking like this:
            //
            //   8->4  7A Report Audio Status  14 45 43 01 01 20 00 53 54 ...
            //   8->F  7A Report Audio Status  14
            //   0->4  7A Report Audio Status  14
            //
            // The first is a stale 7A opcode carrying the tail of another PC's
            // hello, and its first operand, 0x43, would have shown the volume
            // as 67 and jumped the slider's thumb there. The other two are a
            // PC and the television appearing to answer a question that was
            // only ever asked of the soundbar on address 5.
            if (frame.size() == 3 && src == volumeTarget()) {
                ++audioNews_.count;
                audioNews_.refused = false;
                audioNews_.status = frame[2];
            }
            return false;

        // <Feature Abort> - never answer one with another. One aimed at
        // <Give Audio Status> is the set saying it will not report, which the
        // readout needs to hear as much as it needs a level.
        case 0x00:
            // [opcode][reason], so exactly two operands, and only believed from
            // the device the question went to.
            if (frame.size() != 4 || src != volumeTarget()) return false;
            if (frame[2] == 0x71) {
                ++audioNews_.count;
                audioNews_.refused = true;
            } else if (frame[2] == 0x73) {
                // This set will not take an absolute level, so the slider that
                // sends them has nothing to offer and the tray takes it away.
                // Measured on a Polk Mini AX, which answers <Give Audio Status>
                // perfectly and aborts this with reason 0x00, unrecognised
                // opcode - the readout is honest and the command is ignored, so
                // the thumb sprang back to the real level every time. Counted
                // as news as well, so that whoever is watching for an answer
                // wakes up instead of waiting out its timeout.
                ++audioNews_.count;
                audioNews_.levelRefused = true;
            }
            return false;

        // Understood, and nothing to say back.
        //
        // The specification asks for <Feature Abort> in reply to a directly
        // addressed message a device does not understand - which is not the
        // same as one it has nothing to answer. All of these are statements
        // that are acted on elsewhere, or answers to questions asked from
        // somewhere else in this file, and aborting one says the opposite of
        // the truth to whichever device sent it.
        //
        // They are listed because a broadcast whose destination nibble arrives
        // corrupted reads as a message addressed to this device, and two of
        // those - a broadcast <Active Source> read as directed - had this
        // device answering <Feature Abort> to its own kind. The frames are the
        // adapter's fault and cannot be prevented here; replying to them with
        // nonsense was ours.
        case 0x32:     // <Set Menu Language>
        case 0x36:     // <Standby>
        case 0x47:     // <Set OSD Name>, another device naming itself
        case 0x64:     // <Set OSD String>
        case 0x72:     // <Set System Audio Mode>
        case 0x7E:     // <System Audio Mode Status>
        case 0x80:     // <Routing Change>
        case 0x81:     // <Routing Information>
        case 0x82:     // <Active Source>
        case 0x84:     // <Report Physical Address>
        case 0x85:     // <Request Active Source>, answered above as a broadcast
        case 0x86:     // <Set Stream Path>, acted on above
        case 0x87:     // <Device Vendor ID>
        case 0x8E:     // <Menu Status>
        case 0x90:     // <Report Power Status>
        case 0x9D:     // <Inactive Source>
        case 0x9E:     // <CEC Version>
            return false;
        default:
            // The spec requires a directly addressed message that is not
            // understood to be answered with <Feature Abort>, and silence is
            // how a device comes to look broken: a TV waiting on a reply it
            // never gets can decide this device is not worth listing, which is
            // the difference between the PC's name on the input and "HDMI 2".
            // Broadcasts are excluded above, where the address is checked.
            SendFrame({toSrc, 0x00, frame[1], 0x00});   // unrecognised opcode
            break;
    }
    return true;
}

int CecDevice::ServicePending(int maxFrames) {
    // Non-blocking by design. This is what a timer calls, so it takes whatever
    // has already arrived and returns; it must never sit waiting on a bus that
    // may have nothing to say for hours.
    //
    // The frame limit is a latency budget, not a queue depth. Whoever calls
    // this holds the CEC lock for its duration, so answering without limit
    // could stall a keypress for seconds. Anything left waits for the next
    // tick, a few hundred milliseconds away.
    int answered = 0;
    std::vector<uint8_t> frame;
    for (int i = 0; i < maxFrames && ReceiveFrame(frame); ++i) {
        // Spacing goes before the reply rather than after it, so a single
        // answer - which is the normal case - releases the lock immediately
        // instead of sleeping on it for no reason.
        if (answered > 0) Sleep(kInterFrameDelayMs);
        const bool replied = AnswerQuery(frame);
        if (replied) ++answered;
        if (onFrame_) onFrame_(frame, replied);
    }
    return answered;
}

int CecDevice::ServiceQueries(int durationMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(durationMs);
    int answered = 0;
    std::vector<uint8_t> frame;

    // As a difference, so a deadline past the tick count's wrap is still in the
    // future rather than already gone.
    while (static_cast<LONG>(GetTickCount() - deadline) < 0) {
        if (!ReceiveFrame(frame)) {
            Sleep(80);
            continue;
        }
        const bool replied = AnswerQuery(frame);
        if (replied) {
            ++answered;
            Sleep(kInterFrameDelayMs);
        }
        if (onFrame_) onFrame_(frame, replied);
    }
    return answered;
}

int CecDevice::DrainRx(int maxFrames) {
    int dropped = 0;
    std::vector<uint8_t> frame;
    while (dropped < maxFrames && ReceiveFrame(frame)) ++dropped;
    return dropped;
}

bool CecDevice::SendImageViewOn() {
    if (!SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x04})) {
        return false;
    }
    Sleep(kInterFrameDelayMs);

    // <Image View On> powers the panel; <Active Source> is what actually makes
    // the TV switch to this input.
    return SendActiveSource();
}

// How long a claim is watched to see whether something takes the screen back.
//
// Measured from a log: this PC claimed at 17:32:11 and the Chromecast claimed
// again at 17:32:12, with the television following it a moment later. A second
// covers that with room, and the wait costs nothing the person can see - the
// picture has already changed by the time this starts.
constexpr int kContestWatchMs = 1500;

bool CecDevice::WakeAndClaimInput(bool watchForContest) {
    // One Touch Play first, before any question is asked.
    //
    // This used to ask <Give Device Power Status> and wait up to 1.2s for the
    // answer before sending anything at all - a wait in front of the one frame
    // the person pressing a button is waiting for. The answer only decides
    // whether the spaced repeats below are needed, which is a question about
    // what happens afterwards, so it is asked afterwards. On a set that answers
    // in 100ms this saves that much; on one that never answers it saves the
    // whole 1.2 seconds, and that is the set most in need of the frame.
    if (!SendImageViewOn()) return false;

    const PowerStatus before = QueryPowerStatus(1200);
    if (before == PowerStatus::On) {
        if (watchForContest) WatchForContest();
        return true;
    }

    // Standby, or a set that would not say - either way, assume it is waking.
    //
    // <Active Source> is a broadcast statement about this device, and unlike
    // <Standby> it is idempotent: repeating it costs a frame and risks nothing.
    // Two more, spaced, covers a television that takes anywhere from one to
    // four seconds to finish coming up.
    for (int attempt = 0; attempt < 2; ++attempt) {
        Sleep(1500);
        SendActiveSource();
    }
    if (watchForContest) WatchForContest();
    return true;
}

// Claims the screen once more if something took it back. See kContestWatchMs.
void CecDevice::WatchForContest() {
    const uint16_t mine = physicalAddress_;
    const DWORD until = GetTickCount() + kContestWatchMs;
    while (static_cast<LONG>(until - GetTickCount()) > 0) {
        std::vector<uint8_t> frame;
        while (ReceiveFrame(frame)) {
            // Answered as usual: a television that has just seen this device
            // claim the screen is about to ask it who it is.
            const bool replied = AnswerQuery(frame);
            if (onFrame_) onFrame_(frame, replied);
        }
        // Another device has said it has the screen, which means the claim just
        // made did not stick. Only a statement counts here: a television merely
        // asking some other device to take it goes to requestedPath_ and is not
        // yet a screen anybody holds.
        if (activePathKnown_ && activePath_ != mine) {
            SendActiveSource();
            return;
        }
        Sleep(30);
    }
}

bool CecDevice::SendBroadcast(const std::vector<uint8_t>& frame) {
    // A broadcast is refused only when a device on the bus pushes back, and the
    // commonest reason is one that has stopped collecting its messages: an
    // adapter left powered by a sleeping PC fills up and then has to refuse
    // everything. That is worth a second and third try, since it also happens
    // transiently while a set is waking, and one lost broadcast is expensive -
    // <Report Physical Address> and <Active Source> are broadcasts, and a TV
    // that misses them will not take power commands from this device at all.
    //
    // Counted as well as retried, because a bus that refuses every broadcast is
    // something the person can act on - unplug the offender - and nothing else
    // in the log says so.
    for (int attempt = 0; attempt < 3; ++attempt) {
        TxResult tx;
        if (SendFrame(frame, &tx)) {
            broadcastsRefused_ = 0;
            return true;
        }
        if (!tx.nacked) return false;   // not a refusal; retrying will not help
        ++broadcastsRefused_;
        Sleep(kInterFrameDelayMs);
    }
    return false;
}

bool CecDevice::SendFrame(const std::vector<uint8_t>& frame, TxResult* result) {
    TxResult local;
    TxResult& tx = result ? *result : local;
    tx = TxResult();

    if (frame.empty() || frame.size() > 16) {
        lastError_ = "CEC frame must be 1..16 bytes";
        return false;
    }

    // Clear stale transmit status so anything observed afterwards belongs to
    // this send - and only the transmit status.
    //
    // This used to write 0xFF, which also cleared RX_MESSAGE_INFO_VALID, the
    // bit that says a received frame is waiting. Any frame that arrived just
    // before a send was silently thrown away. It went unnoticed while volume
    // reads held the CEC lock until their answer came, because no send could
    // run in between; once the answer was left to arrive on its own, rapid
    // clicks cleared it away before anyone read it, and the readout never
    // updated. A TV's own questions could always have been lost the same way.
    WriteReg(kCecTunnelingIrqFlags, {kIrqTxMask});

    if (!WriteReg(kCecTxMessageBuffer, frame)) return false;
    const uint8_t trigger =
        static_cast<uint8_t>(kCecTxSendWithRetries | (frame.size() - 1));
    if (!WriteReg(kCecTxMessageInfo, {trigger})) return false;
    tx.accepted = true;

    for (int elapsed = 0; elapsed <= 1000; elapsed += 25) {
        Sleep(25);
        std::vector<uint8_t> status;
        if (!backend_.ReadDpcd(port_, kCecTxMessageInfo, 2, status)) break;
        const uint8_t txInfo = status[0];
        // Only the transmit bits decide whether this send is over. A frame
        // arriving mid-send sets the receive bit, and reading the whole byte
        // took that for the end of transmission: the send reported success
        // while the frame was still going out, and the next one - a click's
        // release, say - was written over it.
        const uint8_t flags = static_cast<uint8_t>(status[1] & kIrqTxMask);
        if (!flags && (txInfo & kCecTxSendBit)) continue;

        tx.irqFlags = flags;
        tx.sent = (flags & kIrqTxMessageSent) != 0;
        tx.nacked = (flags & (kIrqTxAddressNackError | kIrqTxDataNackError)) != 0;
        tx.lineError = (flags & kIrqTxLineError) != 0;
        if (onSend_) onSend_(frame, tx);
        if (tx.nacked) {
            lastError_ = "CEC frame was not acknowledged";
            return false;
        }
        if (tx.lineError) {
            lastError_ = "CEC line error";
            return false;
        }
        return true;
    }

    lastError_ = "adapter never reported transmit status";
    if (onSend_) onSend_(frame, tx);
    return false;
}

bool CecDevice::ReceiveFrame(std::vector<uint8_t>& frame) {
    frame.clear();

    std::vector<uint8_t> flags;
    if (!backend_.ReadDpcd(port_, kCecTunnelingIrqFlags, 1, flags)) {
        lastError_ = backend_.LastError();
        return false;
    }
    if (!(flags[0] & kIrqRxMessageInfoValid)) return false;

    // Counted, not acted on, and deliberately so.
    //
    // The adapter raises this when frames arrived faster than they were
    // collected, and it is the likeliest explanation for the frames seen with
    // one message's head and another's body: a buffer holding two frames' bytes
    // at once. Whether discarding the frame in hand would help or would throw
    // away good ones is not known, and a device that stops answering is the
    // worse failure - so for now this only says that it happened, and the shape
    // checks in NoteRouting keep the damage out.
    if (flags[0] & kIrqRxMessageOverflow) ++receiveOverflows_;

    std::vector<uint8_t> info;
    if (!backend_.ReadDpcd(port_, kCecRxMessageInfo, 1, info)) return false;
    const uint32_t length = static_cast<uint32_t>((info[0] & 0x0F) + 1);
    if (!backend_.ReadDpcd(port_, kCecRxMessageBuffer, length, frame)) return false;

    // Write-1-to-clear so the adapter advances to the next queued frame.
    WriteReg(kCecTunnelingIrqFlags,
             {kIrqRxMessageInfoValid | kIrqRxMessageOverflow});

    // A frame sent from this device's own logical address, which this device
    // did not send: nothing else can explain it but another device holding the
    // same address. It happens easily with two PCs on one television, because a
    // machine that is asleep answers no poll and so leaves its address looking
    // free. The damage is quiet and total - the television answers questions
    // addressed to the shared address, so volume and power queries work, while
    // every broadcast comes back unacknowledged and <Standby> is ignored - so
    // it is noticed here and acted on by the caller.
    if (!frame.empty() && static_cast<uint8_t>(frame[0] >> 4) == logicalAddress_) {
        addressClash_ = true;
    }
    // Here rather than where frames are answered, because several paths read
    // frames without answering them, and a routing change must not slip past
    // just because a power query happened to be the one listening.
    NoteRouting(frame);
    return true;
}

// Whether an opcode is one the specification only ever sends to everybody.
//
// These five are defined as broadcast messages: <Routing Change>,
// <Routing Information>, <Active Source>, <Request Active Source> and
// <Set Stream Path>. There is no such thing as a legitimate copy of one
// addressed to a single device, so a copy that arrives that way did not come off
// the wire that way - and that is the one test that catches a corrupted frame
// whose operand count happens to look right.
//
// It is not a theory. A log caught <Set Stream Path> 4.3.0.0 arriving addressed
// to this device: two operands, the correct shape, and an input that does not
// exist. Believed, it would have left this PC thinking something else was on
// screen, which is enough to stop the television being switched off with the
// display. The same log has seven more of them, every one addressed rather than
// broadcast, and every one carrying the payload of a different message.
static bool IsBroadcastOnly(uint8_t opcode) {
    switch (opcode) {
        case 0x80:   // <Routing Change>
        case 0x81:   // <Routing Information>
        case 0x82:   // <Active Source>
        case 0x85:   // <Request Active Source>
        case 0x86:   // <Set Stream Path>
            return true;
        default:
            return false;
    }
}

void CecDevice::NoteRouting(const std::vector<uint8_t>& frame) {
    // Published on the way out, whatever this frame turned out to be. The
    // function below has a return in almost every case of its switch, so one
    // guard object is both shorter and safer than a publish before each of
    // them, and a frame that changed nothing simply republishes the same
    // answer, which costs an atomic store. See set_screenPathSink.
    struct PublishOnExit {
        CecDevice* device;
        ~PublishOnExit() { device->PublishScreenPath(); }
    } publishOnExit{this};

    // Dropped before anything sees it, including the list of what is on the
    // bus: a frame this wrong may have a corrupted source as well, so it is no
    // evidence about who sent it either.
    if (frame.size() >= 2 &&
        static_cast<uint8_t>(frame[0] & 0x0F) != kLogicalAddressBroadcast &&
        IsBroadcastOnly(frame[1])) {
        ++malformedFrames_;
        return;
    }
    // Everything a device says about itself, passed to whoever is keeping a
    // list of what is on this television. Devices volunteer their address when
    // they are plugged in or powered on, and their name and vendor when asked,
    // so a list built from this is right without anything being polled.
    //
    // Any frame at all counts as a sign of life from its sender, which is what
    // tells a device that is there from one that has been switched off.
    if (onDevice_ && !frame.empty()) onDevice_(frame, false);

    // <Feature Abort> names the message being refused, which is the difference
    // between a television that did not hear a command and one that heard it
    // and will not do it. Worth knowing precisely, because the answer to the
    // first is to send it again and the answer to the second is to stop and try
    // something else. The development TCL refuses <Standby> this way - reason
    // 00, "unrecognized opcode" - while answering everything else normally.
    if (frame.size() >= 3 && frame[1] == 0x00) {
        lastAbortOpcode_ = frame[2];
        lastAbortTick_ = GetTickCount();
    }
    if (onRouting_ && frame.size() >= 2) {
        switch (frame[1]) {
            case 0x36:     // <Standby>
            case 0x80:     // <Routing Change>
            case 0x81:     // <Routing Information>
            case 0x82:     // <Active Source>
            case 0x85:     // <Request Active Source>
            case 0x86:     // <Set Stream Path>
            case 0x9D:     // <Inactive Source>
                onRouting_(frame, false);
                break;
            default:
                break;
        }
    }
    // A bare polling message carries no opcode, so there is nothing below for
    // it to be.
    if (frame.size() < 2) return;

    // What the television itself named, kept apart from everything else. See
    // TvNamedPath. <Set Stream Path> counts here, unlike anywhere else: from
    // the set it is the set saying which input it is moving to, where from a
    // source it is only a request.
    if (static_cast<uint8_t>(frame[0] >> 4) == kLogicalAddressTv) {
        if ((frame[1] == 0x82 || frame[1] == 0x81 || frame[1] == 0x86) &&
            frame.size() == 4) {
            tvPath_ = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
            tvPathTick_ = GetTickCount() | 1;
        } else if (frame[1] == 0x80 && frame.size() == 6) {
            tvPath_ = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
            tvPathTick_ = GetTickCount() | 1;
        }
    }

    // Exact lengths, not minimums, for the four frames that decide which input
    // is on screen.
    //
    // A frame carrying more operands than its opcode allows did not come off
    // the wire that way. One was caught in a log: a <Request Active Source>,
    // which has no operands at all, arriving with ten - and those ten bytes
    // were the tail of a vendor message sent a moment earlier, so the receive
    // buffer had been read across two frames. Harmless under that opcode, and
    // not harmless at all under <Active Source>, where the invented operands
    // would have read as a physical address and moved the screen somewhere
    // nobody is. Anything the wrong shape is counted and ignored.
    const size_t operands = frame.size() - 2;
    switch (frame[1]) {
        case 0x82:     // <Active Source> [path]
        case 0x81:     // <Routing Information> [path]
            if (operands != 2) break;
            activePath_ = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
            activePathKnown_ = true;
            claimedTick_ = GetTickCount();
            return;
        case 0x80:     // <Routing Change> [from][to]
            if (operands != 4) break;
            activePath_ = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
            activePathKnown_ = true;
            claimedTick_ = GetTickCount();
            return;
        case 0x86:     // <Set Stream Path> [path]
            // A request, kept apart from what is on screen. A television
            // repeats it, and letting a repeat move the input this device
            // believes is on screen is what put the dot in the menu beside an
            // input nothing had claimed - and kept it there, because no later
            // frame contradicted it.
            if (operands != 2) break;
            requestedPath_ = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
            requestedKnown_ = true;
            requestedTick_ = GetTickCount();
            return;
        case 0x85:     // <Request Active Source>, which takes none
            if (operands) break;
            return;
        default:
            return;
    }
    ++malformedFrames_;
}

bool CecDevice::SendKeepAlive(KeepAlive signal) {
    switch (signal) {
        case KeepAlive::kF5Key:
            return SendUserControl(0x75, kLogicalAddressTv);
        case KeepAlive::kUnassignedKey:
            return SendUserControl(0x77, kLogicalAddressTv);
        case KeepAlive::kImageViewOn:
            return SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x04});
        case KeepAlive::kActiveSource:
            return SendActiveSource();
    }
    return false;
}

PowerStatus CecDevice::WaitForStandby(int timeoutMs) {
    const DWORD started = GetTickCount();
    const DWORD deadline = started + static_cast<DWORD>(timeoutMs);
    PowerStatus last = PowerStatus::Unknown;

    // Ask once, then listen. Calling QueryPowerStatus in a loop re-sent
    // <Give Device Power Status> on every iteration - roughly ten frames for a
    // ten second wait, each one a full send plus its own transmit-status poll.
    // One question and a patient ear is faster here and much quieter on a bus
    // the TV is also using.
    //
    // Frames are read before the next question goes out, so an answer is
    // always read against the question it answers: replies take a fraction of
    // a second and the questions are two seconds apart.
    bool asked = false;
    DWORD lastAsk = 0;
    while (true) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (!IsTvPowerReport(msg)) continue;
            const auto reported = static_cast<PowerStatus>(msg[2]);
            // The spec provides a transitional state for exactly this, and a TV
            // reporting it has committed to powering down. Believed whenever
            // it comes, since acting on it never sends anything.
            if (reported == PowerStatus::Standby ||
                reported == PowerStatus::TransitioningToStandby) {
                return reported;
            }
            // Any other answer only counts if the question was asked once the
            // set has had time to act on the command (hard rule 1). An earlier
            // "on" is not evidence that the command failed, it is a set still
            // on its way down: the TCL confirms standby about three seconds
            // after <Standby>, and this asks at nought and two. Every caller
            // resends or presses a key on "on", so a premature one is exactly
            // the toggle the rule forbids - and the suspend path, whose wait is
            // two and a half seconds, did it on four of eight sleeps on the
            // desktop, the TV staying off each time only because it was
            // already powering down. Too early to count is simply not counted,
            // which leaves the answer unknown and so inconclusive.
            if (asked && lastAsk - started >= kStandbyTransitionMs) {
                last = reported;
            }
        }

        // As a difference, so a deadline that has wrapped past zero is still
        // in the future. Compared directly, a wait begun in the last few
        // seconds before GetTickCount wraps - every 49.7 days of uptime -
        // ended at once and reported whatever had been heard so far.
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) return last;

        if (!asked || GetTickCount() - lastAsk >= 2000) {
            lastAsk = GetTickCount();
            asked = true;
            SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x8F});
        }
        Sleep(150);
    }
}

bool CecDevice::AskAudioStatus() {
    return SendFrame({Header(logicalAddress_, volumeTarget()), 0x71});
}

bool CecDevice::SendSetAudioVolumeLevel(uint8_t level) {
    if (level > 100) level = 100;
    // Retried on a refusal, the same as a volume press and for the same reason:
    // the TCL refuses frames while it is answering a volume question. A knob's
    // last level lost that way left the TV a few steps short of the number on
    // screen. A level is absolute, so sending it twice cannot overshoot.
    for (int attempt = 0;; ++attempt) {
        TxResult tx;
        if (SendFrame({Header(logicalAddress_, volumeTarget()), 0x73, level}, &tx)) {
            return true;
        }
        if (!tx.nacked || attempt >= kPressRetries) return false;
        Sleep(kPressRetryMs);
    }
}

uint8_t CecDevice::QueryCecVersion(int timeoutMs) {
    // <Get CEC Version> -> <CEC Version>
    if (!SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x9F})) {
        return 0;
    }
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 100) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (msg.size() >= 3 && msg[1] == 0x9E) return msg[2];
        }
        Sleep(100);
    }
    return 0;
}

bool CecDevice::QueryAudioStatus(uint8_t* status, int timeoutMs, bool* refused) {
    if (refused) *refused = false;
    // <Give Audio Status>, addressed to whoever owns volume on this bus.
    //
    // This exists to answer a question no amount of sending volume can: a TV
    // that ignores <User Control Pressed> and one that acts on it both ACK the
    // frame identically, so a working send proves nothing. A set that answers
    // this implements the audio feature set and should be controllable; one
    // that returns <Feature Abort>, or nothing, does not - and no amount of
    // trying harder on our side will change that.
    const uint8_t target = volumeTarget();
    if (!SendFrame({Header(logicalAddress_, target), 0x71})) return false;

    // Checked every 20ms, and once more at the deadline, rather than every
    // 100ms. The answer lands a little over 100ms after the question, so the
    // coarse step noticed it up to 100ms late - and one arriving just after
    // the last check was not noticed at all, which read as a set with nothing
    // to say. That is what made the slider vanish at random. Each check is a
    // single register read, so the finer step costs nothing that matters.
    const DWORD start = GetTickCount();
    for (;;) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (msg.size() >= 3 && msg[1] == 0x7A) {   // <Report Audio Status>
                // Read straight off the buffer rather than through
                // AnswerQuery, so this synchronous path records no news. It is
                // the diagnostic; the live readout asks with AskAudioStatus
                // and never waits here.
                if (status) *status = msg[2];
                return true;
            }
            if (msg.size() >= 3 && msg[1] == 0x00 && msg[2] == 0x71) {
                lastError_ = "the device answered <Feature Abort>";
                if (refused) *refused = true;
                return false;
            }
        }
        if (GetTickCount() - start >= static_cast<DWORD>(timeoutMs)) break;
        Sleep(20);
    }
    lastError_ = "no answer";
    return false;
}

PowerStatus CecDevice::QueryPowerStatus(int timeoutMs) {
    // <Give Device Power Status>
    if (!SendFrame({Header(logicalAddress_, kLogicalAddressTv), 0x8F})) {
        return PowerStatus::Unknown;
    }

    // Check before sleeping: a reply that is already queued should not cost a
    // full poll interval to notice.
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 100) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (IsTvPowerReport(msg)) {
                return static_cast<PowerStatus>(msg[2]);
            }
        }
        Sleep(100);
    }
    return PowerStatus::Unknown;
}

void CecDevice::NotePeer(const std::vector<uint8_t>& frame) {
    // Both hello and its answer carry where the sender is and what it calls
    // itself, so learning about another PC costs no further questions - which
    // is the whole point. Polling and asking took seconds, and seconds are what
    // stand between opening the menu and finding the PC that has just woken
    // missing from it.
    if (!onAppPeerFound_ || frame.size() < 9) return;
    const uint8_t src = static_cast<uint8_t>(frame[0] >> 4);
    const uint16_t where = static_cast<uint16_t>((frame[7] << 8) | frame[8]);
    std::string name;
    for (size_t i = 9; i < frame.size() && name.size() < 14; ++i) {
        if (frame[i] >= 0x20 && frame[i] <= 0x7E) {
            name.push_back(static_cast<char>(frame[i]));
        }
    }
    // Whether this is a Here I Am, the answer to a hello, which is what lets
    // the service confirm an address before believing it. See its NotePeer.
    onAppPeerFound_(src, where, name, frame[6] == kAppHereIAm);
}

bool CecDevice::SendHereIAm(uint8_t target) {
    std::vector<uint8_t> frame = {Header(logicalAddress_, target),
                                  0xA0,
                                  kAppVendorId[0],
                                  kAppVendorId[1],
                                  kAppVendorId[2],
                                  kAppProtocolVersion,
                                  kAppHereIAm,
                                  static_cast<uint8_t>(physicalAddress_ >> 8),
                                  static_cast<uint8_t>(physicalAddress_ & 0xFF)};
    const std::string& name = osdName_.empty() ? DefaultOsdName() : osdName_;
    for (size_t i = 0; i < name.size() && frame.size() < 16; ++i) {
        frame.push_back(static_cast<uint8_t>(name[i]));
    }
    return target == kLogicalAddressBroadcast ? SendBroadcast(frame)
                                              : SendFrame(frame);
}

bool CecDevice::SayHello(uint8_t target) {
    std::vector<uint8_t> frame = {Header(logicalAddress_, target),
                                  0xA0,
                                  kAppVendorId[0],
                                  kAppVendorId[1],
                                  kAppVendorId[2],
                                  kAppProtocolVersion,
                                  kAppHello,
                                  static_cast<uint8_t>(physicalAddress_ >> 8),
                                  static_cast<uint8_t>(physicalAddress_ & 0xFF)};
    const std::string& name = osdName_.empty() ? DefaultOsdName() : osdName_;
    for (size_t i = 0; i < name.size() && frame.size() < 16; ++i) {
        frame.push_back(static_cast<uint8_t>(name[i]));
    }
    return target == kLogicalAddressBroadcast ? SendBroadcast(frame)
                                              : SendFrame(frame);
}

bool CecDevice::SendAppMessage(uint8_t target, uint8_t verb) {
    return SendFrame({Header(logicalAddress_, target), 0xA0, kAppVendorId[0],
                      kAppVendorId[1], kAppVendorId[2], kAppProtocolVersion,
                      verb});
}

PollAnswer CecDevice::Poll(uint8_t address) {
    TxResult tx;
    // A polling message is a lone header byte. An ACK means a device holds that
    // address; this device's own is excluded by the caller, since the adapter
    // answers for it.
    //
    // Only a reported status is an answer. This used to count anything but a
    // NACK as present, so a poll whose transmit status never came back - an
    // adapter that has stopped answering, which a cable flap can do for a
    // minute or more - read as a device. In a bus scan that invented a
    // neighbour at every address, the phantom "15.15.15.15=?[Device]" of the
    // field logs, and each one was then asked four more questions, every one
    // waiting out its timeout with the CEC lock held.
    if (SendFrame({Header(logicalAddress_, address)}, &tx)) {
        return PollAnswer::Present;
    }
    return tx.nacked ? PollAnswer::Absent : PollAnswer::Unknown;
}

std::string CecDevice::AskName(uint8_t address, int timeoutMs) {
    if (!SendFrame({Header(logicalAddress_, address), 0x46})) return {};
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            if (msg.size() >= 3 && msg[1] == 0x47 &&
                static_cast<uint8_t>(msg[0] >> 4) == address) {
                std::string name;
                for (size_t i = 2; i < msg.size() && name.size() < 14; ++i) {
                    if (msg[i] >= 0x20 && msg[i] <= 0x7E) {
                        name.push_back(static_cast<char>(msg[i]));
                    }
                }
                return name;
            }
            AnswerQuery(msg);
        }
        Sleep(50);
    }
    return {};
}

uint16_t CecDevice::AskWhere(uint8_t address, int timeoutMs) {
    if (!SendFrame({Header(logicalAddress_, address), 0x83})) return 0xFFFF;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            // At its exact length, header, opcode, two bytes of address and
            // the device type, like every other reader of this message: a
            // floor let a torn frame through (see hard rules, section 9).
            if (msg.size() == 5 && msg[1] == 0x84 &&
                static_cast<uint8_t>(msg[0] >> 4) == address) {
                return static_cast<uint16_t>((msg[2] << 8) | msg[3]);
            }
            AnswerQuery(msg);
        }
        Sleep(50);
    }
    return 0xFFFF;
}

bool CecDevice::AskVendor(uint8_t address, int timeoutMs) {
    if (!SendFrame({Header(logicalAddress_, address), 0x8C})) return false;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            // The answer is a broadcast, and NoteRouting has already passed it
            // to whoever is keeping the list, so there is nothing to return but
            // whether one came.
            if (msg.size() >= 5 && msg[1] == 0x87 &&
                static_cast<uint8_t>(msg[0] >> 4) == address) {
                return true;
            }
            AnswerQuery(msg);
        }
        Sleep(50);
    }
    return false;
}

bool CecDevice::AskIfPeer(uint8_t address, int timeoutMs) {
    peerSeen_ = 0xFF;
    if (!SayHello(address)) return false;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            AnswerQuery(msg);
            if (peerSeen_ == address) return true;
        }
        Sleep(50);
    }
    return false;
}

bool CecDevice::AskPeerToTakeScreen(uint8_t address, int timeoutMs) {
    // Three tries, because a lost frame here looks exactly like a refusal to
    // the person who pressed the menu item, and the cost of one more frame on
    // an idle bus is nothing.
    peerAsked_ = address;
    for (int attempt = 0; attempt < 3; ++attempt) {
        peerAnswer_ = 0;
        if (SendAppMessage(address, kAppTakeScreen)) {
            for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
                std::vector<uint8_t> msg;
                while (ReceiveFrame(msg)) {
                    AnswerQuery(msg);
                    if (peerAnswer_ == kAppTaken) return true;
                    if (peerAnswer_ == kAppRefused) return false;
                }
                Sleep(50);
            }
        }
    }
    return false;
}

void CecDevice::WaitForTvRouteQuiet(int quietMs, int timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        std::vector<uint8_t> frame;
        while (ReceiveFrame(frame)) {
            const bool replied = AnswerQuery(frame);
            if (onFrame_) onFrame_(frame, replied);
        }
        if (!tvPathTick_) return;   // this set says nothing about routing
        if (static_cast<LONG>(GetTickCount() - tvPathTick_) >= quietMs) return;
        if (static_cast<LONG>(GetTickCount() - start) >= timeoutMs) return;
        Sleep(60);
    }
}

bool CecDevice::AwaitActivePath(uint16_t path, int timeoutMs) {
    // Only a claim that arrived after this started counts.
    //
    // This used to accept a match outright, before waiting for anything, which
    // meant a claim from any time in the past could confirm a request just
    // sent - so asking for the input already on screen reported success without
    // a frame having been exchanged, and a second attempt inside one switch
    // confirmed whatever the first had recorded rather than anything new. A
    // device asked to take a screen it already holds re-announces, which is
    // exactly the frame this is here to hear.
    const DWORD start = GetTickCount();
    const auto claimedSinceStart = [&] {
        return activePathKnown_ && activePath_ == path &&
               static_cast<LONG>(claimedTick_ - start) >= 0;
    };
    for (;;) {
        std::vector<uint8_t> frame;
        while (ReceiveFrame(frame)) {
            // Answered, not merely read. A switch is the moment a television is
            // most likely to ask this PC who it is, and a device that goes quiet
            // while a set is enumerating it is one the set may drop.
            const bool replied = AnswerQuery(frame);
            if (onFrame_) onFrame_(frame, replied);
            if (claimedSinceStart()) return true;
        }
        if (GetTickCount() - start >= static_cast<DWORD>(timeoutMs)) return false;
        Sleep(30);
    }
}

bool CecDevice::AskWhoIsOnScreen(int timeoutMs) {
    // <Request Active Source>, which the device on screen answers with
    // <Active Source> and its physical address. Asking is the only way to be
    // sure: the routing messages this device otherwise relies on are broadcast
    // once, when something changes, so a service that started afterwards knows
    // nothing - and then guesses, which on a bus with two PCs is how a set ends
    // up being told to switch off by a device it is not showing.
    //
    // The answer arrives through NoteRouting like any other routing frame. A
    // set that answers nothing leaves what was known before in place, which is
    // the single-device case and costs the wait and nothing else.
    activePathAnswered_ = false;
    if (!SendBroadcast({Header(logicalAddress_, kLogicalAddressBroadcast), 0x85})) {
        return false;
    }
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        std::vector<uint8_t> msg;
        while (ReceiveFrame(msg)) {
            // Answered by <Active Source>, and by <Routing Information>, which
            // is what some sets send instead. Only in the shape NoteRouting
            // believes - four bytes, broadcast - because that is where the
            // answer is actually recorded. A torn frame with either opcode used
            // to end the wait early and call it answered, and the caller then
            // decided from what was known before the real answer arrived.
            if (msg.size() == 4 &&
                static_cast<uint8_t>(msg[0] & 0x0F) == kLogicalAddressBroadcast &&
                (msg[1] == 0x82 || msg[1] == 0x81)) {
                activePathAnswered_ = true;
                return true;
            }
        }
        Sleep(50);
    }
    return false;
}

bool FindCecPort(std::vector<std::unique_ptr<AuxBackend>>& backends,
                 AuxBackend** foundBackend, AuxPort* foundPort,
                 std::string* error) {
    std::string lastProblem = "no GPU backend reported a CEC-capable port";

    for (auto& backend : backends) {
        if (!backend->Open()) {
            // Absent vendor driver is normal, not a failure.
            continue;
        }
        std::vector<AuxPort> ports;
        if (!backend->EnumeratePorts(ports)) {
            lastProblem = backend->LastError();
            continue;
        }
        for (const auto& port : ports) {
            CecDevice device(*backend, port);
            bool capable = false;
            if (!device.IsCecCapable(capable)) continue;
            if (!capable) continue;

            *foundBackend = backend.get();
            *foundPort = port;
            return true;
        }
    }
    if (error) *error = lastProblem;
    return false;
}

}  // namespace cec
