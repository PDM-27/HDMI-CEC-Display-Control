#include "IntelBackend.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "version.lib")

namespace cec {
namespace {

// ---------------------------------------------------------------- the API
//
// Declared here rather than by including igcl_api.h, for the same reason the
// AMD backend declares its own struct: this is the whole of what is used, it is
// a handful of lines, and vendoring a 9000-line header to reach five functions
// would be the larger commitment. The layout is Intel's, verbatim; if it ever
// disagrees with theirs, theirs is right.

constexpr uint32_t kAuxMaxDataSize = 132;

constexpr int kOperationRead = 1;
constexpr int kOperationWrite = 2;

constexpr uint32_t kAuxFlagNativeAux = 1u << 0;   // DPCD
constexpr uint32_t kAuxFlagI2cAux = 1u << 1;      // EDID and other I2C
constexpr uint32_t kAuxFlagI2cAuxMot = 1u << 2;   // middle of transaction

// AuxBackend hands over the 8-bit I2C write address, which is what the NVIDIA
// and AMD paths want. IGCL addresses the device itself, so the read/write bit
// comes off.
uint32_t SevenBit(uint8_t deviceAddress) {
    return static_cast<uint32_t>(deviceAddress >> 1);
}

constexpr uint32_t kResultSuccess = 0x00000000;
constexpr uint32_t kResultUnsupportedVersion = 0x40000009;
constexpr uint32_t kResultInsufficientPermissions = 0x40000006;
// CTL_RESULT_ERROR_INVALID_AUX_ACCESS_FLAG, which is what an elevated EDID
// read on a working Alder Lake laptop returns: the flags, not the rights.
constexpr uint32_t kResultInvalidAuxFlag = 0x48000001;
constexpr uint32_t kResultPlatformNotSupported = 0x40000020;

struct CtlApplicationId {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};

struct CtlInitArgs {
    uint32_t Size;
    uint8_t Version;
    uint32_t AppVersion;         // ctl_version_info_t is a uint32_t
    uint32_t flags;
    uint32_t SupportedVersion;   // [out]
    CtlApplicationId ApplicationUID;
};

struct CtlAuxAccessArgs {
    uint32_t Size;
    uint8_t Version;
    int32_t OpType;              // 1 read, 2 write
    uint32_t Flags;
    uint32_t Address;
    uint64_t RAD;                // branch devices; Intel marks it future use
    uint32_t PortID;             // SST tiled; likewise
    uint32_t DataSize;
    uint8_t Data[kAuxMaxDataSize];
};

using CtlInitFn = uint32_t(__cdecl*)(CtlInitArgs*, void**);
using CtlCloseFn = uint32_t(__cdecl*)(void*);
using CtlEnumerateDevicesFn = uint32_t(__cdecl*)(void*, uint32_t*, void**);
using CtlEnumerateDisplayOutputsFn = uint32_t(__cdecl*)(void*, uint32_t*, void**);
using CtlAuxAccessFn = uint32_t(__cdecl*)(void*, CtlAuxAccessArgs*);

uint32_t MakeVersion(uint32_t major, uint32_t minor) {
    return (major << 16) | (minor & 0xFFFF);
}

// --------------------------------------------------------------- the DLL

// Intel's driver is supposed to put ControlLib.dll in System32, and on the
// machine this was first tried on it had not - the only copy was in the driver
// store, under the graphics driver's own package folder. Loading it from there
// is the difference between "this driver has no IGCL" and "this driver has IGCL
// and the installer skipped a step".
//
// LOAD_WITH_ALTERED_SEARCH_PATH so that whatever the DLL depends on resolves
// from beside it rather than from beside us.
HMODULE LoadControlLib(std::string* whence) {
    if (HMODULE direct = LoadLibraryW(L"ControlLib.dll")) {
        if (whence) *whence = "System32";
        return direct;
    }

    wchar_t root[MAX_PATH] = {};
    if (!GetSystemDirectoryW(root, MAX_PATH)) return nullptr;
    std::wstring pattern =
        std::wstring(root) + L"\\DriverStore\\FileRepository\\*";

    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return nullptr;

    HMODULE library = nullptr;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.cFileName[0] == L'.') continue;
        const std::wstring candidate = std::wstring(root) +
                                       L"\\DriverStore\\FileRepository\\" +
                                       found.cFileName + L"\\ControlLib.dll";
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        library = LoadLibraryExW(candidate.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
        if (library) {
            if (whence) {
                char narrow[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, narrow,
                                    sizeof(narrow) - 1, nullptr, nullptr);
                *whence = std::string("driver store, ") + narrow;
            }
            break;
        }
    } while (FindNextFileW(search, &found));

    FindClose(search);
    return library;
}

}  // namespace

