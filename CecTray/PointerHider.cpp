#include "PointerHider.h"

#include <shellscalingapi.h>

#include <cstdlib>

namespace tray {
namespace {

// Every shape Windows draws the pointer as: normal, text, busy, cross, up
// arrow, the four resize arrows and move, unavailable, link, working in the
// background, help, and the pin and person shapes Windows 10 added. Written as
// numbers because the OCR_ names need OEMRESOURCE defined before windows.h.
constexpr DWORD kPointerShapes[] = {32512, 32513, 32514, 32515, 32516, 32642,
                                    32643, 32644, 32645, 32646, 32648, 32649,
                                    32650, 32651, 32671, 32672};

// Polled slowly while shown, since only the idle time depends on it, and fast
// while hidden, so it is back within a frame or two of the mouse moving.
constexpr UINT kShownPollMs = 250;
constexpr UINT kHiddenPollMs = 25;

constexpr wchar_t kMarkerValue[] = L"PointerHidden";

bool AnyMouseButtonDown() {
    return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
             GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) |
             GetAsyncKeyState(VK_XBUTTON2)) &
            0x8000) != 0;
}

// The tolerance is in the pixels of a 96 DPI screen, so it is the same distance
// on a 4K TV at 200% as on a laptop at 100%.
int Threshold(int tolerance, POINT point) {
    UINT dpiX = 96, dpiY = 96;
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiX = 96;
    }
    return MulDiv(tolerance, static_cast<int>(dpiX), 96);
}

}  // namespace

void PointerHider::RestoreLeftover(const wchar_t* appKey) {
    appKey_ = appKey ? appKey : L"";
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, appKey_, kMarkerValue, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS &&
        value) {
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
        SetMarker(false);
    }
}

void PointerHider::SetMarker(bool hidden) {
    if (hidden) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, appKey_, 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &key, nullptr) == ERROR_SUCCESS) {
            const DWORD one = 1;
            RegSetValueExW(key, kMarkerValue, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(key);
        }
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, appKey_, kMarkerValue);
    }
}

void PointerHider::Configure(HWND window, UINT_PTR timerId, const Options& options) {
    window_ = window;
    timerId_ = timerId;
    options_ = options;
    if (!options_.enabled || !window_) {
        Show();
        if (window_) KillTimer(window_, timerId_);
        return;
    }
    // Starts from now: nothing before this counts as idle time.
    const DWORD now = GetTickCount();
    POINT point = {};
    GetCursorPos(&point);
    Settle(point, now);
    SetTimer(window_, timerId_, hidden_ ? kHiddenPollMs : kShownPollMs, nullptr);
}

void PointerHider::Settle(POINT point, DWORD now) {
    anchor_ = point;
    threshold_ = Threshold(options_.tolerance, point);
    lastActivity_ = now;
}

void PointerHider::Tick() {
    if (!options_.enabled) return;
    const DWORD now = GetTickCount();

    // Failing to read the pointer means the input is on a desktop this app
    // cannot see: the lock screen, or a UAC prompt. The pointer is shown there,
    // always, since nothing here could notice it being needed.
    POINT point = {};
    if (!GetCursorPos(&point)) {
        Show();
        lastActivity_ = now;
        return;
    }

    const bool moved = std::abs(point.x - anchor_.x) > threshold_ ||
                       std::abs(point.y - anchor_.y) > threshold_;
    if (moved || AnyMouseButtonDown()) {
        Settle(point, now);
        Show();
        return;
    }
    if (!hidden_ && now - lastActivity_ >= options_.idleMs) Hide();
}

void PointerHider::Hide() {
    if (hidden_) return;
    // Written first, so that however this process ends, the next start knows.
    SetMarker(true);

    // A cursor that draws nothing: the AND mask keeps the screen, the XOR mask
    // adds nothing. SetSystemCursor takes ownership of what it is given and
    // destroys it, so each shape gets a copy.
    BYTE andMask[32 * 32 / 8];
    BYTE xorMask[32 * 32 / 8];
    memset(andMask, 0xFF, sizeof(andMask));
    memset(xorMask, 0x00, sizeof(xorMask));
    HCURSOR blank = CreateCursor(nullptr, 0, 0, 32, 32, andMask, xorMask);
    if (!blank) {
        SetMarker(false);
        return;
    }
    for (const DWORD shape : kPointerShapes) {
        if (HCURSOR copy = CopyCursor(blank)) {
            if (!SetSystemCursor(copy, shape)) DestroyCursor(copy);
        }
    }
    DestroyCursor(blank);
    hidden_ = true;
    if (window_) SetTimer(window_, timerId_, kHiddenPollMs, nullptr);
}

void PointerHider::Show() {
    if (!hidden_) return;
    // Reloads the user's own pointers, whatever they are, rather than putting
    // back copies of what was there: a scheme, size, or colour changed while
    // hidden is honoured.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    hidden_ = false;
    SetMarker(false);
    if (window_ && options_.enabled) SetTimer(window_, timerId_, kShownPollMs, nullptr);
}

}  // namespace tray
