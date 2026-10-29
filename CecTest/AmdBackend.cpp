#include "AmdBackend.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace cec {
namespace {

constexpr int kAdlOk = 0;
constexpr int kAdlErrInvalidParam = -3;
constexpr int kAdlMaxPath = 256;
constexpr int kMaxEdidData = 256;
constexpr int kEdidBlock = 128;

// A DisplayPort AUX burst is at most 16 bytes, which is why the request struct
// carries its data inline rather than pointing at it.
constexpr uint32_t kAuxChunk = 16;

constexpr int kOperationRead = 0;
constexpr int kOperationWrite = 1;

constexpr int kConnectorDisplayPort = 15;
constexpr int kDisplayConnected = 0x00000001;
constexpr int kDisplayMapped = 0x00000002;

constexpr uint8_t kEdidI2cAddress = 0xA0;

using AdlContext = void*;
using AdlMallocCallback = void*(__stdcall*)(int);

void* __stdcall AdlMalloc(int size) { return std::malloc(static_cast<size_t>(size)); }

struct AdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[kAdlMaxPath];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[kAdlMaxPath];
    char strDisplayName[kAdlMaxPath];
    int iPresent;
    int iExist;
    char strDriverPath[kAdlMaxPath];
    char strDriverPathExt[kAdlMaxPath];
    char strPNPString[kAdlMaxPath];
    int iOSDisplayIndex;
};

struct AdlDisplayId {
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
};

struct AdlDisplayInfo {
    AdlDisplayId displayID;
    int iDisplayControllerIndex;
    char strDisplayName[kAdlMaxPath];
    char strDisplayManufacturerName[kAdlMaxPath];
    int iDisplayType;
    int iDisplayOutputType;
    int iDisplayConnector;
    int iDisplayInfoMask;
    int iDisplayInfoValue;
};

struct AdlDisplayEdidData {
    int iSize;
    int iFlag;
    int iEDIDSize;
    int iBlockIndex;
    char cEDIDData[kMaxEdidData];
    int iReserved[4];
};

// The undocumented one. Recovered by experiment, not guessed:
//
//   iSize      36, and only 36. Every other size is rejected outright.
//   iStatus    written by the driver, comes back 1. Left zero going in.
//   iOperation 0 reads, 1 writes. Confirmed by writing to the CEC tunneling
//              control register and reading the value back.
//   iAddress   DPCD address. Reading 0x00001 returned the max link rate byte,
//              and 0x3000 the CEC tunneling capability, both exactly as the
//              DisplayPort specification requires them to be.
//   iDataSize  1 to 16. Reading address 0 with each length in turn returned
//              one more byte of the receiver capability field each time.
//   data       inline, 16 bytes, which is why the struct is 36 and not a
//              multiple of 8: there is no pointer in it.
struct AdlNativeAuxAccess {
    int iSize;
    int iStatus;
    int iOperation;
    int iAddress;
    int iDataSize;
    unsigned char data[16];
};
static_assert(sizeof(AdlNativeAuxAccess) == 36,
              "the driver rejects any size but 36");

using Adl2MainControlCreate = int(*)(AdlMallocCallback, int, AdlContext*);
using Adl2MainControlDestroy = int(*)(AdlContext);
using Adl2AdapterNumberOfAdaptersGet = int(*)(AdlContext, int*);
using Adl2AdapterAdapterInfoGet = int(*)(AdlContext, AdapterInfo*, int);
using Adl2DisplayDisplayInfoGet = int(*)(AdlContext, int, int*, AdlDisplayInfo**,
                                         int);
using Adl2DisplayEdidDataGet = int(*)(AdlContext, int, int, AdlDisplayEdidData*);
using Adl2DisplayNativeAuxAccess = int(*)(AdlContext, int, int,
                                          AdlNativeAuxAccess*);

}  // namespace

AmdBackend::~AmdBackend() {
    if (context_ && destroyFn_) {
        reinterpret_cast<Adl2MainControlDestroy>(destroyFn_)(context_);
    }
    if (library_) FreeLibrary(static_cast<HMODULE>(library_));
}

