// IntelBackend.h - DisplayPort AUX through Intel's Graphics Control Library.
//
// The easiest of the three, and the only one that is genuinely documented:
// ctlAUXAccess and its argument struct are in Intel's own public header at
// github.com/intel/drivers.gpu.control-library. Nothing here was recovered by
// experiment the way AMD's had to be.
//
// It is also the only backend that reaches both DPCD and the EDID through one
// call: CTL_AUX_FLAG_NATIVE_AUX and CTL_AUX_FLAG_I2C_AUX are two flags on the
// same function. NVAPI needed a separate I2C entry point and AMD's native call
// cannot carry an I2C transaction at all.
//
// Two things about it are awkward, and both are handled here rather than by the
// caller:
//
//   - ControlLib.dll is shipped with the graphics driver and is supposed to be
//     copied into System32. On at least one machine - a Surface Pro 5 running
//     Intel's own 31.0.101.2141 - it exists only in the driver store, so this
//     looks there too rather than concluding the driver has no IGCL.
//   - Writes require administrator privileges, per Intel's comment on OpType.
//     Reads do not, which means a non-elevated process can probe happily and
//     then fail the moment it tries to enable CEC tunneling.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AuxBackend.h"

namespace cec {

class IntelBackend : public AuxBackend {
public:
    ~IntelBackend() override;

    const char* Name() const override { return "intel"; }

    bool Open() override;
    bool EnumeratePorts(std::vector<AuxPort>& out) override;

    bool ReadDpcd(const AuxPort& port, uint32_t address, uint32_t length,
                  std::vector<uint8_t>& out) override;
    bool WriteDpcd(const AuxPort& port, uint32_t address,
                   const std::vector<uint8_t>& data) override;

    bool ReadI2c(const AuxPort& port, uint8_t device, uint32_t length,
                 std::vector<uint8_t>& out) override;
    bool WriteI2c(const AuxPort& port, uint8_t device,
                  const std::vector<uint8_t>& data) override;

    const std::string& LastError() const override { return lastError_; }

private:
    // One AUX transaction. `flags` selects native DPCD or I2C-over-AUX.
    bool Transact(const AuxPort& port, int operation, uint32_t flags,
                  uint32_t address, uint8_t* data, uint32_t length);

    // The slot this output handle already occupies, appending it if it is new.
    uint32_t IndexForOutput(void* handle);

    void* library_ = nullptr;    // ControlLib.dll
    std::string whence_;         // where it was found, for the error messages
    void* api_ = nullptr;        // ctl_api_handle_t
    void* initFn_ = nullptr;
    void* closeFn_ = nullptr;
    void* enumDevicesFn_ = nullptr;
    void* enumOutputsFn_ = nullptr;
    void* auxFn_ = nullptr;

    // Display output handles, indexed by AuxPort::portId. IGCL identifies an
    // output only by an opaque handle, so unlike the other two backends there
    // is no natural numeric id to carry around; the index into this is it.
    std::vector<void*> outputs_;

    std::string lastError_;
};

// Diagnostic sweep, for a machine where ctlInit refuses.
//
// UNSUPPORTED_VERSION is what Intel returns when the app asks for an interface
// the driver does not implement - but it is also, plausibly, what a legacy
// driver returns for hardware IGCL does not cover, in which case no version
// will ever be accepted. Those two look identical from one call.
//
// The AMD struct in section 9 was found by the same reasoning: what identifies
// a correct value is one input being answered *differently* from the others,
// not which code comes back. So this walks the version space, including values
// no real implementation could support, and reports every answer. If the absurd
// versions fail exactly like the plausible ones, the version is not what is
// being rejected.
std::string IntelProbeVersions();

}  // namespace cec
