#include "Edid.h"

#include <windows.h>
#include <setupapi.h>

#include <cstdio>

#pragma comment(lib, "setupapi.lib")

namespace cec {
namespace {

// GUID_DEVINTERFACE_MONITOR. Written out rather than linked from a library for
// one constant.
const GUID kMonitorInterface = {
    0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

constexpr size_t kEdidBlockSize = 128;

// Reading EDID over DDC is a two-step transaction: write the byte offset to
// the EEPROM, then read from it.
bool ReadEdidBlock(AuxBackend& backend, const AuxPort& port, uint8_t offset,
                   std::vector<uint8_t>& out) {
    if (!backend.WriteI2c(port, kEdidI2cAddress, {offset})) return false;
    return backend.ReadI2c(port, kEdidI2cAddress, kEdidBlockSize, out);
}

bool HasValidHeader(const std::vector<uint8_t>& block) {
    static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0x00};
    if (block.size() < kEdidBlockSize) return false;
    for (int i = 0; i < 8; ++i) {
        if (block[i] != kHeader[i]) return false;
    }
    return true;
}

bool ChecksumOk(const std::vector<uint8_t>& block, size_t start) {
    uint8_t sum = 0;
    for (size_t i = 0; i < kEdidBlockSize; ++i) {
        sum = static_cast<uint8_t>(sum + block[start + i]);
    }
    return sum == 0;
}

}  // namespace

bool ReadEdid(AuxBackend& backend, const AuxPort& port,
              std::vector<uint8_t>& out, std::string* error) {
    out.clear();

    std::vector<uint8_t> block;
    if (!ReadEdidBlock(backend, port, 0, block)) {
        if (error) *error = backend.LastError();
        return false;
    }
    if (!HasValidHeader(block)) {
        if (error) *error = "EDID header missing - I2C read returned junk";
        return false;
    }
    out = block;

    // Byte 126 counts extension blocks; the CEA-861 extension carrying the
    // HDMI VSDB is normally the first of them.
    const uint8_t extensions = block[126];
    for (uint8_t i = 1; i <= extensions; ++i) {
        // A byte offset only addresses the first 256 bytes. Beyond that, E-DDC
        // segment pointers are needed, which no sink of interest here requires.
        if (static_cast<size_t>(i) * kEdidBlockSize > 255) break;
        std::vector<uint8_t> ext;
        if (!ReadEdidBlock(backend, port,
                           static_cast<uint8_t>(i * kEdidBlockSize), ext)) {
            break;
        }
        out.insert(out.end(), ext.begin(), ext.end());
    }

    if (!ChecksumOk(out, 0) && error) {
        *error = "EDID base block checksum mismatch (data may be unreliable)";
    }
    return true;
}

bool ReadEdidFromWindows(std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    // Present devices only, so a television unplugged months ago is not
    // mistaken for the one on the other end of this cable.
    HDEVINFO set = SetupDiGetClassDevsW(&kMonitorInterface, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        if (error) *error = "Windows has no list of attached displays";
        return false;
    }

    int televisions = 0;
    SP_DEVINFO_DATA info = {sizeof(info)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) continue;
        // Grown and read again rather than given up on, which is what a
        // fixed buffer amounted to: 512 bytes holds a base block and three
        // extensions, every set seen here, but a display carrying more answered
        // ERROR_MORE_DATA and was skipped without a word. On Intel that is the
        // whole of it - the sink never answers a DDC read there, so Windows'
        // copy is the only address there is, and the machine would have had no
        // CEC at all for want of a few hundred bytes.
        std::vector<uint8_t> edid(512);
        DWORD size = static_cast<DWORD>(edid.size());
        LONG read =
            RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(), &size);
        if (read == ERROR_MORE_DATA && size > edid.size()) {
            edid.resize(size);
            read = RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(),
                                    &size);
        }
        RegCloseKey(key);
        if (read != ERROR_SUCCESS || size < 128) continue;
        edid.resize(size);

        uint16_t address = 0;
        if (!ParsePhysicalAddress(edid, address)) continue;   // not a television
        ++televisions;
        out = std::move(edid);
    }
    SetupDiDestroyDeviceInfoList(set);

    if (televisions == 1) return true;
    out.clear();
    if (error) {
        *error = televisions == 0
                     ? "Windows knows of no attached television"
                     : "Windows knows of more than one attached television, so "
                       "which one is on this port cannot be told";
    }
    return false;
}

bool ParsePhysicalAddress(const std::vector<uint8_t>& edid, uint16_t& out) {
    out = 0;

    for (size_t base = kEdidBlockSize; base + kEdidBlockSize <= edid.size();
         base += kEdidBlockSize) {
        if (edid[base] != 0x02) continue;          // CEA-861 extension tag
        const uint8_t dtdStart = edid[base + 2];   // where data blocks end
        if (dtdStart <= 4 || dtdStart > kEdidBlockSize) continue;

        size_t i = base + 4;
        const size_t end = base + dtdStart;
        while (i < end && i < edid.size()) {
            const uint8_t header = edid[i];
            const uint8_t tag = static_cast<uint8_t>(header >> 5);
            const uint8_t length = static_cast<uint8_t>(header & 0x1F);
            if (length == 0) break;

            // Tag 3 = Vendor Specific Data Block. HDMI 1.4's VSDB is the one
            // with IEEE registration ID 00-0C-03, stored little-endian, and
            // the two bytes right after it are the source physical address.
            if (tag == 3 && length >= 5 && i + 5 < edid.size()) {
                if (edid[i + 1] == 0x03 && edid[i + 2] == 0x0C &&
                    edid[i + 3] == 0x00) {
                    out = static_cast<uint16_t>((edid[i + 4] << 8) | edid[i + 5]);
                    return true;
                }
            }
            i += static_cast<size_t>(length) + 1;
        }
    }
    return false;
}

