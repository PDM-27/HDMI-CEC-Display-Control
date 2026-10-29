// CecDevice.h - HDMI-CEC over DisplayPort AUX tunneling.
//
// Owns one AuxPort and speaks CEC over it. This is the whole protocol surface
// the service and the tray app need; neither should touch DPCD directly.
//
// The critical detail encoded here: enabling tunneling and claiming a logical address is NOT enough to make a TV
// obey power commands. The device must also announce itself on the bus, or
// every message will be transmitted and acknowledged and then ignored.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "AuxBackend.h"
#include "Edid.h"

namespace cec {

// DisplayPort CEC-tunneling DPCD register map (VESA DP 1.3+, matching the
// names used by Linux drm_dp_cec.c).
constexpr uint32_t kCecTunnelingCapability = 0x3000;
constexpr uint32_t kCecTunnelingControl    = 0x3001;
constexpr uint32_t kCecRxMessageInfo       = 0x3002;
constexpr uint32_t kCecTxMessageInfo       = 0x3003;
constexpr uint32_t kCecTunnelingIrqFlags   = 0x3004;
constexpr uint32_t kCecLogicalAddressMask  = 0x300E;  // 2 bytes
constexpr uint32_t kCecRxMessageBuffer     = 0x3010;
constexpr uint32_t kCecTxMessageBuffer     = 0x3020;

// CEC_TUNNELING_IRQ_FLAGS bits. Write-1-to-clear. The only direct evidence of
// what the adapter's transmitter actually did, since every DPCD write in a send
// reports success whether or not the frame is honoured.
constexpr uint8_t kIrqRxMessageInfoValid = 1 << 0;
constexpr uint8_t kIrqRxMessageOverflow  = 1 << 1;
constexpr uint8_t kIrqTxMessageSent      = 1 << 4;
constexpr uint8_t kIrqTxLineError        = 1 << 5;
constexpr uint8_t kIrqTxAddressNackError = 1 << 6;
constexpr uint8_t kIrqTxDataNackError    = 1 << 7;

// The transmit half of the register. The receive half shares it, and a send
// must neither clear nor read those bits: see SendFrame.
constexpr uint8_t kIrqTxMask = kIrqTxMessageSent | kIrqTxLineError |
                               kIrqTxAddressNackError | kIrqTxDataNackError;

// CEC_TX_MESSAGE_INFO: bit 7 = SEND, bits 6:4 = retry count, bits 3:0 =
// length - 1. 0xB0 is "send with 3 retries", which is exactly the
// 0xB0 | (length-1) pattern captured from the Linux driver.
constexpr uint8_t kCecTxSendBit         = 0x80;
constexpr uint8_t kCecTxSendWithRetries = 0xB0;

constexpr uint8_t kLogicalAddressTv        = 0;
constexpr uint8_t kLogicalAddressBroadcast = 15;

// A soundbar or receiver doing System Audio Control sits here. Volume keys
// addressed to it act on it directly; addressed to the TV they arrive at the
// same place only after the TV has decided to pass them on, which is a second
// hop through the slowest device on the bus.
constexpr uint8_t kLogicalAddressAudioSystem = 5;

// The three addresses CEC reserves for playback devices. A source device is
// expected to poll them and take the first that is free, rather than assuming
// one - a Fire Stick, games console or Blu-ray player will already be sitting
// on Playback 1 in most real setups.
constexpr uint8_t kLogicalAddressPlayback1 = 4;
constexpr uint8_t kLogicalAddressPlayback2 = 8;
constexpr uint8_t kLogicalAddressPlayback3 = 11;

// The one address CEC sets aside for a device that cannot have a proper one.
//
// A playback device has exactly three addresses, and three is not many: a bus
// with an Apple TV on 11 and two PCs running this app has none left over, which
// is the Vizio bus in the field logs. What that looks like is not a tidy
// failure. The two machines take turns on 4 and 8, each move is a window where
// both believe they hold the same address, and a television ignores commands
// from an address it thinks belongs to a different input - so a log full of
// "CEC address 4 is shared with another device and no free one was found" came
// with real losses: a tv-on that was not acknowledged, and a standby that had
// to be retried. Nineteen exhaustions in one session.
//
// 14 is "Free Use" in the specification and exists for exactly this. Tried last,
// after the three proper ones, because a device on an address matching its type
// is what a television expects and this is a fallback rather than a preference.
constexpr uint8_t kLogicalAddressFreeUse = 14;

enum class PowerStatus : uint8_t {
    On                     = 0x00,
    Standby                = 0x01,
    TransitioningToOn      = 0x02,
    TransitioningToStandby = 0x03,
    Unknown                = 0xFF,
};

const char* PowerStatusName(PowerStatus status);

// Windows device name, truncated to what a CEC frame allows.
const std::string& DefaultOsdName();

// What a poll found out. A poll is a lone header byte and its answer is the
// acknowledgement, so there are three outcomes, not two: somebody acknowledged
// it, nobody did, or the adapter never said which. The third is not "nobody",
// and it is certainly not "somebody" - see Poll.
enum class PollAnswer : uint8_t { Present, Absent, Unknown };

struct TxResult {
    bool accepted = false;   // DPCD writes went through
    bool sent = false;       // adapter reported TX_MESSAGE_SENT
    bool nacked = false;     // address or data NACK
    bool lineError = false;
    uint8_t irqFlags = 0;
};

class CecDevice {
public:
    CecDevice(AuxBackend& backend, AuxPort port)
        : backend_(backend), port_(std::move(port)) {}

