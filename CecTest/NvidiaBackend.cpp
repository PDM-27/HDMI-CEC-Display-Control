#include "NvidiaBackend.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include "nvapi.h"

// nvapi_QueryInterface is the generic dispatcher every real NVAPI entry point
// is reached through. It is exported from nvapi64.dll but is NOT a public
// symbol in nvapi64.lib, so it has to be resolved at runtime rather than
// linked. The static lib is still used for the documented calls below.
using QueryInterfaceFn = void*(__cdecl*)(unsigned int id);

namespace cec {
namespace {

// Confirmed against jNizM's reversed ID list and arcnmx/nvapi-rs (nvid.rs).
// Not present in the public nvapi.h or nvapi_interface.h.
constexpr unsigned int kDpAuxChannelControlId = 0x8EB56969;

// Values for _NvDpAuxRequest::command. 2/3/6 are the I2C-over-AUX variants,
// used to reach the sink's EDID; 3 vs 6 is whether the transaction is closed
// (middle-of-transaction reads keep it open).
enum AuxCommand : uint32_t {
    kAuxDpcdWrite    = 0,
    kAuxDpcdRead     = 1,
    kAuxI2cWrite     = 2,
    kAuxI2cReadFinal = 3,
    kAuxI2cRead      = 6,
};

constexpr uint32_t kAuxChunkSize = 16;
constexpr int kRetryCount = 10;
constexpr DWORD kRetryDelayMs = 10;

// The version word encodes a 40-byte (0x28) versioned header, which is
// deliberately smaller than sizeof(NvDpAuxRequest) == 0x68. The trailing
// reserved[0x40] is scratch space the driver may use but that is not covered
// by the version. The size argument passed to the call is the full 0x68.
// Do not "fix" this mismatch - both values come from a working implementation
// (gpu_aux) and changing either is likely to break the call.
constexpr uint32_t kDpAuxRequestVersion = 0x00010028;
constexpr uint32_t kDpAuxRequestSize = 0x68;

#pragma pack(push, 1)
struct NvDpAuxRequest {
    uint32_t version;
    uint32_t displayId;
    uint32_t command;
    uint32_t address;
    uint8_t  data[kAuxChunkSize];
    uint32_t lengthField;   // always (payload length - 1)
    uint32_t result;        // per-transaction status, separate from NvAPI_Status
    uint8_t  reserved[0x40];
};
#pragma pack(pop)

static_assert(sizeof(NvDpAuxRequest) == kDpAuxRequestSize,
              "NvDpAuxRequest must be exactly 0x68 bytes");

using DpAuxChannelControlFn = NvAPI_Status(__cdecl*)(NvU32 displayId,
                                                     NvDpAuxRequest* request,
                                                     NvU32 size);

const char* AuxResultText(uint32_t result) {
    switch (result) {
        case 0x00: return "success";
        case 0x01: return "defer";
        case 0x02: return "requested data not found";
        case 0xFF: return "timeout";
        default:   return "unknown";
    }
}

std::string NvStatusText(NvAPI_Status status) {
    NvAPI_ShortString buf = {};
    if (NvAPI_GetErrorMessage(status, buf) == NVAPI_OK) return buf;
    return "NVAPI error " + std::to_string(static_cast<int>(status));
}

}  // namespace

NvidiaBackend::~NvidiaBackend() {
    if (opened_) {
        NvAPI_Unload();
        opened_ = false;
    }
    auxFn_ = nullptr;
}

bool NvidiaBackend::Open() {
    // auxFn_, not opened_, is what "ready" means. Keying off opened_ meant a
    // run that initialised NVAPI but then failed to resolve the entry point
    // latched as successful and could never recover - every later call returned
    // true with a null function pointer. It also makes Open() cheap to call
    // repeatedly, which the attach path now relies on.
    if (auxFn_) return true;

    if (!opened_) {
        NvAPI_Status status = NvAPI_Initialize();
        if (status != NVAPI_OK) {
            lastError_ = "NvAPI_Initialize failed: " + NvStatusText(status);
            return false;
        }
        opened_ = true;
    }

    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) {
        lastError_ = "nvapi64.dll could not be loaded";
        return false;
    }
    auto queryInterface = reinterpret_cast<QueryInterfaceFn>(
        reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface")));
    if (!queryInterface) {
        lastError_ = "nvapi64.dll does not export nvapi_QueryInterface";
        return false;
    }

    auxFn_ = queryInterface(kDpAuxChannelControlId);
    if (!auxFn_) {
        lastError_ = "nvapi_QueryInterface(0x8EB56969) returned null - this "
                     "driver does not expose DpAuxChannelControl";
        return false;
    }
    return true;
}

bool NvidiaBackend::EnumeratePorts(std::vector<AuxPort>& out) {
    out.clear();

    NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 gpuCount = 0;
    NvAPI_Status status = NvAPI_EnumPhysicalGPUs(gpus, &gpuCount);
    if (status != NVAPI_OK) {
        lastError_ = "NvAPI_EnumPhysicalGPUs failed: " + NvStatusText(status);
        return false;
    }

    for (NvU32 i = 0; i < gpuCount; ++i) {
        NvAPI_ShortString name = {};
        std::string gpuName = (NvAPI_GPU_GetFullName(gpus[i], name) == NVAPI_OK)
                                  ? name : "NVIDIA GPU";

        NvU32 idCount = 0;
        if (NvAPI_GPU_GetConnectedDisplayIds(gpus[i], nullptr, &idCount, 0) != NVAPI_OK
            || idCount == 0) {
            continue;
        }

        std::vector<NV_GPU_DISPLAYIDS> ids(idCount);
        for (auto& id : ids) id.version = NV_GPU_DISPLAYIDS_VER1;
        if (NvAPI_GPU_GetConnectedDisplayIds(gpus[i], ids.data(), &idCount, 0) != NVAPI_OK) {
            continue;
        }

        for (NvU32 j = 0; j < idCount; ++j) {
            const NV_GPU_DISPLAYIDS& id = ids[j];
            // Only real, physically attached DisplayPort sinks can carry AUX.
            if (!id.isConnected || !id.isPhysicallyConnected) continue;
            if (id.connectorType != NV_MONITOR_CONN_TYPE_DP) continue;

            AuxPort port;
            port.backend = "nvidia";
            port.adapterName = gpuName;
            port.adapterIndex = i;
            port.portId = id.displayId;
            port.isActive = id.isActive != 0;
            out.push_back(port);
        }
    }
    return true;
}

bool NvidiaBackend::Transact(const AuxPort& port, uint32_t command, uint32_t address,
               const uint8_t* data, uint32_t dataLen, uint32_t lengthField,
               uint8_t* outData) {
    if (!auxFn_) {
        lastError_ = "AUX entry point not resolved; call Open() first";
        return false;
    }

    NvDpAuxRequest request = {};
    request.version = kDpAuxRequestVersion;
    request.displayId = port.portId;
    request.command = command;
    request.address = address;
    request.lengthField = lengthField;
    if (data && dataLen) std::memcpy(request.data, data, dataLen);

    auto fn = reinterpret_cast<DpAuxChannelControlFn>(auxFn_);

    // A call can return NVAPI_OK while request.result still reports a deferred
    // or timed-out transaction, so both are checked before accepting a result.
    for (int attempt = 0; attempt < kRetryCount; ++attempt) {
        NvDpAuxRequest attemptReq = request;
        NvAPI_Status status = fn(port.portId, &attemptReq, kDpAuxRequestSize);
        if (status == NVAPI_OK && attemptReq.result == 0) {
            // The whole burst, not the requested length, so outData is always a
            // 16-byte buffer - both callers declare one and use the first
            // `chunk` bytes of it.
            if (outData) std::memcpy(outData, attemptReq.data, kAuxChunkSize);
            return true;
        }
        if (attempt == kRetryCount - 1) {
            if (status != NVAPI_OK) {
                lastError_ = "DpAuxChannelControl failed: " + NvStatusText(status);
            } else {
                lastError_ = std::string("AUX transaction failed: ") +
                             AuxResultText(attemptReq.result);
            }
        }
        if (attempt + 1 < kRetryCount) Sleep(kRetryDelayMs);
    }
    return false;
}

bool NvidiaBackend::ReadDpcd(const AuxPort& port, uint32_t address, uint32_t length,
                   std::vector<uint8_t>& out) {
    out.clear();
    if (length == 0) {
        lastError_ = "read length must be positive";
        return false;
    }
    if (address > 0xFFFFF) {
        lastError_ = "DPCD address must be in range 0x00000..0xFFFFF";
        return false;
    }

    while (out.size() < length) {
        uint32_t chunk = length - static_cast<uint32_t>(out.size());
        if (chunk > kAuxChunkSize) chunk = kAuxChunkSize;

        uint8_t buf[kAuxChunkSize] = {};
        if (!Transact(port, kAuxDpcdRead, address + static_cast<uint32_t>(out.size()),
                  nullptr, 0, chunk - 1, buf)) {
            return false;
        }
        out.insert(out.end(), buf, buf + chunk);
    }
    return true;
}

bool NvidiaBackend::WriteDpcd(const AuxPort& port, uint32_t address,
                    const std::vector<uint8_t>& data) {
    if (data.empty()) {
        lastError_ = "write data must not be empty";
        return false;
    }
    if (address > 0xFFFFF) {
        lastError_ = "DPCD address must be in range 0x00000..0xFFFFF";
        return false;
    }

    for (size_t offset = 0; offset < data.size(); offset += kAuxChunkSize) {
        uint32_t chunk = static_cast<uint32_t>(
            (std::min)(static_cast<size_t>(kAuxChunkSize), data.size() - offset));
        if (!Transact(port, kAuxDpcdWrite, address + static_cast<uint32_t>(offset),
                  data.data() + offset, chunk, chunk - 1, nullptr)) {
            return false;
        }
    }
    return true;
}

bool NvidiaBackend::ReadI2c(const AuxPort& port, uint8_t device,
                            uint32_t length, std::vector<uint8_t>& out) {
    out.clear();
    if (length == 0) {
        lastError_ = "read length must be positive";
        return false;
    }
    if (device & 1) {
        lastError_ = "I2C write address must be even";
        return false;
    }

    while (out.size() < length) {
        uint32_t chunk = length - static_cast<uint32_t>(out.size());
        if (chunk > kAuxChunkSize) chunk = kAuxChunkSize;
        const bool final = (out.size() + chunk) >= length;

        // Middle reads keep the transaction open (MOT set); only the last one
        // closes it, which is what the two distinct read commands encode.
        uint8_t buf[kAuxChunkSize] = {};
        if (!Transact(port, final ? kAuxI2cReadFinal : kAuxI2cRead, device,
                      nullptr, 0, chunk - 1, buf)) {
            return false;
        }
        out.insert(out.end(), buf, buf + chunk);
    }
    return true;
}

bool NvidiaBackend::WriteI2c(const AuxPort& port, uint8_t device,
                             const std::vector<uint8_t>& data) {
    if (data.empty()) {
        lastError_ = "write data must not be empty";
        return false;
    }
    if (device & 1) {
        lastError_ = "I2C write address must be even";
        return false;
    }

    for (size_t offset = 0; offset < data.size(); offset += kAuxChunkSize) {
        const uint32_t chunk = static_cast<uint32_t>(
            (std::min)(static_cast<size_t>(kAuxChunkSize), data.size() - offset));
        if (!Transact(port, kAuxI2cWrite, device, data.data() + offset, chunk,
                      chunk - 1, nullptr)) {
            return false;
        }
    }
    return true;
}

}  // namespace cec