std::string IntelProbeVersions() {
    std::string report;
    char line[256];

    std::string whence;
    HMODULE library = LoadControlLib(&whence);
    if (!library) {
        return "ControlLib.dll not found in System32 or the driver store.\n";
    }
    snprintf(line, sizeof(line), "ControlLib.dll: %s\n", whence.c_str());
    report += line;

    // The DLL's own file version, which says which driver branch built it.
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(library, path, MAX_PATH)) {
            DWORD ignored = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
            std::vector<uint8_t> block(size);
            VS_FIXEDFILEINFO* info = nullptr;
            UINT infoLen = 0;
            if (size && GetFileVersionInfoW(path, 0, size, block.data()) &&
                VerQueryValueW(block.data(), L"\\",
                               reinterpret_cast<void**>(&info), &infoLen) &&
                info) {
                snprintf(line, sizeof(line), "  file version: %u.%u.%u.%u\n",
                         HIWORD(info->dwFileVersionMS),
                         LOWORD(info->dwFileVersionMS),
                         HIWORD(info->dwFileVersionLS),
                         LOWORD(info->dwFileVersionLS));
                report += line;
            }
        }
    }

    auto init = reinterpret_cast<CtlInitFn>(GetProcAddress(library, "ctlInit"));
    auto close =
        reinterpret_cast<CtlCloseFn>(GetProcAddress(library, "ctlClose"));
    if (!init) {
        FreeLibrary(library);
        return report + "  ctlInit is not exported.\n";
    }
    snprintf(line, sizeof(line), "  sizeof(init args) = %u\n",
             static_cast<unsigned>(sizeof(CtlInitArgs)));
    report += line;

    // Plausible versions, then values no implementation could support. If
    // 99.99 is refused exactly like 1.0, the version is not the objection.
    const uint32_t candidates[] = {
        MakeVersion(1, 0), MakeVersion(1, 1), MakeVersion(1, 2),
        MakeVersion(1, 3), MakeVersion(1, 4), MakeVersion(1, 5),
        MakeVersion(2, 0), MakeVersion(2, 1), MakeVersion(0, 1),
        MakeVersion(99, 99),
    };
    // flags 0, then CTL_INIT_FLAG_USE_LEVEL_ZERO, which Intel's samples set.
    const uint32_t flagSets[] = {0u, 1u};

    for (uint32_t flags : flagSets) {
        snprintf(line, sizeof(line), "  flags = 0x%X\n", flags);
        report += line;
        for (uint32_t version : candidates) {
            CtlInitArgs args = {};
            args.Size = sizeof(args);
            args.Version = 0;
            args.AppVersion = version;
            args.flags = flags;
            void* api = nullptr;
            const uint32_t result = init(&args, &api);
            snprintf(line, sizeof(line),
                     "    app %2u.%-2u -> 0x%08X   supported %u.%u%s\n",
                     static_cast<unsigned>(version >> 16),
                     static_cast<unsigned>(version & 0xFFFF), result,
                     static_cast<unsigned>(args.SupportedVersion >> 16),
                     static_cast<unsigned>(args.SupportedVersion & 0xFFFF),
                     result == kResultSuccess ? "   <-- ACCEPTED" : "");
            report += line;
            if (result == kResultSuccess && api && close) close(api);
        }
    }

    FreeLibrary(library);
    return report;
}

IntelBackend::~IntelBackend() {
    if (api_ && closeFn_) {
        reinterpret_cast<CtlCloseFn>(closeFn_)(api_);
    }
    if (library_) FreeLibrary(static_cast<HMODULE>(library_));
}