    const AuxPort& port() const { return port_; }
    const std::string& lastError() const { return lastError_; }

    // True if this port's sink advertises CEC tunneling at all.
    bool IsCecCapable(bool& capable);

    // Enables tunneling and claims the current logical address. Must be re-run
    // after any link drop - a sleep/wake cycle very likely clears both.
    bool Initialise();

    // Polls the three playback addresses and claims the first unused one.
    // Anything that answers a poll already owns that address. Returns false
    // only if the bus could not be probed at all, in which case the previous
    // address is kept. Worth re-running whenever the topology may have changed;
    // the wake path reuses the last result to stay fast.
    // `preferred` is the address this device held last time, which is tried
    // first so that a machine keeps the same one across restarts. Televisions
    // remember what they learnt about an address, and this one would not be
    // told otherwise - see the note in the definition.
    bool AllocateLogicalAddress(uint8_t preferred = 0);

    // The address to ask for at the next Attach(). Set by the service from what
    // it remembers of the last run, since only it has somewhere to keep it.
    void set_preferredAddress(uint8_t address) { preferredAddress_ = address; }

    // The address this device holds until an allocation says otherwise, which
    // on the wake path is for good, since a wake never allocates. Set by the
    // service from the device this one replaces.
    //
    // Without it every rebuilt device started on Playback 1, so a PC that went
    // to sleep on 8 woke on 4 - on top of whatever was already there. With two
    // PCs on a Fire TV that made the pair swap addresses on every wake, and
    // after four swaps in eight minutes the set stopped registering one of them
    // at all and ignored its every claim to the screen until it was unplugged
    // at the wall. The comments here had always said the wake path kept the
    // address; nothing had ever made it so.
    //
    // It also decides which address AllocateLogicalAddress treats as this
    // device's own, and so exempts from the avoid mask. Left at the default,
    // the exemption fell on 4 whoever held it, so a full attach would probe a
    // sleeping neighbour's 4 and take it.
    //
    // Anything that is not one of the four addresses this app uses is ignored,
    // so a bad value can never reach the mask: the address stays as it was.
    void set_logicalAddress(uint8_t address) {
        if (address == kLogicalAddressPlayback1 ||
            address == kLogicalAddressPlayback2 ||
            address == kLogicalAddressPlayback3 ||
            address == kLogicalAddressFreeUse) {
            logicalAddress_ = address;
        }
    }

    // The television this port read as last time, for the same reason: only the
    // sink's own answer may rename a set, and a service that has just started
    // has not read anything yet. The name is left empty; it is the key that
    // decides whether this is the same television.
    void set_knownIdentityKey(std::string key) { identity_.key = std::move(key); }

    uint8_t logicalAddress() const { return logicalAddress_; }

    // Joins the CEC topology: <Report Physical Address>, <Device Vendor ID>,
    // <Set OSD Name>. Required before any power command will be honoured.
    bool Announce(uint16_t physicalAddress);

    // Reads the sink's EDID and sets the physical address from it.
    bool DiscoverPhysicalAddress();

    // Empty when the last address discovery read the sink's own EDID, as it
    // should. Otherwise it says what went wrong and where the address came
    // from instead, for the caller to log: an address believed wrongly is the
    // one failure here that looks exactly like success.
    const std::string& addressNote() const { return addressNote_; }

    // Whether the last address came from Windows' stored copy rather than from
    // the sink itself. The two age quite differently: the sink answers about
    // this port, now, while Windows' copy is written when Windows sees fit and
    // has been caught still naming the port a cable had already left. Anything
    // waiting for an address to change has to know which of the two it is
    // waiting on.
    bool addressFromWindows() const { return addressFromWindows_; }

    // Convenience: DiscoverPhysicalAddress() + Initialise() + Announce().
    // A failed EDID read is not fatal - the previous address is kept. Pass
    // false on a wake path, where the EDID has not changed and re-reading it
    // only adds latency before the TV hears anything.
    bool Attach(bool discoverAddress = true);

    // Wake path: gets <Image View On> onto the wire as early as possible, then
    // rejoins the topology and repeats. Assumes the physical address is already
    // known, so pair it with a prior Attach() or DiscoverPhysicalAddress().
    // msToFirstCommand, if given, receives how long until <Image View On> was
    // actually on the wire - which is the latency the user perceives. The
    // function itself runs for longer, completing the topology announce.
    bool WakeDisplay(uint32_t* msToFirstCommand = nullptr);

    // Sends <Set OSD Name> on its own, without re-registering. Useful because
    // the TV accepts a name while it is showing only its own placeholder, but
    // ignores one once it holds a real name - so this can fill in a label that
    // was missed at first contact without disturbing anything else.
    bool SendOsdName();

    // CEC user-control codes (HDMI CEC spec). Sent as <User Control Pressed>
    // followed by <User Control Released>, which is how a remote key press is
    // represented on the bus.
    enum UiCommand : uint8_t {
        kUiVolumeUp   = 0x41,
        kUiVolumeDown = 0x42,
        kUiMute       = 0x43,
    };