bool AmdBackend::Open() {
    // auxFn_ is what "ready" means, not library_. Keying off the library would
    // let a run that loaded the DLL but failed to resolve the entry point latch
    // as successful and never recover - the same mistake NvidiaBackend made
    // once and had to be corrected for.
    if (auxFn_ && context_) return true;

    if (!library_) {
        library_ = LoadLibraryW(L"atiadlxx.dll");
        if (!library_) {
            // Not an error worth reporting loudly: a machine simply may not
            // have an AMD GPU.
            lastError_ = "atiadlxx.dll not present";
            return false;
        }
    }

    auto module = static_cast<HMODULE>(library_);
    auto resolve = [&](const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };

    void* createFn = resolve("ADL2_Main_Control_Create");
    destroyFn_ = resolve("ADL2_Main_Control_Destroy");
    adapterCountFn_ = resolve("ADL2_Adapter_NumberOfAdapters_Get");
    adapterInfoFn_ = resolve("ADL2_Adapter_AdapterInfo_Get");
    displayInfoFn_ = resolve("ADL2_Display_DisplayInfo_Get");
    edidFn_ = resolve("ADL2_Display_EdidData_Get");
    auxFn_ = resolve("ADL2_Display_NativeAUXChannel_Access");

    if (!createFn || !adapterCountFn_ || !adapterInfoFn_ || !displayInfoFn_) {
        lastError_ = "ADL is present but its documented entry points are not";
        return false;
    }
    if (!auxFn_) {
        lastError_ = "this AMD driver does not export NativeAUXChannel_Access";
        return false;
    }

    if (!context_) {
        // 1: enumerate connected adapters only.
        const int created = reinterpret_cast<Adl2MainControlCreate>(createFn)(
            AdlMalloc, 1, &context_);
        if (created != kAdlOk || !context_) {
            lastError_ = "ADL2_Main_Control_Create failed";
            context_ = nullptr;
            return false;
        }
    }
    return true;
}