bool IntelBackend::Open() {
    // Already open, and this guard is the one the other two backends have had
    // all along. Without it every attach did the whole thing again, and on this
    // backend that is expensive three times over: the library was loaded again
    // without the old handle being released, the driver store was walked from
    // scratch to find it on a machine where it is not in System32, and ctlInit
    // was called again with its result overwriting api_ - leaking the context
    // the last call returned. Once per attach is once per wake on a laptop.
    if (auxFn_ && api_) return true;

    // Loaded once and kept, so a retry costs nothing but the ctlInit below.
    if (!library_) {
        HMODULE library = LoadControlLib(&whence_);
        if (!library) {
            lastError_ =
                "ControlLib.dll not found; the Intel graphics driver either "
                "predates IGCL or did not install it";
            return false;
        }
        library_ = library;
    }
    auto module = static_cast<HMODULE>(library_);

    initFn_ = GetProcAddress(module, "ctlInit");
    closeFn_ = GetProcAddress(module, "ctlClose");
    enumDevicesFn_ = GetProcAddress(module, "ctlEnumerateDevices");
    enumOutputsFn_ = GetProcAddress(module, "ctlEnumerateDisplayOutputs");
    auxFn_ = GetProcAddress(module, "ctlAUXAccess");
    if (!initFn_ || !closeFn_ || !enumDevicesFn_ || !enumOutputsFn_ || !auxFn_) {
        lastError_ = "ControlLib.dll (" + whence_ +
                     ") is missing one of the functions this needs";
        return false;
    }

    // Version 1.0, deliberately, and asked exactly once.
    //
    // Both halves of that were learned the hard way on a Kaby Lake Surface Pro
    // 5. Its ControlLib.dll is 1.0.93.0, so an earlier version of this asked
    // for 1.1 first and got UNSUPPORTED_VERSION - and then the 1.0 retry
    // returned the *same* error rather than the real one, because ctlInit is
    // stateful: only the first call in a process gives a true answer, and every
    // call after it repeats the first refusal. The fallback loop meant to be
    // robust was hiding the actual reason.
    //
    // Asked on its own, 1.0 reported SupportedVersion 1.0 and failed with
    // PLATFORM_NOT_SUPPORTED, which is the answer that was wanted. So: no
    // retry loop, and ask for the oldest interface that has everything this
    // needs. ctlEnumerateDevices, ctlEnumerateDisplayOutputs and ctlAUXAccess
    // are all in 1.0, and IGCL is backward compatible, so a newer driver will
    // accept it too.
    CtlInitArgs args = {};
    args.Size = sizeof(args);
    args.Version = 0;
    args.AppVersion = MakeVersion(1, 0);
    void* api = nullptr;
    const uint32_t result = reinterpret_cast<CtlInitFn>(initFn_)(&args, &api);
    if (result == kResultSuccess && api) {
        api_ = api;
        return true;
    }

    if (result == kResultPlatformNotSupported) {
        // Worth saying in words. IGCL is documented as Alder Lake-P and newer,
        // and this is that gate speaking: the library loads and runs on older
        // integrated graphics, accepts the version, and then declines the
        // hardware. Nothing about it is fixable from here.
        lastError_ =
            "this Intel GPU is too old for IGCL, which starts at 12th "
            "generation (Alder Lake-P); the driver's own answer is "
            "PLATFORM_NOT_SUPPORTED";
        return false;
    }

    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "ctlInit failed (0x%08X); ControlLib.dll came from %s", result,
             whence_.c_str());
    lastError_ = buffer;
    if (result == kResultUnsupportedVersion && args.SupportedVersion) {
        char detail[64];
        snprintf(detail, sizeof(detail), ", which implements %u.%u",
                 static_cast<unsigned>(args.SupportedVersion >> 16),
                 static_cast<unsigned>(args.SupportedVersion & 0xFFFF));
        lastError_ += detail;
    }
    return false;
}

bool IntelBackend::EnumeratePorts(std::vector<AuxPort>& out) {
    if (!api_) {
        lastError_ = "not initialised";
        return false;
    }

    uint32_t deviceCount = 0;
    uint32_t result =
        reinterpret_cast<CtlEnumerateDevicesFn>(enumDevicesFn_)(api_,
                                                                &deviceCount,
                                                                nullptr);
    if (result != kResultSuccess || !deviceCount) {
        lastError_ = "ctlEnumerateDevices reported no adapters";
        return false;
    }

    std::vector<void*> devices(deviceCount, nullptr);
    result = reinterpret_cast<CtlEnumerateDevicesFn>(enumDevicesFn_)(
        api_, &deviceCount, devices.data());
    if (result != kResultSuccess) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "ctlEnumerateDevices failed (0x%08X)",
                 result);
        lastError_ = buffer;
        return false;
    }

    // Deliberately not cleared.
    //
    // AuxPort carries a uint32_t, not a handle, so portId is an index into this
    // - and CecDevice keeps a copy of its AuxPort for as long as it lives.
    // Clearing and rebuilding would silently repoint an existing port at a
    // different display whenever enumeration order changed. Reusing the slot a
    // handle already has keeps an issued id meaning what it meant. The list
    // only grows by however many distinct outputs the driver ever reports,
    // which is a handful.
    for (uint32_t index = 0; index < deviceCount; ++index) {
        if (!devices[index]) continue;

        uint32_t outputCount = 0;
        if (reinterpret_cast<CtlEnumerateDisplayOutputsFn>(enumOutputsFn_)(
                devices[index], &outputCount, nullptr) != kResultSuccess ||
            !outputCount) {
            continue;
        }
        std::vector<void*> handles(outputCount, nullptr);
        if (reinterpret_cast<CtlEnumerateDisplayOutputsFn>(enumOutputsFn_)(
                devices[index], &outputCount, handles.data()) !=
            kResultSuccess) {
            continue;
        }

        for (uint32_t which = 0; which < outputCount; ++which) {
            if (!handles[which]) continue;

            // Narrowed on nothing, deliberately.
            //
            // The AMD backend learned this the expensive way: every
            // descriptive field a vendor offers about what is plugged in can
            // lie, and in Session 0 all of them did, while AUX itself worked
            // perfectly. IGCL has ctlGetDisplayProperties and an attached flag,
            // and asking it here would be an optimisation that saves a failed
            // DPCD read and risks a false negative. FindCecPort's read of
            // 0x3000 is the only test that has ever meant the same thing
            // everywhere.
            AuxPort port;
            port.backend = "intel";
            port.adapterName = "Intel Graphics";
            port.adapterIndex = index;
            port.portId = IndexForOutput(handles[which]);
            port.isActive = true;
            out.push_back(port);
        }
    }

    if (out.empty()) {
        // outputs_ is deliberately never cleared - see the note above - so it
        // is the wrong thing to judge this call by.
        lastError_ = "no display outputs enumerated";
        return false;
    }
    return true;
}

