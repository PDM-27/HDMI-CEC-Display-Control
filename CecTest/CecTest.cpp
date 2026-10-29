// CecTest - bench tool for driving HDMI-CEC over the DisplayPort AUX channel.
// See CecDevice.h for why the announce sequence is not optional.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CecDevice.h"
#include "IntelBackend.h"
#include "ShutdownKind.h"

namespace {

bool ParseHex(const char* text, uint32_t& out) {
    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 16);
    if (end == text || *end != '\0') return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool ParseHexBytes(int argc, char** argv, int start, std::vector<uint8_t>& out) {
    for (int i = start; i < argc; ++i) {
        uint32_t value = 0;
        if (!ParseHex(argv[i], value) || value > 0xFF) {
            std::printf("Not a hex byte: %s\n", argv[i]);
            return false;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return !out.empty();
}

void PrintHex(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        std::printf("%02X%s", data[i], (i + 1 == data.size()) ? "" : " ");
    }
}

const char* OpcodeName(uint8_t op) {
    switch (op) {
        case 0x00: return "<Feature Abort>";
        case 0x04: return "<Image View On>";
        case 0x0D: return "<Text View On>";
        case 0x32: return "<Set Menu Language>";
        case 0x36: return "<Standby>";
        case 0x44: return "<User Control Pressed>";
        case 0x45: return "<User Control Released>";
        case 0x46: return "<Give OSD Name>";
        case 0x47: return "<Set OSD Name>";
        case 0x70: return "<System Audio Mode Request>";
        case 0x71: return "<Give Audio Status>";
        case 0x72: return "<Set System Audio Mode>";
        case 0x73: return "<Set Audio Volume Level>";
        case 0x7A: return "<Report Audio Status>";
        case 0x7D: return "<Give System Audio Mode Status>";
        case 0x7E: return "<System Audio Mode Status>";
        case 0x80: return "<Routing Change>";
        case 0x81: return "<Routing Information>";
        case 0x82: return "<Active Source>";
        case 0x83: return "<Give Physical Address>";
        case 0x84: return "<Report Physical Address>";
        case 0x85: return "<Request Active Source>";
        case 0x86: return "<Set Stream Path>";
        case 0x87: return "<Device Vendor ID>";
        case 0x89: return "<Vendor Command>";
        case 0x8A: return "<Vendor Remote Button Down>";
        case 0x8B: return "<Vendor Remote Button Up>";
        case 0x8C: return "<Give Device Vendor ID>";
        case 0x8D: return "<Menu Request>";
        case 0x8E: return "<Menu Status>";
        case 0x8F: return "<Give Device Power Status>";
        case 0x90: return "<Report Power Status>";
        case 0x9E: return "<CEC Version>";
        case 0x9D: return "<Inactive Source>";
        case 0x9F: return "<Get CEC Version>";
        case 0xA0: return "<Vendor Command With ID>";
        default:   return "";
    }
}

void PrintTx(const char* label, const cec::TxResult& tx, bool ok) {
    std::printf("  %-28s %s", label, ok ? "sent" : "FAILED");
    if (tx.irqFlags) {
        std::printf("   (0x3004=0x%02X", tx.irqFlags);
        if (tx.nacked) std::printf(" NACK");
        if (tx.lineError) std::printf(" LINE_ERROR");
        std::printf(")");
    }
    std::printf("\n");
}

int CmdList(std::vector<std::unique_ptr<cec::AuxBackend>>& backends) {
    int total = 0;
    for (auto& backend : backends) {
        if (!backend->Open()) {
            std::printf("[%s] unavailable: %s\n", backend->Name(),
                        backend->LastError().c_str());
            continue;
        }
        std::vector<cec::AuxPort> ports;
        if (!backend->EnumeratePorts(ports)) {
            std::printf("[%s] enumeration failed: %s\n", backend->Name(),
                        backend->LastError().c_str());
            continue;
        }
        for (const auto& port : ports) {
            cec::CecDevice device(*backend, port);
            bool capable = false;
            const bool read = device.IsCecCapable(capable);
            std::printf("[%d] %s  %s  CEC:%s\n", total++, port.Describe().c_str(),
                        port.isActive ? "active" : "inactive",
                        !read ? "unreadable" : (capable ? "YES" : "no"));
        }
    }
    if (total == 0) std::printf("No connected DisplayPort outputs found.\n");
    return total > 0 ? 0 : 1;
}

int CmdProbe(cec::CecDevice& device, cec::AuxBackend& backend) {
    std::printf("%s\n\n", device.port().Describe().c_str());

    std::vector<uint8_t> caps;
    if (!backend.ReadDpcd(device.port(), 0x00000, 16, caps)) {
        std::printf("DPCD 0x00000 read FAILED: %s\n", backend.LastError().c_str());
        return 1;
    }
    std::printf("DPCD 0x00000 (receiver caps): ");
    PrintHex(caps);
    std::printf("\n  DPCD revision %u.%u\n", caps[0] >> 4, caps[0] & 0x0F);

    // Which converter chip is actually inside the dongle.
    //
    // Worth having because CEC support is a property of the chip and of whether
    // the board bothered to wire the HDMI CEC pin to it, and neither is on the
    // box. DisplayPort branch devices identify themselves at 0x500: three bytes
    // of IEEE OUI, then six ASCII characters of device ID, then revisions. A
    // Chrontel CH7218 answers 2B-02-F0 and "CH7218", which is self-describing
    // in the same way the receiver caps are - nothing else produces those bytes
    // in that order.
    std::vector<uint8_t> branch;
    if (backend.ReadDpcd(device.port(), 0x00500, 12, branch) &&
        branch.size() >= 12) {
        std::printf("\nDPCD 0x00500 (branch device): ");
        PrintHex(branch);
        std::printf("\n");

        const uint32_t oui = (static_cast<uint32_t>(branch[0]) << 16) |
                             (static_cast<uint32_t>(branch[1]) << 8) | branch[2];
        const char* vendor = "unknown";
        switch (oui) {
            case 0x2B02F0: vendor = "Chrontel"; break;
            case 0x00E04C: vendor = "Realtek"; break;
            case 0x000000: vendor = "none reported"; break;
            default: break;
        }
        std::printf("  OUI       : %02X-%02X-%02X (%s)\n", branch[0],
                    branch[1], branch[2], vendor);

        char id[7] = {};
        for (int i = 0; i < 6; ++i) {
            const uint8_t c = branch[3 + i];
            id[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : ' ';
        }
        std::printf("  device ID : %s\n", id);
        std::printf("  revision  : hw %u, fw %u.%u\n", branch[9],
                    branch[10], branch[11]);
    }

    std::vector<uint8_t> cecRegs;
    if (!backend.ReadDpcd(device.port(), cec::kCecTunnelingCapability, 2, cecRegs)) {
        std::printf("\nDPCD 0x03000 read FAILED: %s\n", backend.LastError().c_str());
        return 1;
    }
    std::printf("\nDPCD 0x03000 CEC_TUNNELING_CAPABILITY = 0x%02X\n", cecRegs[0]);
    std::printf("  tunneling capable : %s\n", (cecRegs[0] & 0x01) ? "YES" : "no");
    std::printf("  snooping capable  : %s\n", (cecRegs[0] & 0x02) ? "YES" : "no");
    std::printf("  multi-LA capable  : %s\n", (cecRegs[0] & 0x04) ? "YES" : "no");
    std::printf("DPCD 0x03001 CEC_TUNNELING_CONTROL = 0x%02X\n", cecRegs[1]);
    std::printf("  tunneling enabled : %s\n", (cecRegs[1] & 0x01) ? "YES" : "no");
    return 0;
}

int CmdCecStatus(cec::CecDevice& device, cec::AuxBackend& backend) {
    std::vector<uint8_t> regs;
    if (!backend.ReadDpcd(device.port(), cec::kCecTunnelingCapability, 5, regs)) {
        std::printf("Read 0x03000..0x03004 FAILED: %s\n", backend.LastError().c_str());
        return 1;
    }
    std::printf("0x03000 CAPABILITY   = 0x%02X\n", regs[0]);
    std::printf("0x03001 CONTROL      = 0x%02X  (tunneling %s)\n", regs[1],
                (regs[1] & 0x01) ? "ENABLED" : "disabled");
    std::printf("0x03002 RX_INFO      = 0x%02X\n", regs[2]);
    std::printf("0x03003 TX_INFO      = 0x%02X  (send bit %s)\n", regs[3],
                (regs[3] & cec::kCecTxSendBit) ? "STILL SET" : "clear");
    std::printf("0x03004 IRQ flags    = 0x%02X", regs[4]);
    if (regs[4] & cec::kIrqTxMessageSent)      std::printf("  TX_MESSAGE_SENT");
    if (regs[4] & cec::kIrqTxLineError)        std::printf("  TX_LINE_ERROR");
    if (regs[4] & cec::kIrqTxAddressNackError) std::printf("  TX_ADDRESS_NACK");
    if (regs[4] & cec::kIrqTxDataNackError)    std::printf("  TX_DATA_NACK");
    if (regs[4] & cec::kIrqRxMessageInfoValid) std::printf("  RX_VALID");
    if (regs[4] & cec::kIrqRxMessageOverflow)  std::printf("  RX_OVERFLOW");
    std::printf("\n");

    std::vector<uint8_t> mask;
    if (backend.ReadDpcd(device.port(), cec::kCecLogicalAddressMask, 2, mask)) {
        const uint16_t bits = static_cast<uint16_t>(mask[0] | (mask[1] << 8));
        std::printf("0x0300E LA mask      = %02X %02X (0x%04X):", mask[0], mask[1], bits);
        if (!bits) std::printf(" none claimed");
        for (int la = 0; la < 16; ++la) {
            if (bits & (1u << la)) std::printf(" LA%d", la);
        }
        std::printf("\n");
    }
    return 0;
}

int CmdRx(cec::CecDevice& device, int maxFrames, bool sayWhenEmpty = true) {
    int seen = 0;
    std::vector<uint8_t> frame;
    while (seen < maxFrames && device.ReceiveFrame(frame)) {
        std::printf("  RX  ");
        PrintHex(frame);
        if (frame.size() >= 2) {
            std::printf("   src=%u dst=%u %s", frame[0] >> 4, frame[0] & 0x0F,
                        OpcodeName(frame[1]));
        } else {
            std::printf("   poll: src=%u dst=%u", frame[0] >> 4, frame[0] & 0x0F);
        }
        std::printf("\n");
        ++seen;
    }
    if (seen == 0 && sayWhenEmpty) {
        std::printf("  (no queued incoming frames)\n");
    }
    return 0;
}

int CmdAttach(cec::CecDevice& device) {
    if (device.DiscoverPhysicalAddress()) {
        std::printf("Physical address from EDID: %s\n",
                    cec::FormatPhysicalAddress(device.physicalAddress()).c_str());
    } else {
        std::printf("Physical address: %s (EDID unavailable: %s)\n",
                    cec::FormatPhysicalAddress(device.physicalAddress()).c_str(),
                    device.lastError().c_str());
    }

    if (!device.Initialise()) {
        std::printf("CEC init FAILED: %s\n", device.lastError().c_str());
        return 1;
    }
    if (device.AllocateLogicalAddress()) {
        std::printf("CEC tunneling enabled, claimed logical address %u\n",
                    device.logicalAddress());
    } else {
        std::printf("CEC tunneling enabled, using logical address %u (%s)\n",
                    device.logicalAddress(), device.lastError().c_str());
    }

    const uint16_t pa = device.physicalAddress();
    std::printf("Announcing as %s:\n", cec::FormatPhysicalAddress(pa).c_str());
    if (!device.Announce(pa)) {
        std::printf("Announce FAILED: %s\n", device.lastError().c_str());
        return 1;
    }
    // Listed in the order Announce() actually sends them - the name goes second
    // so the TV replaces its placeholder label as early as possible.
    std::printf("  <Report Physical Address>    sent\n");
    std::printf("  <Set OSD Name> \"%s\"    sent\n", device.osdName().c_str());
    std::printf("  <Device Vendor ID>           sent\n");
    return 0;
}

// Joins the CEC bus before a command that talks on it.
//
// Every command below that sends a frame needs this and several were not doing
// it, which cost an evening: `audio-status` on an LG reported that the frame
// was not acknowledged, and it was not, because nothing had enabled tunnelling
// or claimed a logical address, so the question went out from an unregistered
// device and the television had no reason to answer. The same silence reads
// exactly like a television that refuses, which is how a wrong finding gets
// written down.
//
// Quiet on success, because these commands print their own results and the
// attach is not the subject.
bool JoinBus(cec::CecDevice& device) {
    if (device.Attach()) return true;
    std::printf("Could not join the CEC bus: %s\n", device.lastError().c_str());
    return false;
}

void PrintUsage() {
    std::printf(
        "Usage: CecTest [-d <index>] <command>\n\n"
        "  list                    enumerate DisplayPort outputs and CEC support\n"
        "  probe                   receiver caps + CEC tunneling capability\n"
        "  read  <addr> <len>      raw DPCD read  (hex addr, decimal length)\n"
        "  write <addr> <hex...>   raw DPCD write (hex addr, hex bytes)\n"
        "  attach                  enable tunneling and join the CEC topology\n"
        "  cec-status              dump the CEC tunneling register block\n"
        "  cec-send <hex...>       send one raw CEC frame\n"
        "  rx                      drain queued incoming CEC frames\n"
        "  listen [seconds]        watch the CEC bus for incoming traffic\n"
        "  snoop [seconds]         watch EVERY frame, including other devices'\n"
        "  power                   ask the TV for its current power state\n"
        "  audio-status            can this TV report its volume level?\n"
        "  set-volume <0-100>      does it accept an absolute volume?\n"
        "  standby                 attach + <Standby>\n"
        "  on                      attach + <Image View On> + <Active Source>\n"
        "  input-test <play|active|path>  will this TV switch to us at all\n"
        "  intel-probe             why Intel graphics refused, if it did\n");
}

}  // namespace

// Isolates why "switch input" does nothing, which no amount of sending it
// can answer on its own.
//
// Three messages are involved and they are routinely conflated:
//
//   <Image View On> 0x04   "wake up"
//   <Active Source> 0x82   "I am the source being shown now"
//   <Set Stream Path> 0x86 "the stream path is now this physical address"
//
// The specification's One Touch Play is the *pair* - <Image View On> then
// <Active Source> - and a television is entitled to want both. Sending
// <Active Source> alone, as the first version of this test did, therefore
// proves less than it appears to; `play` exists because of that mistake.
//
// <Set Stream Path> asks whatever lives at an address to claim the path, so
// it failing has two indistinguishable causes from the sending end: the set
// ignores routing from a source, or it honoured it and nothing was there to
// answer. Aiming it at this PC's own address removes the second.
int CmdInputTest(cec::CecDevice& device, const std::string& mode) {
    if (mode != "active" && mode != "play" && mode != "path") {
        std::printf(
            "Usage: CecTest input-test <play|active|path>\n"
            "\n"
            "Run one at a time. Each gives you ten seconds to switch the TV\n"
            "to another input with its remote, then sends one thing and\n"
            "watches. The question is whether the TV comes back by itself.\n"
            "\n"
            "  play    <Image View On> + <Active Source> - One Touch Play,\n"
            "          the full sequence, and what the hotkey sends\n"
            "  active  <Active Source> alone - isolates the second half\n"
            "  path    <Set Stream Path> aimed at this PC's own address\n"
            "\n"
            "Try it twice for each: once against a live device on the other\n"
            "input, once against an empty or powered-off one. A source that\n"
            "is awake can contest the path, and that is a different failure\n"
            "from a TV that refuses to move.\n");
        return 1;
    }

    if (!device.Attach()) {
        std::printf("Attach FAILED: %s\n", device.lastError().c_str());
        return 1;
    }
    const uint16_t self = device.physicalAddress();
    std::printf("This PC is at %s (logical address %u).\n",
                cec::FormatPhysicalAddress(self).c_str(),
                device.logicalAddress());

    const char* what = (mode == "play")   ? "<Image View On> + <Active Source>"
                       : (mode == "active") ? "<Active Source>"
                                            : "<Set Stream Path>";
    std::printf("\nSwitch the TV to a DIFFERENT input now. Sending %s in ten seconds.\n",
                what);
    for (int second = 10; second > 0; --second) {
        std::printf("  %2d\r", second);
        std::fflush(stdout);
        Sleep(1000);
    }

    // Re-announced for the same reason every power command does it: a TV
    // ignores messages from a device it has dropped from its topology, and
    // that failure would otherwise read as a TV ignoring the opcode.
    device.Announce(self);

    // Drained before sending, and this is not tidiness.
    //
    // The first run of this test captured <Routing Change> and
    // <Set Stream Path> from the TV and it was impossible to tell whether
    // they were replies or leftovers from the remote press ten seconds
    // earlier. Everything printed below now definitely arrived after the
    // send, and is stamped, so a reply can be told from an echo.
    device.ServicePending(32);

    const DWORD sentAt = GetTickCount();
    device.set_frameObserver([sentAt](const std::vector<uint8_t>& frame,
                                      bool answered) {
        std::printf("  +%4lums  <- ", GetTickCount() - sentAt);
        PrintHex(frame);
        std::printf("%s\n", answered ? "  (answered)" : "");
    });

    bool sent = false;
    if (mode == "play") {
        sent = device.SendImageViewOn();   // sends <Active Source> after it
    } else if (mode == "active") {
        sent = device.SendActiveSource();
    } else {
        sent = device.SendSetStreamPath(self);
    }
    std::printf("     \r%s %s\n", what,
                sent ? "went out on the wire." : "FAILED to send.");
    if (!sent) {
        std::printf("  %s\n", device.lastError().c_str());
        return 1;
    }

    std::printf("\nAnything the TV says back, with the time since sending:\n");
    device.ServiceQueries(6000);

    std::printf(
        "\nDid the TV switch back to this PC on its own?\n"
        "\n"
        "  0x80 <Routing Change> or 0x86 <Set Stream Path> from the TV means\n"
        "  it moved the path - to us if the address is this PC's, away if not.\n"
        "  Nothing at all means it heard and declined.\n");
    return 0;
}

// Whether this television accepts an absolute volume, which decides whether a
// slider is possible or whether the only way there is a run of single steps.
//
// <Set Audio Volume Level> is CEC 2.0. A device announces its version happily
// enough - <CEC Version> is answered by everything - but the version is a floor
// and not a list of what is implemented, and the mechanism for asking what *is*
// implemented, <Give Features>, arrived in 2.0 as well. So a 1.4 set cannot be
// asked about a 2.0 message at all, and sending it is the only way to find out.
//
// Which is cheap: one frame, and the level either moves or it does not.
int CmdSetVolume(cec::CecDevice& device, int target) {
    device.DetectAudioSystem();
    std::printf("Volume on this bus belongs to %s.\n",
                device.hasAudioSystem() ? "the audio system (LA 5)"
                                        : "the TV (LA 0)");

    const uint8_t version = device.QueryCecVersion();
    std::printf("The TV reports CEC version %s (0x%02X).\n",
                version == 0    ? "unknown - it did not answer"
                : version >= 6  ? "2.0 or newer"
                : version == 5  ? "1.4"
                                : "older than 1.4",
                version);
    if (version && version < 6) {
        std::printf(
            "  A 1.4 set has no obligation to implement this. Trying anyway,\n"
            "  because the only cost of being wrong is one ignored frame.\n");
    }

    // A set that will not report its level is still worth asking, because a
    // person can see what this cannot.
    //
    // Refusing to send here was a mistake: it made the one question nobody has
    // answered about the LG unanswerable by the tool built for it. Sending
    // volume where it cannot be read is not something the app would ever do,
    // but the app is not the point of this command; finding out is.
    uint8_t before = 0;
    const bool canRead = device.QueryAudioStatus(&before);
    if (canRead) {
        std::printf("\nVolume before: %u\n", before & 0x7F);
    } else {
        std::printf(
            "\nThis set will not report its volume, so nothing here can measure\n"
            "the result. Sending anyway, because you can watch the television\n"
            "and this tool cannot. Look at its volume while the next line runs.\n");
    }

    // Watch for the polite refusal while waiting for the set to act.
    bool aborted = false;
    device.set_frameObserver(
        [&aborted](const std::vector<uint8_t>& frame, bool) {
            if (frame.size() >= 3 && frame[1] == 0x00 && frame[2] == 0x73) {
                aborted = true;
            }
        });

    std::printf("Sending <Set Audio Volume Level> %d...\n", target);
    if (!device.SendSetAudioVolumeLevel(static_cast<uint8_t>(target))) {
        std::printf("  did not reach the wire: %s\n", device.lastError().c_str());
        return 1;
    }
    device.ServiceQueries(1500);

    uint8_t after = 0;
    const bool readBack = device.QueryAudioStatus(&after);
    if (canRead && !readBack) {
        std::printf("It stopped reporting its volume. Inconclusive.\n");
        return 2;
    }
    if (readBack) std::printf("Volume after:  %u\n\n", after & 0x7F);

    // Nothing to compare against, so the answer is on the screen rather than
    // here. The Feature Abort is still worth reporting, since that one is a
    // refusal in writing and settles it without anybody watching.
    if (!canRead) {
        if (aborted) {
            std::printf(
                "\nThe set answered <Feature Abort> for this opcode. That is a\n"
                "clear no, and absolute volume is not available here.\n");
            return 2;
        }
        std::printf(
            "\nThe frame went out and nothing was said against it.\n"
            "Did the television's volume move to %d?\n"
            "  It did    - absolute volume works here, and the buttons could\n"
            "              be built on it even though the level cannot be read.\n"
            "  It did not - the message was accepted and ignored, which is what\n"
            "              a set that predates it does.\n", target);
        return 0;
    }

    if (aborted) {
        std::printf("The set answered <Feature Abort> for this opcode.\n"
                    "That is a clear no. Absolute volume is not available.\n");
        return 2;
    }
    if ((after & 0x7F) == target) {
        std::printf("It landed exactly on %d. Absolute volume WORKS here,\n"
                    "so a slider is possible and would be instant.\n", target);
        return 0;
    }
    if ((after & 0x7F) != (before & 0x7F)) {
        std::printf("It moved, but not to %d. Something acted on the message;\n"
                    "worth running again from a different starting level.\n", target);
        return 0;
    }
    std::printf("Nothing moved, and nothing was said. The message was ignored,\n"
                "which is what a set that predates it does. A slider would have\n"
                "to be built out of single steps, at about 90ms each.\n");
    return 2;
}

int main(int argc, char** argv) {
    int displayIndex = -1;
    int argi = 1;
    if (argc >= 3 && std::strcmp(argv[1], "-d") == 0) {
        displayIndex = std::atoi(argv[2]);
        argi = 3;
    }
    if (argi >= argc) {
        PrintUsage();
        return 1;
    }
    const std::string command = argv[argi];
    const int rest = argi + 1;

    // Diagnostic: what the service would conclude if asked to shut down now.
    if (command == "shutdown-kind") {
        std::string detail;
        const cec::ShutdownRecord seen =
            cec::QueryShutdown(86400000, {}, 0, &detail);
        std::printf("most recent shutdown event: %s%s\n",
                    cec::ShutdownKindName(seen.kind),
                    seen.byWindowsUpdate ? ", by Windows Update" : "");
        std::printf("  %s\n", detail.c_str());
        return 0;
    }

    auto backends = cec::CreateAllBackends();
    if (command == "intel-probe") {
        std::printf("%s", cec::IntelProbeVersions().c_str());
        return 0;
    }

    if (command == "list") return CmdList(backends);

    cec::AuxBackend* backend = nullptr;
    cec::AuxPort port;
    std::string error;

    if (displayIndex >= 0) {
        // Explicit selection: walk the same order CmdList prints.
        int index = 0;
        for (auto& candidate : backends) {
            if (!candidate->Open()) continue;
            std::vector<cec::AuxPort> ports;
            if (!candidate->EnumeratePorts(ports)) continue;
            for (const auto& candidatePort : ports) {
                if (index++ == displayIndex) {
                    backend = candidate.get();
                    port = candidatePort;
                }
            }
        }
        if (!backend) {
            std::printf("Display index %d not found.\n", displayIndex);
            return 1;
        }
    } else if (!cec::FindCecPort(backends, &backend, &port, &error)) {
        std::printf("No CEC-capable DisplayPort output found: %s\n", error.c_str());
        return 1;
    }

    cec::CecDevice device(*backend, port);

    if (command == "edid") {
        std::vector<uint8_t> edid;
        std::string error;
        if (!cec::ReadEdid(*backend, port, edid, &error)) {
            std::printf("EDID read FAILED: %s\n", error.c_str());
            return 1;
        }
        std::printf("EDID: %zu bytes", edid.size());
        const std::string name = cec::ParseMonitorName(edid);
        if (!name.empty()) std::printf("   monitor: \"%s\"", name.c_str());
        // The key the service files this set's settings under.
        cec::DisplayIdentity identity;
        if (cec::ParseDisplayIdentity(edid, identity)) {
            std::printf("   id: %s", identity.key.c_str());
        }
        std::printf("\n");
        for (size_t i = 0; i < edid.size(); i += 16) {
            std::printf("  %03zX  ", i);
            for (size_t j = i; j < i + 16 && j < edid.size(); ++j) {
                std::printf("%02X ", edid[j]);
            }
            std::printf("\n");
        }
        uint16_t pa = 0;
        if (cec::ParsePhysicalAddress(edid, pa)) {
            std::printf("\nCEC physical address: %s\n",
                        cec::FormatPhysicalAddress(pa).c_str());
        } else {
            std::printf("\nNo HDMI VSDB found - sink has no CEC physical address.\n");
        }
        return 0;
    }

    if (command == "probe")      return CmdProbe(device, *backend);
    if (command == "cec-status") return CmdCecStatus(device, *backend);
    if (command == "attach")     return CmdAttach(device);
    if (command == "rx")         return JoinBus(device) ? CmdRx(device, 32) : 1;

    // Announce under an arbitrary name. Exists to settle whether a mangled
    // label on the TV is dropped characters or just its own rendering: pick
    // something where a missing character is unmistakable.
    if (command == "osdname") {
        if (argc - rest < 1) {
            std::printf("Usage: CecTest osdname <name>\n");
            return 1;
        }
        device.set_osdName(argv[rest]);
        device.DiscoverPhysicalAddress();
        device.Initialise();
        std::printf("Announcing as \"%s\" (%zu chars)\n",
                    device.osdName().c_str(), device.osdName().size());
        device.Announce(device.physicalAddress());
        device.ServiceQueries(3000);
        return 0;
    }

    // Answer whatever the TV asks for a while, reporting how many it asked.
    if (command == "serve") {
        if (!JoinBus(device)) return 1;
        const int seconds = (argc - rest >= 1) ? std::atoi(argv[rest]) : 5;
        std::printf("Answering CEC queries for %d seconds as LA %u...\n",
                    seconds, device.logicalAddress());
        const int answered = device.ServiceQueries(seconds * 1000);
        std::printf("answered %d quer%s\n", answered, answered == 1 ? "y" : "ies");
        return 0;
    }

    if (command == "listen") {
        if (!JoinBus(device)) return 1;
        const int seconds = (argc - rest >= 1) ? std::atoi(argv[rest]) : 10;
        std::printf("Listening %d seconds...\n", seconds);
        for (int i = 0; i < seconds * 4; ++i) {
            CmdRx(device, 8, false);
            Sleep(250);
        }
        return 0;
    }

    // Every frame on the bus, not only the ones addressed to this device.
    //
    // The adapter normally hands over what is addressed here plus the
    // broadcasts, which is right for an application and useless for working out
    // what two other devices are saying to each other. The DisplayPort CEC
    // block has a second mode for that, and this CH7218 advertises it:
    // capability register 0x3000 reads 0x07, which is tunnelling, snooping and
    // multiple logical addresses. Bit 1 of the control register turns snooping
    // on alongside tunnelling, the same bit Linux calls DP_CEC_SNOOPING_ENABLE.
    //
    // Bench only, and put back at the end. Everything on the bus arrives
    // through the same small receive buffer that already tears frames when it
    // is busy, so a service left in this mode would be reading rather more
    // nonsense than usual.
    if (command == "snoop") {
        if (!JoinBus(device)) return 1;
        if (!backend->WriteDpcd(device.port(), cec::kCecTunnelingControl,
                                {0x03})) {
            std::printf("Could not enable snooping: %s\n",
                        backend->LastError().c_str());
            return 1;
        }
        const int seconds = (argc - rest >= 1) ? std::atoi(argv[rest]) : 30;
        std::printf("Snooping the whole bus for %d seconds, as LA %u.\n"
                    "Everything below is somebody's traffic, not necessarily "
                    "ours.\n\n",
                    seconds, device.logicalAddress());
        for (int i = 0; i < seconds * 4; ++i) {
            CmdRx(device, 16, false);
            Sleep(250);
        }
        std::printf("\nDone. Snooping turned back off.\n");
        backend->WriteDpcd(device.port(), cec::kCecTunnelingControl, {0x01});
        return 0;
    }

    // Whether this television can say what its volume is, which decides
    // whether a volume readout is possible on it at all.
    //
    // <Give Audio Status> is asked of whoever owns volume on this bus, so the
    // soundbar poll runs first - asking the TV when a soundbar is holding the
    // audio would answer the wrong question and look like a refusal.
    if (command == "audio-status") {
        if (!JoinBus(device)) return 1;
        device.DetectAudioSystem();
        const char* owner = device.hasAudioSystem() ? "the audio system (LA 5)"
                                                    : "the TV (LA 0)";
        std::printf("Volume on this bus belongs to %s.\n\n", owner);

        uint8_t status = 0;
        if (!device.QueryAudioStatus(&status)) {
            std::printf("<Give Audio Status> got nothing back: %s\n",
                        device.lastError().c_str());
            std::printf(
                "\nThat device does not implement the audio feature set, so a\n"
                "volume readout is not possible from it. The volume buttons may\n"
                "still work, or may not - the two are separate.\n");
            return 2;
        }

        const unsigned level = status & 0x7F;
        std::printf("<Report Audio Status> = 0x%02X\n", status);
        std::printf("  muted  : %s\n", (status & 0x80) ? "YES" : "no");
        if (level == 0x7F) {
            std::printf("  volume : not reported (0x7F means it declines to say)\n");
            return 2;
        }
        std::printf("  volume : %u of 100\n", level);
        std::printf("\nThis device can report its volume, so a readout will work here.\n");
        return 0;
    }

    if (command == "set-volume") {
        const int target = (argc - rest >= 1) ? std::atoi(argv[rest]) : -1;
        if (target < 0 || target > 100) {
            std::printf("Usage: CecTest set-volume <0-100>\n\n"
                        "Tries CEC 2.0's absolute volume and reports whether\n"
                        "anything happened. Pick a level well away from the\n"
                        "current one so a change is unmistakable.\n");
            return 1;
        }
        if (!JoinBus(device)) return 1;
        return CmdSetVolume(device, target);
    }

    if (command == "power") {
        if (!JoinBus(device)) return 1;
        const cec::PowerStatus status = device.QueryPowerStatus();
        if (status == cec::PowerStatus::Unknown) {
            std::printf("TV did not answer the power-status query.\n");
            return 2;
        }
        std::printf("TV power status: %s (0x%02X)\n",
                    cec::PowerStatusName(status), static_cast<uint8_t>(status));
        return 0;
    }

    if (command == "read") {
        uint32_t address = 0;
        if (argc - rest < 2 || !ParseHex(argv[rest], address)) {
            PrintUsage();
            return 1;
        }
        const uint32_t length = static_cast<uint32_t>(std::atoi(argv[rest + 1]));
        std::vector<uint8_t> data;
        if (!backend->ReadDpcd(port, address, length, data)) {
            std::printf("Read FAILED: %s\n", backend->LastError().c_str());
            return 1;
        }
        std::printf("0x%05X -> ", address);
        PrintHex(data);
        std::printf("\n");
        return 0;
    }

    if (command == "write") {
        uint32_t address = 0;
        std::vector<uint8_t> data;
        if (argc - rest < 2 || !ParseHex(argv[rest], address) ||
            !ParseHexBytes(argc, argv, rest + 1, data)) {
            PrintUsage();
            return 1;
        }
        if (!backend->WriteDpcd(port, address, data)) {
            std::printf("Write FAILED: %s\n", backend->LastError().c_str());
            return 1;
        }
        std::printf("0x%05X <- ", address);
        PrintHex(data);
        std::printf("\n");
        return 0;
    }

    if (command == "cec-send") {
        std::vector<uint8_t> frame;
        if (!ParseHexBytes(argc, argv, rest, frame)) {
            PrintUsage();
            return 1;
        }
        // The header carries the source address, so a raw frame is the one
        // place a caller may deliberately want to send from wherever this
        // happens to be. Tunnelling still has to be on for anything to leave.
        if (!device.Initialise()) {
            std::printf("Could not enable CEC tunnelling: %s\n",
                        device.lastError().c_str());
            return 1;
        }
        cec::TxResult tx;
        const bool ok = device.SendFrame(frame, &tx);
        std::printf("TX  ");
        PrintHex(frame);
        std::printf("\n");
        PrintTx("result", tx, ok);
        if (!ok) std::printf("  %s\n", device.lastError().c_str());
        return ok ? 0 : 1;
    }

    if (command == "input-test") {
        return CmdInputTest(device, (argc - rest >= 1) ? argv[rest] : "");
    }

    if (command == "standby" || command == "on") {
        const int rc = CmdAttach(device);
        if (rc != 0) return rc;

        const bool standby = (command == "standby");
        std::printf("\n%s\n", standby ? "<Standby>" : "<Image View On>");
        const bool ok = standby ? device.SendStandby() : device.SendImageViewOn();
        if (!ok) {
            std::printf("  FAILED: %s\n", device.lastError().c_str());
            return 1;
        }
        std::printf("  sent\n");
        return 0;
    }

    PrintUsage();
    return 1;
}