bool AmdBackend::EnumeratePorts(std::vector<AuxPort>& out) {
    if (!Open()) return false;

    int adapters = 0;
    reinterpret_cast<Adl2AdapterNumberOfAdaptersGet>(adapterCountFn_)(context_,
                                                                      &adapters);
    if (adapters <= 0) {
        lastError_ = "ADL reports no adapters";
        return false;
    }

    std::vector<AdapterInfo> info(static_cast<size_t>(adapters));
    std::memset(info.data(), 0, info.size() * sizeof(AdapterInfo));
    if (reinterpret_cast<Adl2AdapterAdapterInfoGet>(adapterInfoFn_)(
            context_, info.data(),
            static_cast<int>(info.size() * sizeof(AdapterInfo))) != kAdlOk) {
        lastError_ = "ADL2_Adapter_AdapterInfo_Get failed";
        return false;
    }

    // Everything ADL will admit to, kept with the facts it reports about each
    // one. Which of those facts can be believed depends on the session this is
    // running in, so nothing is discarded while collecting.
    struct Candidate {
        AuxPort port;
        bool connected;
        bool displayPort;
    };
    std::vector<Candidate> candidates;
    int displaysSeen = 0;
    std::string connectorsSeen;

    // iPresent is not consulted, and that is the third field in a row to earn
    // that treatment. As SYSTEM this card reports four adapters and marks every
    // one of them absent; from a logged-in session the same call marks them
    // present. An adapter that does not exist fails its DPCD read, which costs
    // nothing and is the only test here that has proved to mean the same thing
    // in both sessions.
    for (const AdapterInfo& adapter : info) {
        int displays = 0;
        AdlDisplayInfo* list = nullptr;
        if (reinterpret_cast<Adl2DisplayDisplayInfoGet>(displayInfoFn_)(
                context_, adapter.iAdapterIndex, &displays, &list, 0) != kAdlOk ||
            !list) {
            continue;
        }
        displaysSeen += displays;

        for (int i = 0; i < displays; ++i) {
            const AdlDisplayInfo& display = list[i];

            char seen[8];
            snprintf(seen, sizeof(seen), "%d ", display.iDisplayConnector);
            if (connectorsSeen.find(seen) == std::string::npos) {
                connectorsSeen += seen;
            }

            // ADL presents one adapter entry per output on the same physical
            // GPU, so the same display can appear several times. They are left
            // in rather than filtered: FindCecPort probes each and keeps
            // whichever answers, and a duplicate costs one DPCD read.
            AuxPort port;
            port.backend = "amd";
            port.adapterName = adapter.strAdapterName;
            port.adapterIndex = static_cast<uint32_t>(adapter.iAdapterIndex);
            port.portId =
                static_cast<uint32_t>(display.displayID.iDisplayLogicalIndex);
            port.isActive = (display.iDisplayInfoValue & kDisplayMapped) != 0;
            candidates.push_back(
                {port, (display.iDisplayInfoValue & kDisplayConnected) != 0,
                 display.iDisplayConnector == kConnectorDisplayPort});
        }
        std::free(list);
    }

    // Three descriptions of the same hardware, in descending order of how much
    // ADL is willing to tell a given caller about it.
    //
    // From a logged-in session the first is exact: DisplayPort outputs, marked
    // connected, with a TV on the end of one. Running as SYSTEM in session 0 -
    // which is where the service lives - the same calls on the same machine
    // report adapters normally and then describe their outputs as neither
    // connected nor DisplayPort. Both fields turn out to say as much about the
    // caller's session as about the wire.
    //
    // Narrowing on them was never load-bearing. FindCecPort probes each port
    // and keeps whichever answers with CEC tunneling capability, which is a far
    // stronger statement about a display being there than any flag, so the
    // right move when a description is unavailable is to offer more and let the
    // read decide. A port that is genuinely empty costs one failed DPCD read.
    for (const Candidate& c : candidates) {
        if (c.connected && c.displayPort) out.push_back(c.port);
    }
    if (!out.empty()) return true;

    for (const Candidate& c : candidates) {
        if (c.displayPort) out.push_back(c.port);
    }
    if (!out.empty()) return true;

    for (const Candidate& c : candidates) out.push_back(c.port);
    if (!out.empty()) return true;

    // ADL described no outputs at all. The AUX call is addressed by adapter and
    // display index rather than by anything from this list, and index 0 is what
    // every adapter on this card answered to, so it is worth one read each
    // before giving up. If these fail too, ADL's AUX is genuinely session-bound
    // and an AMD service cannot reach the bus - which is a real answer, and the
    // error below is what says so.
    for (const AdapterInfo& adapter : info) {
        AuxPort port;
        port.backend = "amd";
        port.adapterName = adapter.strAdapterName;
        port.adapterIndex = static_cast<uint32_t>(adapter.iAdapterIndex);
        port.portId = 0;
        port.isActive = false;
        out.push_back(port);
    }
    if (!out.empty()) return true;

    char buffer[160];
    int present = 0;
    for (const AdapterInfo& adapter : info) present += adapter.iPresent ? 1 : 0;
    snprintf(buffer, sizeof(buffer),
             "%d adapters (%d marked present), %d displays, connectors: %s",
             adapters, present, displaysSeen,
             connectorsSeen.empty() ? "(none)" : connectorsSeen.c_str());
    lastError_ = buffer;
    return false;
}

bool AmdBackend::Transact(const AuxPort& port, int operation, uint32_t address,
                          uint8_t* data, uint32_t length) {
    if (!auxFn_ || !context_) {
        lastError_ = "AMD AUX not opened";
        return false;
    }
    if (length == 0 || length > kAuxChunk) {
        lastError_ = "AUX transfers are 1 to 16 bytes";
        return false;
    }

    AdlNativeAuxAccess request = {};
    request.iSize = static_cast<int>(sizeof(request));
    request.iOperation = operation;
    request.iAddress = static_cast<int>(address);
    request.iDataSize = static_cast<int>(length);
    if (operation == kOperationWrite) std::memcpy(request.data, data, length);

    const int result = reinterpret_cast<Adl2DisplayNativeAuxAccess>(auxFn_)(
        context_, static_cast<int>(port.adapterIndex),
        static_cast<int>(port.portId), &request);
    if (result != kAdlOk) {
        // -3 gets a hint attached, because it is what this driver answers to a
        // struct whose size it does not recognise. If AMD ever revises the
        // request, that is the error a newer driver would give, and the message
        // should point at the fix rather than leaving someone to rediscover the
        // whole problem from "AUX read failed".
        char buffer[224];
        snprintf(buffer, sizeof(buffer), "AUX %s at 0x%05X failed (ADL %d)%s",
                 operation == kOperationWrite ? "write" : "read", address,
                 result,
                 result == kAdlErrInvalidParam
                     ? "; this driver may expect a different request struct - "
                       "see AmdBackend.h for how the current one was found"
                     : "");
        lastError_ = buffer;
        return false;
    }

    if (operation == kOperationRead) std::memcpy(data, request.data, length);
    return true;
}