    // Sends <User Control Pressed> + <User Control Released> to `target`.
    // Whether a given set acts on these is entirely up to the TV - the transport
    // carries them regardless, so a failure here is the TV declining, not the
    // adapter refusing.
    bool SendUserControl(uint8_t uiCommand, uint8_t target = kLogicalAddressTv);

    // The end of a press-and-hold. <User Control Pressed> means a key is being
    // held and the device repeats the action on its own until this arrives, so
    // holding a button costs two frames in total rather than two per step.
    //
    // It is not optional. If it is lost the device keeps repeating, so every
    // path that ends a hold must send it. The press itself goes through
    // SendVolumePressed, which retries a refusal.
    bool SendUserControlReleased(uint8_t target = kLogicalAddressTv);

    // Volume, addressed to whichever device is actually going to act on it.
    //
    // With a soundbar or receiver on the bus, sending volume to the TV makes
    // the TV relay it, and on some TVs that relay is the whole delay: their own
    // remote is just as slow, while the soundbar's remote is not. These pick
    // the audio system when one answered a poll, and fall back to the TV the
    // moment it stops answering, so a wrong guess costs one frame rather than
    // leaving the buttons dead.
    bool SendVolumePressed(uint8_t uiCommand);
    bool SendVolumeReleased();
    bool SendVolumeTap(uint8_t uiCommand);

    // How many refused attempts the last SendVolumePressed needed before it
    // got through or gave up. For the log: it is the only evidence of whether
    // the retry is rescuing clicks or merely delaying failures.
    int pressRetries() const { return pressRetries_; }

    // Reports every frame taken off the wire, answered or not.
    //
    // Diagnostics only, and deliberately a hook rather than logging in here:
    // this layer is shared with a console tool and a service that log in
    // completely different ways, and neither should have to care what the
    // other does. Set it to nothing to turn it off.
    using FrameObserver =
        std::function<void(const std::vector<uint8_t>& frame, bool answered)>;
    void set_frameObserver(FrameObserver observer) {
        onFrame_ = std::move(observer);
    }

    // Reports every frame that says which input is on screen, or asks, or
    // switches something off - from any path that reads the bus, not only the
    // ones FrameObserver sees. Rare enough to log always. Diagnostics for
    // several devices sharing one TV.
    void set_routingObserver(FrameObserver observer) {
        onRouting_ = std::move(observer);
    }

    // Told the input on screen every time that answer could have changed, from
    // whichever thread noticed it.
    //
    // Pushed rather than polled, and that is the whole point. The menu's dot
    // used to be read from a snapshot the query pump rebuilt, and the pump is
    // exactly what stops running when something is happening: it yields for
    // kPumpYieldMs after any traffic, and it skips the tick altogether while a
    // command holds the CEC lock. A switch is several seconds of both at once,
    // so the snapshot was not rebuilt even once during the exchange that
    // changed the answer. The picture moved, this device knew, and the only
    // copy anybody could read still named the input from before the switch.
    // Giving the menu its own pipe did not help, because what it then fetched
    // promptly was that same stale copy.
    //
    // Every path that drains the bus reaches NoteRouting, including the command
    // paths, which drain frames themselves rather than waiting for the pump. So
    // hanging the publish there covers precisely the case the pump cannot.
    using ScreenPathSink = std::function<void(uint16_t path, bool known)>;
    void set_screenPathSink(ScreenPathSink sink) {
        onScreenPath_ = std::move(sink);
        PublishScreenPath();   // so a sink starts out current rather than empty
    }

    // Reports every frame put on the wire and what the adapter made of it.
    //
    // Frames received have been logged for a long time and frames sent have
    // not, which left "CEC frame was not acknowledged" naming no frame - and a
    // television that answers questions while refusing to be switched off is
    // exactly the case where which frame was refused is the whole answer.
    using SendObserver =
        std::function<void(const std::vector<uint8_t>& frame, const TxResult&)>;
    void set_sendObserver(SendObserver observer) {
        onSend_ = std::move(observer);
    }

    // Reports the remote-control keys a TV forwards to this device.
    //
    // A TV hands its remote's buttons to whichever device is the active source,
    // which is how a PC on an HDMI input gets to be driven from the sofa. Kept
    // as a hook for the same reason as FrameObserver: this layer cannot inject
    // keystrokes anyway. The service runs in Session 0, which has no desktop,
    // so the press has to travel to the tray app before it becomes input.
    //
    // uiCommand is the CEC UI command code on a press, and meaningless on a
    // release - <User Control Released> carries no parameter, because a TV only
    // ever has one button down at a time.
    using UserControlObserver =
        std::function<void(uint8_t uiCommand, bool pressed)>;
    void set_userControlObserver(UserControlObserver observer) {
        onUserControl_ = std::move(observer);
    }

    // Answers whatever the TV has already asked, then returns. Non-blocking,
    // so it can be driven from a timer: being reachable at any moment is what
    // makes the input name reliable, since a TV that asks <Give OSD Name> and
    // hears nothing falls back to a placeholder.
    int ServicePending(int maxFrames = 8);

