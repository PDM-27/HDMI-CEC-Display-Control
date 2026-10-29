// Edid.h - reading the sink's EDID over I2C-over-AUX, to discover this PC's
// CEC physical address.
//
// The physical address is not a constant: it encodes which HDMI port of the TV
// the signal arrives on (port 1 -> 1.0.0.0, port 2 -> 2.0.0.0, and so on), and
// gets announced to the CEC bus in <Report Physical Address> and
// <Active Source>. Hardcoding it works on exactly one machine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AuxBackend.h"

namespace cec {

// Standard DDC/EDID I2C device address, in 8-bit write form.
constexpr uint8_t kEdidI2cAddress = 0xA0;

// Reads the base EDID block plus its extension blocks, if any.
bool ReadEdid(AuxBackend& backend, const AuxPort& port,
              std::vector<uint8_t>& out, std::string* error);

// The same EDID, from Windows' own record of it rather than from the sink.
//
// Windows keeps a copy of every attached display's EDID, read when the display
// was plugged in, and reading it needs no graphics driver co-operation at all.
// That matters because Intel's control library refuses the I2C write that a
// real DDC read begins with, so on Intel this is the only way to learn which
// HDMI port this PC arrives on.
//
// Only televisions are considered, which here means a display whose EDID
// carries a CEC address. A PC almost always has exactly one, and anything else
// - a plain monitor, a laptop's own panel - is passed over. Fails when there
// is more than one, since there is then no way to tell which is on the port
// being asked about, and a wrong address is worse than none.
bool ReadEdidFromWindows(std::vector<uint8_t>& out, std::string* error);

// Finds the CEC physical address in the HDMI Vendor Specific Data Block of a
// CEA-861 extension (identified by IEEE OUI 00-0C-03). Returns false when the
// sink advertises no HDMI VSDB at all, which is normal for a plain DP monitor.
bool ParsePhysicalAddress(const std::vector<uint8_t>& edid, uint16_t& out);

// The reverse, for an address that has come back over the pipe as text.
// Returns 0 for anything malformed.
uint16_t ParsePhysicalAddress(const std::string& text);

// "1.0.0.0" style formatting of a packed physical address.
std::string FormatPhysicalAddress(uint16_t address);

// Whether a packed physical address is a shape the HDMI tree can actually
// produce, for addresses that arrive over the wire rather than out of an EDID.
//
// The four digits are depth, not a number: the first is the television's own
// port, the second a port on whatever is plugged into it, and so on. A branch
// that ends has zeroes the rest of the way down, so once a digit is zero every
// digit after it must be too. 3.1.0.0 is a device behind a switch; 3.0.1.0
// cannot exist, because there is no switch for it to be behind.
//
// This exists because the CH7218's receive buffer tears frames, pairing a stale
// opcode with a fresh payload, and the halves that come out of it look like
// addresses. One of them, 3.0.1.0, got into a field report's device list as a
// port nothing could be switched to, and sat there being polled for the rest of
// the session. Taken from a capture on a four-device bus: 2.0.4.5, 0.0.4.13 and
// 4.9.5.4 all arrived inside a single <Routing Change> that claimed five
// addresses where that message carries two.
//
// 0.0.0.0 is the television itself and 15.15.15.15 is "unknown", neither of
// which is an input, so both are refused here as well.
bool PlausiblePhysicalAddress(uint16_t address);

// Human-readable monitor name from the EDID descriptor blocks, for logging.
std::string ParseMonitorName(const std::vector<uint8_t>& edid);

// Which television this is, from the fields EDID sets aside for exactly that.
struct DisplayIdentity {
    // Manufacturer, product code and serial, as "TCL-2E5A-00000001". The same
    // for a given set every time it is read, and independent of which HDMI
    // port it is plugged into, since the port lives in the physical address
    // and not here.
    std::string key;
    // The monitor name descriptor, printable ASCII only. For the log.
    std::string name;
};

// Fills `out` only from an EDID that is intact: the header, the base block
// checksum, and a manufacturer ID made of real letters. A read that tore or
// returned junk fails here rather than producing a plausible stranger, which
// is what would otherwise turn one bad read into a "new" television.
bool ParseDisplayIdentity(const std::vector<uint8_t>& edid, DisplayIdentity& out);

}  // namespace cec
