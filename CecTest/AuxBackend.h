// AuxBackend.h - vendor-agnostic DisplayPort AUX access.
//
// Each GPU vendor exposes AUX through a completely different API
// (NVIDIA: NvAPI_Disp_DpAuxChannelControl, AMD: ADL_Display_NativeAUXChannel_Access,
// Intel: ctlAUXAccess), but all three end up doing the same thing: read and
// write DPCD registers on a given output. Everything above this interface -
// the CEC protocol, the service, the tray app - only needs that much.
//
// Vendor detection is deliberately empirical rather than static. On hybrid
// laptops the external port is often wired only to the integrated GPU, so the
// discrete chip's AUX API would fail for want of a physical wire, not a bug.
// FindCecPort() therefore probes every port of every backend and keeps whichever
// one actually answers with CEC tunneling capability.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cec {

// One DisplayPort output on some GPU, from some vendor.
struct AuxPort {
    std::string backend;      // "nvidia", "amd", "intel"
    std::string adapterName;  // human-readable GPU name
    uint32_t adapterIndex = 0;
    uint32_t portId = 0;      // vendor-specific display/output identifier
    bool isActive = false;

    std::string Describe() const {
        return adapterName + " [" + backend + " port 0x" +
               [&] {
                   char buf[16];
                   snprintf(buf, sizeof(buf), "%08X", portId);
                   return std::string(buf);
               }() + "]";
    }
};

class AuxBackend {
public:
    virtual ~AuxBackend() = default;

    // Vendor name, for logging.
    virtual const char* Name() const = 0;

    // Brings the vendor API up. Returns false if this vendor's driver is not
    // present at all, which is normal and not an error - a machine simply may
    // not have this vendor's GPU.
    virtual bool Open() = 0;

    virtual bool EnumeratePorts(std::vector<AuxPort>& out) = 0;

    virtual bool ReadDpcd(const AuxPort& port, uint32_t address, uint32_t length,
                          std::vector<uint8_t>& out) = 0;
    virtual bool WriteDpcd(const AuxPort& port, uint32_t address,
                           const std::vector<uint8_t>& data) = 0;

    // I2C-over-AUX, used to reach the sink's EDID at device address 0xA0.
    // `device` is the 8-bit I2C write address, so it must be even.
    virtual bool ReadI2c(const AuxPort& port, uint8_t device, uint32_t length,
                         std::vector<uint8_t>& out) = 0;
    virtual bool WriteI2c(const AuxPort& port, uint8_t device,
                          const std::vector<uint8_t>& data) = 0;

    virtual const std::string& LastError() const = 0;
};

// Every backend compiled into this build, whether or not the hardware is
// present. Callers are expected to Open() each and ignore the ones that fail.
std::vector<std::unique_ptr<AuxBackend>> CreateAllBackends();

}  // namespace cec