    // Asks whoever owns volume what it currently is. Diagnostic: a set that
    // answers implements CEC audio control, and one that does not cannot be
    // made to by sending volume harder. Status is the raw <Report Audio Status>
    // byte - bit 7 mute, bits 6-0 volume, 0x7F meaning the device declines to
    // say. Returns false, with lastError() set, if nothing usable came back.
    // refused, when given, says whether that was an explicit <Feature Abort>
    // rather than silence: a set that refuses will go on refusing, while one
    // that was merely slow may well answer next time.
    bool QueryAudioStatus(uint8_t* status, int timeoutMs = 1500,
                          bool* refused = nullptr);

    // Sends <Give Audio Status> and returns as soon as it is on the wire,
    // without waiting for the answer.
    //
    // The answer arrives like any other frame and is recorded by whoever next
    // services the receive buffer - see audioNews(). Waiting for it here would
    // mean holding the CEC lock through the television's thinking time, which
    // is exactly what used to make a button release wait behind a volume read.
    bool AskAudioStatus();

    // Everything heard about volume, counted. Each <Report Audio Status>
    // addressed to this device, asked for or volunteered, and each
    // <Feature Abort> of <Give Audio Status> adds one, so a caller can tell news
    // that arrived after its question from news that was already there. A
    // count rather than a timestamp, because a count cannot wrap round into
    // looking new.
    struct AudioNews {
        uint32_t count = 0;     // zero means nothing heard yet
        bool refused = false;   // the latest news was a refusal
        uint8_t status = 0;     // bit 7 mute, bits 6-0 level; 0x7F declines
        // Set for good once the device that owns volume has answered
        // <Set Audio Volume Level> with <Feature Abort>, which is a statement
        // about the device and not about the moment, so it is never cleared.
        // See SendSetAudioVolumeLevel for why the message is sent at all.
        bool levelRefused = false;
    };
    const AudioNews& audioNews() const { return audioNews_; }

    // <Set Audio Volume Level>, CEC 2.0's absolute volume. One byte, 0-100.
    //
    // Sent on spec rather than on evidence: there is no way to ask a 1.4 device
    // whether it implements a 2.0 message, because the mechanism for asking -
    // <Give Features> - is itself 2.0. Trying it costs one frame, and a set
    // that does not know it either ignores it or answers <Feature Abort>.
    bool SendSetAudioVolumeLevel(uint8_t level);

    // The TV's own CEC version, or 0 if it will not say. 0x05 is 1.4, 0x06 is
    // 2.0. A floor, not a capability list - see SendSetAudioVolumeLevel.
    uint8_t QueryCecVersion(int timeoutMs = 1500);

    // Logical addresses known to belong to another copy of this app, as a bit
    // per address, which AllocateLogicalAddress will not take.
    //
    // A poll is the only way to ask whether an address is free, and it lies in
    // one specific case: a device that is itself probing, or part way through
    // announcing, does not answer. Two PCs starting together hit that window
    // and take each other's addresses, and then keep doing it, because each
    // move makes the other detect a clash and probe again. Measured on a Fire
    // TV: the Deck attached on 8, the Dell probed 8 two seconds later while the
    // Deck was mid-announce, read the silence as free, and moved onto it.
    //
    // A hello is better evidence than a poll, because it was sent rather than
    // not-sent, it carries this app's vendor ID so nothing else can forge it,
    // and it is repeated every ten seconds. Where one says an address is taken,
    // no amount of silence should talk us out of it.
    //
    // The address this device already holds is never avoided, whatever is in
    // here, or a stale entry would stop it keeping its own address.
    void set_avoidAddresses(uint16_t mask) { avoidAddresses_ = mask; }

    // Throws away everything this object has concluded about the television.
    //
    // Call it when the set on the other end is a different one. A wake is
    // exactly when a laptop turns up on a different television, because it was
    // carried there asleep, and it only learns that after this device has
    // already started talking to the new set.
    //
    // What is kept is everything about the adapter and this PC: the logical
    // address, the physical address, the OSD name, the malformed-frame and
    // overflow counters. Those describe hardware that did not change. What goes
    // is every conclusion drawn from the old set and its bus - which input is on
    // screen, whether a soundbar answers, whether volume levels are accepted,
    // what the set last said about routing, and the last Feature Abort. Each of
    // those would otherwise be applied to a television that never said it.
    void ForgetTelevision();

    // Polls for an audio system and remembers the answer. Cheap enough to redo
    // whenever the topology is refreshed, which is also when it can change.
    void DetectAudioSystem();
    bool hasAudioSystem() const { return audioSystem_; }
    uint8_t volumeTarget() const {
        return audioSystem_ ? kLogicalAddressAudioSystem : kLogicalAddressTv;
    }

    // <Set Stream Path>: asks whoever owns that physical address to become the
    // active source, which is how the TV is told to switch input. Broadcast.
    bool SendSetStreamPath(uint16_t physicalAddress);

    // <Active Source>: claims the TV's attention for this PC's own input.
    bool SendActiveSource();

