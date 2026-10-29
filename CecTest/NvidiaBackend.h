// NvidiaBackend.h - AuxBackend implementation over NVIDIA NVAPI.
//
// Wraps the undocumented NvAPI_Disp_DpAuxChannelControl entry point, which is
// reachable only through nvapi_QueryInterface() with a hardcoded 32-bit hash.
// The ID is not in NVIDIA's public headers; it comes from community lists of
// NVAPI's interface IDs, and NvidiaBackend.cpp says which and why the entry
// point has to be resolved at run time rather than linked.
#pragma once

#include "AuxBackend.h"

namespace cec {

class NvidiaBackend : public AuxBackend {
public:
    ~NvidiaBackend() override;

    const char* Name() const override { return "nvidia"; }
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
    bool Transact(const AuxPort& port, uint32_t command, uint32_t address,
                  const uint8_t* data, uint32_t dataLen, uint32_t lengthField,
                  uint8_t* outData);

    void* auxFn_ = nullptr;   // NvAPI_Disp_DpAuxChannelControl
    bool opened_ = false;
    std::string lastError_;
};

}  // namespace cec