uint16_t ParsePhysicalAddress(const std::string& text) {
    // "2.0.0.0" back into the packed form. Anything malformed gives 0, which
    // every caller treats as no address rather than as the television's.
    //
    // Four fields, each read as a whole number. This took one character per
    // field, which is right for the fifteen addresses out of sixteen that fit
    // in a single digit and wrong for the last of them: a nibble runs to 15,
    // FormatPhysicalAddress prints it in decimal, and so "10.0.0.0" read back
    // as 1.0.0.0. That pair is a round trip in the one place it matters - the
    // service prints a device's address into DEVICES, the tray hands the same
    // text back in CMD switch-to - so a port numbered that high would have been
    // answered by switching to a different one, with nothing to say so.
    uint16_t address = 0;
    size_t at = 0;
    for (int field = 0; field < 4; ++field) {
        if (field && (at >= text.size() || text[at++] != '.')) return 0;
        if (at >= text.size() || text[at] < '0' || text[at] > '9') return 0;
        unsigned value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = value * 10 + static_cast<unsigned>(text[at++] - '0');
            if (value > 15) return 0;
        }
        address = static_cast<uint16_t>((address << 4) | value);
    }
    return at == text.size() ? address : 0;
}

std::string FormatPhysicalAddress(uint16_t address) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (address >> 12) & 0xF,
                  (address >> 8) & 0xF, (address >> 4) & 0xF, address & 0xF);
    return buf;
}

bool PlausiblePhysicalAddress(uint16_t address) {
    if (!address || address == 0xFFFF) return false;
    bool ended = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const unsigned digit = (address >> shift) & 0xF;
        if (!digit) {
            ended = true;
        } else if (ended) {
            return false;
        }
    }
    return true;
}

std::string ParseMonitorName(const std::vector<uint8_t>& edid) {
    if (edid.size() < kEdidBlockSize) return {};
    // Four 18-byte descriptors start at offset 54. A display descriptor opens
    // with three zero bytes and carries its tag in the fourth, so a monitor
    // name reads 00 00 00 FC 00 and then up to 13 characters ended by 0x0A.
    // This once tested the fifth byte for the tag, which is always zero, so no
    // name was ever found - unnoticed while the result only fed the bench tool.
    for (size_t d = 54; d + 18 <= kEdidBlockSize; d += 18) {
        if (edid[d] || edid[d + 1] || edid[d + 2] || edid[d + 3] != 0xFC) continue;
        std::string name;
        for (size_t i = d + 5; i < d + 18; ++i) {
            if (edid[i] == 0x0A) break;
            name += static_cast<char>(edid[i]);
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name;
    }
    return {};
}

bool ParseDisplayIdentity(const std::vector<uint8_t>& edid, DisplayIdentity& out) {
    // ReadEdid only warns about a bad checksum, because a physical address
    // from a slightly damaged block is still better than none. An identity is
    // different: it decides which stored settings apply, so it is taken only
    // from a block that checks out.
    if (!HasValidHeader(edid) || !ChecksumOk(edid, 0)) return false;

    // Bytes 8-9, big-endian: three letters of five bits each, 1 meaning 'A'.
    // The top bit is reserved and always zero on a real sink.
    const uint32_t packed = (static_cast<uint32_t>(edid[8]) << 8) | edid[9];
    if (packed & 0x8000) return false;
    char letters[4] = {};
    for (int i = 0; i < 3; ++i) {
        const uint32_t code = (packed >> (10 - 5 * i)) & 0x1F;
        if (code < 1 || code > 26) return false;
        letters[i] = static_cast<char>('A' + code - 1);
    }

    // Product code at 10-11 and serial at 12-15, both little-endian. Plenty of
    // televisions leave the serial at zero, in which case two identical sets
    // share one identity - which is the outcome anyone would pick for them.
    const uint32_t product = edid[10] | (static_cast<uint32_t>(edid[11]) << 8);
    const uint32_t serial = edid[12] | (static_cast<uint32_t>(edid[13]) << 8) |
                            (static_cast<uint32_t>(edid[14]) << 16) |
                            (static_cast<uint32_t>(edid[15]) << 24);
    char key[24];
    std::snprintf(key, sizeof(key), "%s-%04X-%08X", letters,
                  static_cast<unsigned>(product), static_cast<unsigned>(serial));
    out.key = key;

    out.name.clear();
    for (const char c : ParseMonitorName(edid)) {
        if (c >= 0x20 && c <= 0x7E) out.name += c;
    }
    return true;
}

}  // namespace cec