    // Whether this PC is, as far as the bus has said, the input on screen.
    //
    // Followed from the routing messages that pass anyway - <Active Source>,
    // <Set Stream Path>, <Routing Change> and <Routing Information> - and from
    // this device's own claims. Nothing asks the TV, so a set that changes input
    // without announcing it leaves this believing the old answer. True until
    // anything has been heard, because this device claims the input on every
    // wake and attach, which is nearly always the first thing said.
    //
    // Deliberately loose, and a request counts: a television routing away from
    // this PC is reason enough to stop believing we are on screen, even before
    // the device it named has said anything. The power paths read this, and
    // leaving a television on that is showing somebody else is the safer way to
    // be wrong. activePath() is the strict answer, and the two are kept apart
    // because the menu needs the strict one.
    bool OnScreen() const {
        const bool requestIsNewer =
            requestedKnown_ &&
            (!activePathKnown_ ||
             static_cast<int32_t>(requestedTick_ - claimedTick_) > 0);
        if (requestIsNewer) return requestedPath_ == physicalAddress_;
        return !activePathKnown_ || activePath_ == physicalAddress_;
    }

    // A signal meant to count, on the TV, as someone using it, so that the TV's
    // own auto power off does not switch it off under a still picture.
    //
    // Nothing in CEC means "stay on". What TVs switch off for is usually hours
    // without remote input, so the likeliest signal is a remote key, and the
    // question is which key does nothing visible. In order of expectation:
    enum class KeepAlive : uint32_t {
        // F5. A real key in the CEC key table, so a TV passes it into its input
        // handling - on Android-based sets that is what resets the inactivity
        // timer - yet almost nothing gives it a job, least of all while an HDMI
        // input fills the screen. If the TV hands it back to this PC, the
        // service does not map it and ignores it.
        kF5Key = 0,
        // 0x77, a code the key table leaves unassigned. Certain to do nothing,
        // and for that reason likely to be dropped before it counts.
        kUnassignedKey = 1,
        // <Image View On>. Invisible on a set that is already on, and it never
        // changes input, but it is less likely to count as use. Only ever sent
        // to a TV that has just reported being on, since otherwise it would
        // switch the TV on.
        kImageViewOn = 2,
        // <Active Source>. Re-claims the input this PC already has. Some TVs
        // show an input banner for it.
        kActiveSource = 3,
    };
    bool SendKeepAlive(KeepAlive signal);

    bool SendStandby(TxResult* result = nullptr);
    bool SendImageViewOn();     // includes <Active Source> to switch input

    // One Touch Play for a television that might be asleep.
    //
    // The specified pair, <Image View On> then <Active Source>, is enough on a
    // set that is already awake. On one in standby it is not: the panel starts
    // powering up and the <Active Source> arrives while the TV's own input
    // handling is still coming up, so it is dropped. The symptom is exact - the
    // first press wakes the television and leaves it on the wrong input, and a
    // second press moments later switches it.
    //
    // This asks the set what state it is in, and repeats the claim only if it
    // was asleep. Use it for anything a person triggers deliberately; the wake
    // path does not need it, because its announce already puts a second of
    // delay between the two frames by accident.
    // watchForContest is for the paths where a person asked for this PC by
    // name - the hotkey, and the menu's own entry. A device that was on screen
    // may claim it straight back: a Chromecast did exactly that, one second
    // after this PC claimed it, and the television followed the Chromecast. So
    // those paths stay and listen for a moment, and claim once more if the
    // screen was taken away again. Once, not repeatedly: two devices taking
    // turns is worse than losing.
    //
    // Off by default, and deliberately off for a claim made because another
    // device asked for one, since that path runs on the query pump and holding
    // the CEC link for another second and a half there would be felt by the
    // volume buttons.
    bool WakeAndClaimInput(bool watchForContest = false);

    // Sends one raw CEC frame and waits for the adapter's transmit status.
    bool SendFrame(const std::vector<uint8_t>& frame, TxResult* result = nullptr);

    // A broadcast, retried when the bus refuses it. See the definition.
    bool SendBroadcast(const std::vector<uint8_t>& frame);

    // Asks the bus which input is on screen and waits for the answer, which
    // updates what OnScreen() reports. True when something answered.
    bool AskWhoIsOnScreen(int timeoutMs);

    // Whether the television itself has named an input since a given moment.
    //
    // A source device claiming the screen is the standard signal that a switch
    // took, and it is what this app waits for - but on the television used for
    // development it is not sufficient. Every switch in the logs that actually
    // moved the picture shows the set naming the path itself, in its own
    // <Set Stream Path> or <Routing Change>, right after the claim; every one
    // that did not move it shows the set saying nothing whatever. So a claim the
    // television has not acknowledged is worth asking about again, which is
    // precisely what a second click was doing by hand.
    //
    // Only frames from the television count. The same opcode from a source is a
    // request, and a request is what is being confirmed.
    bool TvNamedPath(uint16_t path, uint32_t sinceTick) const {
        return tvPathTick_ && tvPath_ == path &&
               static_cast<int32_t>(tvPathTick_ - sinceTick) >= 0;
    }

    // Whether this television ever says anything about routing at all.
    //
    // Measured on an Amazon Fire TV: not one routing frame from the set in a
    // whole session, while switching between two devices worked on the first
    // request every time. It simply follows <Active Source>, which is One Touch
    // Play and exactly what the standard asks of it. So silence from a set like
    // that is not a set withholding confirmation, it is a set with nothing to
    // say - and a device's claim is the whole of the evidence, which is enough.
    // Only a television that does announce routing, and then did not name the
    // input asked for, has told us anything by its silence.
    bool TvAnnouncesRouting() const { return tvPathTick_ != 0; }

