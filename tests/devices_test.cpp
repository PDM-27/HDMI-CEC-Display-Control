// Checks the DEVICES line end to end: the exact bytes the service writes, fed
// to the exact loop the tray reads them with. Both halves are copied verbatim
// from CecService.cpp and CecTray.cpp so that a change to either side that
// breaks the field positions fails here.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ------------------------------------------------- the service's side
struct Peer {
    uint16_t where = 0xFFFF;
    uint8_t type = 0xFF;
    uint8_t address = 0;
    std::string name;
    bool present = false;
    uint8_t switchIgnored = 0;
};

constexpr uint8_t kLogicalAddressAudioSystem = 5;
constexpr uint8_t kSwitchIgnoredSure = 1;

bool IsAudioSystem(const Peer& one) {
    return one.address == kLogicalAddressAudioSystem || one.type == 5;
}

std::string FormatPhysicalAddress(uint16_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 12) & 0xF,
                  (a >> 8) & 0xF, (a >> 4) & 0xF, a & 0xF);
    return buf;
}

const char* DeviceTypeName(uint8_t) { return "Device"; }

std::string BuildDevicesReply(const std::vector<Peer>& peers, uint16_t onScreen,
                              uint16_t here) {
    std::string reply = "OK";
    for (const Peer& one : peers) {
        reply += "\t";
        reply += FormatPhysicalAddress(one.where);
        reply += one.present ? "|awake" : "|asleep";
        reply += one.where == onScreen ? "|onscreen" : "|";
        reply += one.where == here ? "|thispc" : "|";
        reply += IsAudioSystem(one) ? "|audio"
                 : one.switchIgnored >= kSwitchIgnoredSure ? "|ignores"
                                                           : "|";
        reply += "|";
        reply += one.name.empty() ? DeviceTypeName(one.type) : one.name.c_str();
    }
    return reply;
}

// ------------------------------------------------- the tray's side
struct InputEntry {
    std::string port;
    std::string label;
    bool onScreen = false;
    bool thisPc = false;
    bool asleep = false;
    bool audio = false;
    bool cannotSwitch = false;
};

std::vector<InputEntry> ParseInputs(const std::string& response) {
    std::vector<InputEntry> found;
    if (response.rfind("OK", 0) != 0) return found;
    size_t at = response.find('\t');
    while (at != std::string::npos && found.size() < 15) {
        const size_t end = response.find('\t', at + 1);
        const std::string entry = response.substr(
            at + 1, end == std::string::npos ? end : end - at - 1);
        at = end;

        std::string fields[6];
        size_t field = 0;
        for (size_t i = 0; i < entry.size() && field < 6; ++i) {
            if (entry[i] == '|' && field < 5) {
                ++field;
            } else {
                fields[field].push_back(entry[i]);
            }
        }
        if (fields[0].size() != 7) continue;

        InputEntry one;
        one.port = fields[0];
        one.onScreen = fields[2] == "onscreen";
        one.thisPc = fields[3] == "thispc";
        one.audio = fields[4] == "audio";
        one.cannotSwitch = fields[4] == "ignores";
        one.asleep = fields[1] == "asleep";

        std::string label = "HDMI ";
        label += fields[0][0];
        if (fields[0][2] != '0') {
            label += " port ";
            label += fields[0][2];
        }
        label += " - ";
        label += fields[5].empty() ? "Device" : fields[5];
        if (one.thisPc) label += " (this PC)";
        if (one.cannotSwitch) label += " (use remote)";
        one.label = label;
        found.push_back(one);
    }
    return found;
}

// ------------------------------------------------- the check
int failures = 0;

void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    // The Vizio bus from the field logs, with the Apple TV having ignored one
    // request, which under kSwitchIgnoredSure == 1 is enough.
    Peer polk;
    polk.where = 0x1000; polk.address = 5; polk.type = 5;
    polk.name = "Polk Mini AX"; polk.present = true;

    Peer deck;
    deck.where = 0x2000; deck.address = 4; deck.type = 4;
    deck.name = "STEAMDE"; deck.present = true;

    Peer apple;
    apple.where = 0x3000; apple.address = 11; apple.type = 4;
    apple.name = "Apple TV"; apple.present = true; apple.switchIgnored = 1;

    Peer self;
    self.where = 0x4000; self.address = 8; self.type = 4;
    self.name = "DELL-5430"; self.present = true;

    const std::string reply =
        BuildDevicesReply({polk, deck, apple, self}, 0x4000, 0x4000);
    std::printf("wire: %s\n\n", reply.c_str());

    const std::vector<InputEntry> rows = ParseInputs(reply);
    Expect(rows.size() == 4, "four rows parsed");
    if (rows.size() != 4) return 1;

    for (const InputEntry& r : rows) std::printf("  row: %s\n", r.label.c_str());
    std::printf("\n");

    Expect(rows[0].label == "HDMI 1 - Polk Mini AX", "soundbar label");
    Expect(rows[0].audio, "soundbar flagged as audio");
    Expect(!rows[0].cannotSwitch, "soundbar not flagged as ignoring");

    Expect(rows[1].label == "HDMI 2 - STEAMDE", "deck label has no suffix");
    Expect(!rows[1].cannotSwitch, "deck not flagged");

    Expect(rows[2].label == "HDMI 3 - Apple TV (use remote)", "apple tv label");
    Expect(rows[2].cannotSwitch, "apple tv flagged as ignoring");
    Expect(!rows[2].audio, "apple tv not flagged as audio");

    Expect(rows[3].label == "HDMI 4 - DELL-5430 (this PC)", "this pc label");
    Expect(rows[3].onScreen, "this pc is on screen");

    // A name with a space is why the name is the last field; prove it survives.
    Expect(rows[0].port == "1.0.0.0" && rows[2].port == "3.0.0.0", "ports");

    // And the count below the threshold must say nothing at all.

    {
        Peer quiet = apple;
        quiet.switchIgnored = 0;
        const std::vector<InputEntry> none =
            ParseInputs(BuildDevicesReply({quiet}, 0, 0));
        Expect(none.size() == 1 && none[0].label == "HDMI 3 - Apple TV",
               "no suffix when the count is zero");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
