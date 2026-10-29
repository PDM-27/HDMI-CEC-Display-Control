#include "AudioOutputs.h"

#include <windows.h>
#include <mmdeviceapi.h>

namespace tray {
namespace {

// {a45c254e-df1c-4efd-8020-67d146a850e0}: 2 is the device's own name, the part
// a person can rename in Windows' sound settings ("Living Room TV"); 14 is that
// name with the driver's appended ("Living Room TV (NVIDIA High Definition
// Audio)").
constexpr PROPERTYKEY kDeviceNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
constexpr PROPERTYKEY kFullNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Windows has no documented way for an app to change the default output. This
// is the interface its own sound settings use, and the one every app that
// switches outputs relies on: EarTrumpet, SoundSwitch, and others. It has kept
// the same identity and layout since Windows 7. Only SetDefaultEndpoint is
// called; the entries before it are declared purely to hold their places in
// the table, and their signatures do not matter because they are never used.
//
// If a future Windows removes it, creating it fails, CanChooseAudioOutput says
// no, and the panel simply shows no picker. Nothing else depends on it.
constexpr CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID kPolicyConfigIid = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR id, ERole role) = 0;
};

IPolicyConfig* CreatePolicyConfig() {
    IPolicyConfig* config = nullptr;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL,
                                kPolicyConfigIid,
                                reinterpret_cast<void**>(&config)))) {
        return nullptr;
    }
    return config;
}

std::wstring ReadString(IPropertyStore* store, const PROPERTYKEY& key) {
    std::wstring text;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal) {
        text = value.pwszVal;
    }
    PropVariantClear(&value);
    return text;
}

}  // namespace

std::vector<AudioOutput> ListAudioOutputs() {
    std::vector<AudioOutput> outputs;
    std::vector<std::wstring> fullNames;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) {
        return outputs;
    }

    std::wstring defaultId;
    IMMDevice* current = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &current))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(current->GetId(&id)) && id) defaultId = id;
        CoTaskMemFree(id);
        current->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                 &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;
            LPWSTR id = nullptr;
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
                AudioOutput output;
                output.id = id;
                output.name = ReadString(store, kDeviceNameKey);
                output.isDefault = output.id == defaultId;
                std::wstring full = ReadString(store, kFullNameKey);
                if (full.empty()) full = output.name;
                if (output.name.empty()) output.name = full;
                if (output.name.empty()) output.name = L"Sound output";
                outputs.push_back(std::move(output));
                fullNames.push_back(std::move(full));
                store->Release();
            }
            CoTaskMemFree(id);
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();

    // Two outputs with the same short name - "Speakers" twice, or an earbud's
    // "Headphones" and "Headset" pair under one name - cannot be told apart in
    // a list. Those, and only those, show the driver's part as well.
    //
    // Decided for all of them first and applied afterwards, because replacing
    // as it went undid the comparison for the rest: the first of a pair took
    // its full name, and the second then matched nothing and kept the short one.
    // A list showing "Speakers (NVIDIA High Definition Audio)" above a plain
    // "Speakers" is the half-done version of exactly the fix this is.
    std::vector<bool> ambiguous(outputs.size(), false);
    for (size_t i = 0; i < outputs.size(); ++i) {
        for (size_t j = 0; j < outputs.size(); ++j) {
            if (i != j &&
                _wcsicmp(outputs[i].name.c_str(), outputs[j].name.c_str()) == 0) {
                ambiguous[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (ambiguous[i] && !fullNames[i].empty()) outputs[i].name = fullNames[i];
    }
    return outputs;
}

bool CanChooseAudioOutput() {
    // Asked once. Whether the interface exists does not change while the
    // process runs, and creating it is not free.
    static const bool available = [] {
        IPolicyConfig* config = CreatePolicyConfig();
        if (config) config->Release();
        return config != nullptr;
    }();
    return available;
}

bool ChooseAudioOutput(const std::wstring& id) {
    if (id.empty()) return false;
    IPolicyConfig* config = CreatePolicyConfig();
    if (!config) return false;
    // Console and multimedia, the defaults that games, players, and browsers
    // follow. The communications default is left alone, so a headset someone
    // keeps for calls stays their call device.
    const bool ok = SUCCEEDED(config->SetDefaultEndpoint(id.c_str(), eConsole)) &&
                    SUCCEEDED(config->SetDefaultEndpoint(id.c_str(), eMultimedia));
    config->Release();
    return ok;
}

}  // namespace tray