    // The input on screen, from whichever statement about it came last.
    //
    // Two things state it: a device claiming the screen with <Active Source>,
    // and the television naming an input itself. Both are facts, and the later
    // one is the current one - which matters on a set that answers a switch
    // request by finishing the switch it was already making, because then the
    // device's claim is true about the bus and stale about the picture. This is
    // what the menu's dot is drawn from, so the dot follows the screen.
    uint16_t screenPath() const {
        const bool tvIsLater =
            tvPathTick_ && (!activePathKnown_ ||
                            static_cast<int32_t>(tvPathTick_ - claimedTick_) > 0);
        return tvIsLater ? tvPath_ : activePath_;
    }
    bool screenPathKnown() const { return activePathKnown_ || tvPathTick_ != 0; }

    // Waits until the television has stopped announcing routing changes.
    //
    // A switch request is only worth sending to a set that is not already in the
    // middle of changing input, because a set that is will finish what it was
    // doing and drop what arrives meanwhile. Measured twice: the request landed
    // while the set was completing a switch another machine had asked for, the
    // named device claimed the screen, and the set routed back to where it had
    // been going anyway. The click that worked, seconds later, landed on an idle
    // bus.
    //
    // Frames are read and answered while waiting, which is not incidental - the
    // thing being waited for only becomes visible by reading them. A set that
    // never announces routing at all has nothing to wait for and returns at
    // once, so this costs such a set nothing.
    void WaitForTvRouteQuiet(int quietMs, int timeoutMs);

    // Waits for a given input to be stated as the one on screen, and returns
    // the moment it is.
    //
    // This is how a switch is confirmed. A device told to take the screen
    // broadcasts <Active Source> when it does, which is a frame that arrives on
    // its own - so the confirmation costs nothing on the bus, and it lands as
    // soon as the device acts rather than whenever a fixed wait happens to
    // expire. Questions that arrive meanwhile are answered as usual, since a
    // television interrogating this PC is exactly what a switch provokes.
    bool AwaitActivePath(uint16_t path, int timeoutMs);

    // ------------------------------------------------ talking to our own kind
    //
    // Two copies of this app on one television cannot switch the set between
    // each other through CEC alone: <Set Stream Path> is a message a TV sends,
    // not one it takes from a source, and the development TCL rebroadcasts it
    // and does nothing. What does work, every time, is a device claiming the
    // screen for itself - that is what every wake does. So a PC that wants to
    // hand the screen over asks the other PC to claim it.
    //
    // Carried in <Vendor Command With ID>, which exists for exactly this and
    // which other makes ignore or Feature Abort. Every frame carries the ID
    // below and a version, and anything else is answered as an unknown opcode.
    static constexpr uint8_t kAppVendorId[3] = {'C', 'E', 'C'};
    static constexpr uint8_t kAppProtocolVersion = 1;
    static constexpr uint8_t kAppHello = 0x01;       // are you one of us?
    static constexpr uint8_t kAppHereIAm = 0x02;     // yes
    static constexpr uint8_t kAppTakeScreen = 0x03;  // please claim the screen
    static constexpr uint8_t kAppTaken = 0x04;       // done
    static constexpr uint8_t kAppRefused = 0x05;     // cannot just now
    static constexpr uint8_t kAppBye = 0x06;         // going away, forget me

    // Hello and its answer both carry the sender's physical address and name,
    // so a PC that wakes is known to the others the moment it announces itself,
    // with nothing to ask and nothing to wait for. Broadcasting hello at attach
    // is what makes discovery immediate rather than a matter of polling.
    bool SayHello(uint8_t target);
    bool SendHereIAm(uint8_t target);

    // Reports every frame received, for keeping a list of what is on the
    // television: which device said it, and what it said about itself.
    void set_deviceObserver(FrameObserver observer) {
        onDevice_ = std::move(observer);
    }

    // Reports another copy of this app: its address, where it is, and its name.
    using PeerFound =
        std::function<void(uint8_t address, uint16_t where, const std::string&,
                           bool answered)>;
    void set_peerFoundHandler(PeerFound handler) {
        onAppPeerFound_ = std::move(handler);
    }

    // Reports one saying goodbye, which it does on its way to sleep.
    using PeerGone = std::function<void(uint8_t address)>;
    void set_peerGoneHandler(PeerGone handler) {
        onAppPeerGone_ = std::move(handler);
    }

    // Whether a device answers a poll at that address. Never this device's own,
    // whose adapter answers for it.
    PollAnswer Poll(uint8_t address);

    // <Give OSD Name> and <Give Physical Address>, for the device map. The name
    // is empty and the address 0xFFFF when nothing answers in time.
    std::string AskName(uint8_t address, int timeoutMs);
    uint16_t AskWhere(uint8_t address, int timeoutMs);

    // <Give Device Vendor ID>. The answer reaches the device list through the
    // observer like everything else; this only says whether one came.
    bool AskVendor(uint8_t address, int timeoutMs);

    // Whether the device at that address is another copy of this app.
    bool AskIfPeer(uint8_t address, int timeoutMs);

    // Asks it to claim the screen, and waits to hear that it has. False covers
    // both a refusal, which a PC whose own screen is off answers, and silence.
    bool AskPeerToTakeScreen(uint8_t address, int timeoutMs);