uint32_t IntelBackend::IndexForOutput(void* handle) {
    for (size_t at = 0; at < outputs_.size(); ++at) {
        if (outputs_[at] == handle) return static_cast<uint32_t>(at);
    }
    outputs_.push_back(handle);
    return static_cast<uint32_t>(outputs_.size() - 1);
}

bool IntelBackend::Transact(const AuxPort& port, int operation, uint32_t flags,
                            uint32_t address, uint8_t* data, uint32_t length) {
    if (!api_ || port.portId >= outputs_.size()) {
        lastError_ = "not initialised";
        return false;
    }
    if (!length || length > kAuxMaxDataSize) {
        lastError_ = "bad transfer length";
        return false;
    }

    CtlAuxAccessArgs args = {};
    args.Size = sizeof(args);
    args.Version = 0;
    args.OpType = operation;
    args.Flags = flags;
    args.Address = address;
    args.DataSize = length;
    if (operation == kOperationWrite) memcpy(args.Data, data, length);

    const uint32_t result = reinterpret_cast<CtlAuxAccessFn>(auxFn_)(
        outputs_[port.portId], &args);
    if (result != kResultSuccess) {
        // Named where it is known, because the first version blamed every
        // failed write on privileges. An elevated run then returned a
        // different code, which read as "still a rights problem" when it was
        // the library refusing the flags outright - a difference that decides
        // whether the answer is to elevate or to find another way entirely.
        const char* meaning = "";
        if (result == kResultInsufficientPermissions) {
            meaning = "; IGCL requires administrator privileges for writes";
        } else if (result == kResultInvalidAuxFlag) {
            meaning = "; IGCL rejected the AUX flags, which is what it does to "
                      "the I2C write an EDID read begins with";
        }
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "AUX %s at 0x%05X failed (0x%08X)%s",
                 operation == kOperationWrite ? "write" : "read", address,
                 result, meaning);
        lastError_ = buffer;
        return false;
    }

    if (operation == kOperationRead) {
        memcpy(data, args.Data, length < args.DataSize ? length : args.DataSize);
    }
    return true;
}

bool IntelBackend::ReadDpcd(const AuxPort& port, uint32_t address,
                            uint32_t length, std::vector<uint8_t>& out) {
    out.assign(length, 0);
    return Transact(port, kOperationRead, kAuxFlagNativeAux, address, out.data(),
                    length);
}

bool IntelBackend::WriteDpcd(const AuxPort& port, uint32_t address,
                             const std::vector<uint8_t>& data) {
    if (data.empty()) return true;
    std::vector<uint8_t> copy = data;
    return Transact(port, kOperationWrite, kAuxFlagNativeAux, address,
                    copy.data(), static_cast<uint32_t>(copy.size()));
}

bool IntelBackend::ReadI2c(const AuxPort& port, uint8_t device, uint32_t length,
                           std::vector<uint8_t>& out) {
    out.assign(length, 0);
    return Transact(port, kOperationRead, kAuxFlagI2cAux, SevenBit(device),
                    out.data(), length);
}

bool IntelBackend::WriteI2c(const AuxPort& port, uint8_t device,
                            const std::vector<uint8_t>& data) {
    if (data.empty()) return true;
    std::vector<uint8_t> copy = data;
    // MOT - middle of transaction - is what makes this a real I2C sequence
    // rather than two unrelated ones. It writes the byte offset and leaves the
    // bus held, so the read that follows starts from there.
    //
    // The first version of this put nothing on the wire at all: it stashed the
    // offset and passed it to the read as the AUX address, which is where the
    // I2C slave address belongs. That would have read from a device numbered
    // after the offset, which is not a device.
    return Transact(port, kOperationWrite, kAuxFlagI2cAux | kAuxFlagI2cAuxMot,
                    SevenBit(device), copy.data(),
                    static_cast<uint32_t>(copy.size()));
}

}  // namespace cec
