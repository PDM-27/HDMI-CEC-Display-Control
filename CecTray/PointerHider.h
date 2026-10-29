// PointerHider.h - hides the mouse pointer while the mouse is left alone.
//
// On a TV the pointer sits in the middle of whatever is playing. Windows only
// hides it while typing in some apps, and the tools that do more are old and
// fragile. This does the one job: after the mouse has been still for a while,
// the pointer goes; move the mouse or press a button and it is back.
//
// Hiding it while typing is deliberately not offered. Apps decide that for
// themselves: Windows' "Hide pointer while typing" is honoured by some and
// ignored by others, the Windows 11 Notepad hides the pointer whatever it says,
// and an option here could only ever disagree with one of them.
//
// How it hides: every standard pointer shape is swapped for a blank one with
// SetSystemCursor, and the user's own scheme - shapes, size, and colour - is
// reloaded with SPI_SETCURSORS to show it again. That is the one way that works
// in every app without special rights. Its danger is that the swap outlives the
// process, so every way out puts the pointer back first, and a marker in the
// registry lets the next start put it back if this process died with it hidden.
//
// How it watches: a timer, not a mouse hook. A low-level mouse hook sits in the
// path of every movement system-wide, which a 1000Hz gaming mouse notices;
// reading the pointer's position a few times a second costs nothing anyone can
// measure.
//
// UI thread only.
#pragma once

#include <windows.h>

namespace tray {

class PointerHider {
public:
    struct Options {
        bool enabled = false;
        DWORD idleMs = 5000;       // still for this long, and it hides
        int tolerance = 5;         // movement, at 96 DPI, that counts as moving
    };

    // At startup, before anything else could hide it: puts the pointer back if
    // the last run ended with it hidden. appKey is the tray's HKCU key, where
    // the marker lives, and is kept for the life of the process.
    void RestoreLeftover(const wchar_t* appKey);

    // Starts, stops, or changes the behaviour. The timer is on the given
    // window, which passes it to Tick.
    void Configure(HWND window, UINT_PTR timerId, const Options& options);

    // The timer.
    void Tick();

    // Shows the pointer now, if it is hidden. For locking, sleeping, signing
    // out, and exit. Watching carries on.
    void Show();

private:
    void Hide();
    void Settle(POINT point, DWORD now);   // the pointer moved, or was clicked
    void SetMarker(bool hidden);

    const wchar_t* appKey_ = L"";
    HWND window_ = nullptr;
    UINT_PTR timerId_ = 0;
    Options options_;
    bool hidden_ = false;
    POINT anchor_ = {};
    int threshold_ = 0;         // the tolerance in pixels where the pointer is
    DWORD lastActivity_ = 0;
};

}  // namespace tray