    // Claims the screen, when another PC asks over the private channel or when
    // the television passes on a <Set Stream Path> naming this device. The
    // service supplies this, because only it knows whether this PC has a
    // picture worth showing.
    using TakeScreenRequest = std::function<bool()>;
    void set_takeScreenHandler(TakeScreenRequest handler) {
        onAppTakeScreen_ = std::move(handler);
    }

    bool SendAppMessage(uint8_t target, uint8_t verb);

    // Whether the last such question was answered, as opposed to what this
    // device has merely overheard or assumed.
    bool activePathAnswered() const { return activePathAnswered_; }

    // How many received frames carried an operand count their opcode does not
    // allow. Always zero on a healthy link; anything else is a torn read out of
    // the adapter's receive buffer, and the service says so once.
    uint32_t malformedFrames() const { return malformedFrames_; }

    // How many times the adapter reported that frames arrived faster than they
    // were collected. See ReceiveFrame.
    uint32_t receiveOverflows() const { return receiveOverflows_; }

    // Reads one queued incoming frame, if any. Returns false when none waiting.
    bool ReceiveFrame(std::vector<uint8_t>& frame);

    // Discards anything queued. The TV polls this device roughly once a second
    // and the adapter buffers those, so a long-lived process that never reads
    // will sit permanently in RX_OVERFLOW. Returns how many frames were dropped.
    int DrainRx(int maxFrames = 32);

    // Answers the standard interrogations a TV sends to a device that has just
    // appeared - OSD name, vendor ID, physical address, power status, CEC
    // version - for up to durationMs. Returns how many were answered.
    //
    // Without this the device is write-only: it announces and then ignores
    // everything asked of it, which is why the TV could be left showing its own
    // placeholder label instead of the name. Run as a short window after
    // announcing rather than a permanent listener, since a service that polls
    // forever defeats the point of being idle.
    int ServiceQueries(int durationMs);

    // Asks the TV for its power state and waits up to timeoutMs for the reply.
    // A TV mid-transition simply stops answering, so a timeout here is a real
    // outcome rather than an error - it usually means "still changing state".
    PowerStatus QueryPowerStatus(int timeoutMs = 3000);

    // Polls until the TV reports standby, or timeoutMs elapses, and returns the
    // last status seen. Preferred over sleeping a fixed time before checking:
    // how long a set takes to act on <Standby> is not specified by CEC and
    // varies by model, so a fixed wait is simultaneously too long for a fast TV
    // and too short for a slow one. This returns as soon as the answer is known.
    //
    // Standby is believed whenever it is reported. Any other state only from a
    // question asked 2.5 seconds or more after the wait began, so a wait
    // shorter than about 4.5 seconds can return standby or unknown, never "on"
    // (hard rule 1). Call it straight after the command it is waiting on.
    PowerStatus WaitForStandby(int timeoutMs);

    // The name the TV shows for this input. Truncated to the 14 characters a
    // CEC frame allows, with non-ASCII dropped. Ignored if that leaves nothing.
    void set_osdName(const std::string& name);
    const std::string& osdName() const { return osdName_; }

    uint16_t physicalAddress() const { return physicalAddress_; }

    // The input the bus last said was on screen, and whether anything has said
    // so at all. For the log: which of the two a refused command is down to,
    // the television ignoring it or this service asking about the wrong input,
    // cannot be told apart afterwards without it.
    uint16_t activePath() const { return activePath_; }
    bool activePathKnown() const { return activePathKnown_; }

    // Whether a frame has arrived claiming to come from this device's own
    // logical address, meaning another device has taken it. Cleared by the
    // caller once it has moved this device somewhere free.
    // Whether the television has refused `opcode` with <Feature Abort> since
    // `sinceTick`. A refusal is not a lost frame: sending it again achieves
    // nothing, and the caller should try another way instead.
    bool Refused(uint8_t opcode, uint32_t sinceTick) const {
        return lastAbortOpcode_ == opcode && lastAbortTick_ &&
               static_cast<int32_t>(lastAbortTick_ - sinceTick) >= 0;
    }

    // <User Control Pressed> with the power-off key, then its release.
    //
    // A different thing entirely from <Standby>: it is the remote's own off
    // button as the television understands it, delivered the same way volume is
    // - which on a set that refuses <Standby> is the route that still works.
    // The key is "Power Off Function" (0x6C), not "Power" (0x40) and not
    // "Power Toggle Function" (0x6D), because only this one means off and
    // nothing else, so it can never turn a television back on.
    bool SendPowerOffKey();

    // How many broadcasts in a row the bus has refused, reset by the first one
    // it accepts. Anything above a handful means a device on the bus has
    // stopped collecting its messages, which nothing here can put right - but
    // saying so plainly is the difference between a five minute fix, unplug it,
    // and an afternoon of blaming the television.
    int broadcastsRefused() const { return broadcastsRefused_; }

    // Leaves the CEC bus alone: tunneling off, so this adapter stops taking
    // part until the next Initialise(). For a PC on its way to sleep, whose
    // adapter often stays powered afterwards, with nothing left running to
    // collect what arrives.
    bool Quiesce() { return WriteReg(kCecTunnelingControl, {0x00}); }