bool AmdBackend::ReadDpcd(const AuxPort& port, uint32_t address, uint32_t length,
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
        if (chunk > kAuxChunk) chunk = kAuxChunk;

        uint8_t buffer[kAuxChunk] = {};
        if (!Transact(port, kOperationRead,
                      address + static_cast<uint32_t>(out.size()), buffer,
                      chunk)) {
            return false;
        }
        out.insert(out.end(), buffer, buffer + chunk);
    }
    return true;
}

bool AmdBackend::WriteDpcd(const AuxPort& port, uint32_t address,
                           const std::vector<uint8_t>& data) {
    if (data.empty()) {
        lastError_ = "write data must not be empty";
        return false;
    }
    if (address > 0xFFFFF) {
        lastError_ = "DPCD address must be in range 0x00000..0xFFFFF";
        return false;
    }

    for (size_t offset = 0; offset < data.size(); offset += kAuxChunk) {
        const uint32_t chunk = static_cast<uint32_t>(
            (std::min)(static_cast<size_t>(kAuxChunk), data.size() - offset));
        uint8_t buffer[kAuxChunk] = {};
        std::memcpy(buffer, data.data() + offset, chunk);
        if (!Transact(port, kOperationWrite,
                      address + static_cast<uint32_t>(offset), buffer, chunk)) {
            return false;
        }
    }
    return true;
}

// EDID goes through ADL's own documented call rather than I2C-over-AUX.
//
// The native AUX request has no field for an I2C device address, so it cannot
// carry an I2C transaction at all - it is a DPCD interface. ADL exposes the
// EDID directly instead, which is what the I2C path was only ever being used to
// fetch, so the result is the same and the route is documented.
bool AmdBackend::WriteI2c(const AuxPort& port, uint8_t device,
                          const std::vector<uint8_t>& data) {
    (void)port;
    if (device != kEdidI2cAddress) {
        lastError_ = "AMD backend only serves the EDID over I2C";
        return false;
    }
    // The EDID reader writes a single byte to say where the next block starts.
    edidOffset_ = data.empty() ? 0 : data[0];
    return true;
}

bool AmdBackend::ReadI2c(const AuxPort& port, uint8_t device, uint32_t length,
                         std::vector<uint8_t>& out) {
    out.clear();
    if (device != kEdidI2cAddress) {
        lastError_ = "AMD backend only serves the EDID over I2C";
        return false;
    }
    if (!Open() || !edidFn_) {
        lastError_ = "ADL2_Display_EdidData_Get not available";
        return false;
    }

    AdlDisplayEdidData edid = {};
    edid.iSize = static_cast<int>(sizeof(edid));
    edid.iBlockIndex = static_cast<int>(edidOffset_ / kEdidBlock);

    if (reinterpret_cast<Adl2DisplayEdidDataGet>(edidFn_)(
            context_, static_cast<int>(port.adapterIndex),
            static_cast<int>(port.portId), &edid) != kAdlOk) {
        lastError_ = "ADL2_Display_EdidData_Get failed";
        return false;
    }

    const uint32_t within = edidOffset_ % kEdidBlock;
    const uint32_t available =
        (edid.iEDIDSize > static_cast<int>(within))
            ? static_cast<uint32_t>(edid.iEDIDSize) - within
            : 0;
    if (available < length) {
        lastError_ = "EDID block shorter than the requested read";
        return false;
    }

    // The driver's own count is trusted for how much is there, but not for how
    // much can be read: cEDIDData is a fixed 256 bytes, and a size larger than
    // that would walk off the end of the struct.
    if (within + length > kMaxEdidData) {
        lastError_ = "EDID read would run past the block";
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(edid.cEDIDData);
    out.assign(bytes + within, bytes + within + length);
    return true;
}

}  // namespace cec
