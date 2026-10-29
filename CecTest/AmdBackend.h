// AmdBackend.h - DisplayPort AUX through AMD's Display Library.
//
// The function this rests on, ADL2_Display_NativeAUXChannel_Access, is exported
// by every AMD driver but documented only to holders of an AMD NDA. Its
// parameter struct was recovered by experiment against a real card, by
// sweeping values and watching which ones the driver answered differently:
// it accepts a size of 36 and nothing else, with the data carried inline.
//
// Everything above AuxBackend is unchanged by this file existing, which is what
// that interface was for.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AuxBackend.h"

namespace cec {

class AmdBackend : public AuxBackend {
public:
    ~AmdBackend() override;

    const char* Name() const override { return "amd"; }

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
    // One AUX transaction, read or write, of at most 16 bytes.
    bool Transact(const AuxPort& port, int operation, uint32_t address,
                  uint8_t* data, uint32_t length);

    void* library_ = nullptr;   // atiadlxx.dll
    void* context_ = nullptr;   // ADL_CONTEXT_HANDLE
    void* auxFn_ = nullptr;
    void* edidFn_ = nullptr;
    void* adapterCountFn_ = nullptr;
    void* adapterInfoFn_ = nullptr;
    void* displayInfoFn_ = nullptr;
    void* destroyFn_ = nullptr;

    // Where the next EDID read starts. Set by WriteI2c, which is how the EDID
    // reader asks for an offset before reading a block.
    uint32_t edidOffset_ = 0;

    std::string lastError_;
};

}  // namespace cec