    bool addressClash() const { return addressClash_; }
    void clear_addressClash() { addressClash_ = false; }
    // Put back when a move could not even be attempted, so the next tick tries
    // again rather than waiting for the clash to be noticed a second time. See
    // MoveIfAddressTaken.
    void set_addressClash() { addressClash_ = true; }

    // False when the last AllocateLogicalAddress could not get a single probe
    // onto the wire, which is a busy moment rather than a full bus. The two
    // failures look identical from outside and want opposite responses.
    bool probedTheBus() const { return probedTheBus_; }

    // False when probing left this device with no logical address at all.
    //
    // The one outcome of AllocateLogicalAddress that cannot be lived with. The
    // address mask is dropped to broadcast while probing and put back after,
    // and if putting it back fails five times over, the television stops seeing
    // this device entirely: every frame addressed to it goes unanswered, no
    // clash is ever detected because nothing arrives, and nothing retries. It
    // looks exactly like a working connection from the outside, which is why
    // the attach path has to be told rather than left to infer it.
    bool addressUsable() const { return addressUsable_; }
    void set_physicalAddress(uint16_t pa) { physicalAddress_ = pa; }

    // Which television the last good EDID read described. Empty until a read
    // succeeds, and kept rather than cleared when a later one fails.
    const DisplayIdentity& identity() const { return identity_; }

private:
    // Replies to one received frame if it is a question this device owes an
    // answer to. Shared by the timer-driven and the blocking paths so both
    // answer identically.
    bool AnswerQuery(const std::vector<uint8_t>& frame);
    void NotePeer(const std::vector<uint8_t>& frame);

    bool WriteReg(uint32_t address, const std::vector<uint8_t>& data);
    bool ClaimAddressMask(uint16_t mask);

    // <User Control Pressed>, tried again a moment later if the device refuses
    // it. See kPressRetries.
    bool SendPressRetrying(uint8_t uiCommand, uint8_t target);
    int pressRetries_ = 0;

    // Claims the screen again if something took it back. See WakeAndClaimInput.
    void WatchForContest();

    // Notes which input a received frame says is on screen. See OnScreen.
    void NoteRouting(const std::vector<uint8_t>& frame);

    AuxBackend& backend_;
    AuxPort port_;
    uint8_t logicalAddress_ = kLogicalAddressPlayback1;

    // Whether a device answered on the audio system address, and where the key
    // currently held down was sent. The release has to reach the same device as
    // its press, or that device is left holding a key nothing ever lifts.
    bool audioSystem_ = false;
    uint8_t volumeTarget_ = kLogicalAddressTv;
    uint16_t physicalAddress_ = 0x1000;   // 1.0.0.0 until EDID says otherwise
    // The path the bus last named as on screen, and whether it has named one.
    // What is on screen, and only ever from something that said so:
    // <Active Source>, <Routing Change>, <Routing Information>, or this
    // device's own claim. This is what the menu's dot is drawn from.
    uint16_t activePath_ = 0;
    bool activePathKnown_ = false;
    bool activePathAnswered_ = false;
    uint32_t claimedTick_ = 0;
    // And what was last asked for, which is a different thing: a device may
    // ignore <Set Stream Path>, and a television repeats it. Only OnScreen()
    // reads these, to stay on the careful side for the power paths.
    uint16_t requestedPath_ = 0;
    bool requestedKnown_ = false;
    uint32_t requestedTick_ = 0;
    // The last input the television itself named, whichever routing message it
    // used. See TvNamedPath.
    uint16_t tvPath_ = 0;
    uint32_t tvPathTick_ = 0;
    uint16_t avoidAddresses_ = 0;
    bool addressClash_ = false;
    bool probedTheBus_ = false;
    bool addressUsable_ = true;
    uint8_t preferredAddress_ = 0;
    int broadcastsRefused_ = 0;
    uint8_t lastAbortOpcode_ = 0;
    uint32_t lastAbortTick_ = 0;
    DisplayIdentity identity_;
    std::string osdName_ = DefaultOsdName();
    AudioNews audioNews_;

    std::string addressNote_;
    bool addressFromWindows_ = false;
    uint32_t lastClaimTick_ = 0;
    uint32_t malformedFrames_ = 0;
    uint32_t receiveOverflows_ = 0;
    uint8_t peerSeen_ = 0xFF;
    uint8_t peerAnswer_ = 0;
    uint8_t peerAsked_ = 0xFF;   // whom AskPeerToTakeScreen is waiting on
    TakeScreenRequest onAppTakeScreen_;
    PeerFound onAppPeerFound_;
    PeerGone onAppPeerGone_;
    FrameObserver onFrame_;
    FrameObserver onRouting_;
    ScreenPathSink onScreenPath_;
    void PublishScreenPath() {
        if (onScreenPath_) onScreenPath_(screenPath(), screenPathKnown());
    }
    FrameObserver onDevice_;
    SendObserver onSend_;
    UserControlObserver onUserControl_;
    std::string lastError_;
};

// Probes every port of every backend and returns the first that advertises CEC
// tunneling capability. This is how the correct output is found without
// guessing - including on hybrid laptops where the port may belong to the
// integrated GPU rather than the discrete one.
bool FindCecPort(std::vector<std::unique_ptr<AuxBackend>>& backends,
                 AuxBackend** foundBackend, AuxPort* foundPort,
                 std::string* error);

}  // namespace cec
