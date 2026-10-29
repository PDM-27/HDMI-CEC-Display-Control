// AudioOutputs.h - the list of Windows sound outputs, and choosing one.
//
// For the device picker in the volume panel. Kept apart from the panel, which
// only draws, and from the tray, which only decides where volume goes: this is
// the one place that talks to Windows about which output is the default.
//
// UI thread only, with COM already initialised on it, as the tray does at
// startup. Every call acquires what it needs fresh and lets it go before
// returning, because outputs come and go - headphones unplugged, a TV switched
// off - and a held interface would describe a device that is no longer there.
#pragma once

#include <string>
#include <vector>

namespace tray {

struct AudioOutput {
    std::wstring id;     // the endpoint ID Windows uses; opaque
    std::wstring name;   // what to show, as named in Windows' sound settings
    bool isDefault = false;
};

// The outputs that can play right now, in Windows' own order. Empty if Windows
// could not be asked, which the panel treats the same as having nothing to pick.
std::vector<AudioOutput> ListAudioOutputs();

// Whether this copy of Windows lets the default output be changed at all. Asked
// once; see the note on IPolicyConfig in AudioOutputs.cpp.
bool CanChooseAudioOutput();

// Makes an output the default for playback, as choosing it in Windows' own
// sound list does. Returns false if Windows refused, most often because the
// device has just gone away.
bool ChooseAudioOutput(const std::wstring& id);

}  // namespace tray
