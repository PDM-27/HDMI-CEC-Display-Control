#include "VolumePanel.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace tray {
namespace {

constexpr wchar_t kClassName[] = L"CecVolumePanel";

// Sizes at 96 DPI; everything is scaled from these when the panel is shown.
constexpr int kBaseDiameter = 42;
constexpr int kBasePadding = 10;

constexpr int kButtonCount = 3;
enum ButtonIndex { kVolumeDown = 0, kMute, kVolumeUp };

constexpr int kCornerRound = 2;   // DWMWCP_ROUND
constexpr UINT_PTR kHoldTimer = 1;
constexpr UINT_PTR kRepeatTimer = 2;

// Asks for the level while the panel is open, and only then.
//
// The TCL never announces a change on its own - not one <Report Audio Status>
// in a full traffic log - so a volume moved with the TV's own remote is
// invisible unless someone asks. This asks. It costs one frame per interval
// while the panel is on screen, which is seconds a day, and nothing at all
// while it is closed: the timer exists only between showing and hiding.
constexpr UINT_PTR kPollTimer = 3;
//
// 500ms, and not much lower, because of what one poll is on the wire. CEC runs
// at about 417 bits a second: <Give Audio Status> is a start bit and two
// 10-bit blocks, around 52ms; the <Report Audio Status> reply adds a third
// block, around 76ms; with the mandatory silence between frames and the set's
// own thinking, one question and answer takes roughly 150-250ms. A 100ms poll
// is not slow, it is impossible - the answer would still be arriving when the
// next question was due.
//
// And the bus is shared. Every poll is time nobody else may transmit: not our
// own button presses, not the television asking us something. At 500ms a poll
// occupies the bus perhaps a third of the time the panel is open, which leaves
// plenty of room. Much tighter and a click would routinely wait behind one.
constexpr UINT kPollMs = 500;
//
// That is the television. This PC's own volume is a local read that costs
// nothing, so it is read ten times a second, whether or not a button was just
// touched: the level then follows the keyboard's volume keys, other apps, and
// Windows' own flyout as they happen, where the television's pace left it up to
// a second behind. The timer ticks at the fast rate, and a TV is only asked on
// the ticks that meet its own rules.
constexpr UINT kLocalPollMs = 100;

// The picker's list. Eight rows show at once at most, which fits comfortably on
// any screen at any scale; more than that scroll. Rows are hit-tested as codes
// above the buttons' indexes, so a button index can never be mistaken for one.
constexpr int kMaxOutputRows = 8;
constexpr int kHitOutputRow = 10;
constexpr int kHitOutputItem = 100;

constexpr wchar_t kGlyphCheck[]       = L"\uE73E";  // CheckMark
constexpr wchar_t kGlyphChevronUp[]   = L"\uE70E";  // ChevronUp
constexpr wchar_t kGlyphChevronDown[] = L"\uE70D";  // ChevronDown

// How long a panel shown for a volume key stays up after the last key: about
// what Windows gives its own volume flyout.
constexpr UINT_PTR kPassiveHideTimer = 4;
constexpr UINT kPassiveHideMs = 2000;

// How long after a button or the slider is touched before polling resumes.
// While someone is actively adjusting, the tray already keeps the number in
// step: it counts each click's step, asks during a hold, and asks once the
// clicking pauses, placing each question itself so it never lands between a
// click's press and release. The poll is for catching changes nobody here made.
//
// There used to be a read 250ms after each release, on a timer here. Reads
// then held the CEC lock until the answer came, so they had to wait for a
// quiet moment; now a question returns as soon as it is on the wire, and the
// wait only made the number late.
constexpr DWORD kPollQuietMs = 1000;

// The CEC spec puts the repeat on the initiator, not the TV: a held key
// has to be re-asserted with another <User Control Pressed> or the TV
// stops repeating after roughly 550ms. Sending it once and expecting the
// TV to carry on - which an earlier version did - gives exactly one step.
constexpr UINT kRepeatMs = 300;

// How long after hiding a tray click still counts as the click that closed the
// panel rather than a new one asking for it back. Long enough to cover the gap
// between losing activation and the click notification arriving, short enough
// that a deliberate second click is never mistaken for it.
constexpr DWORD kReopenGuardMs = 250;

// The least time between two levels sent while dragging. Each one is a CEC
// frame of roughly 90ms, so anything quicker would queue faster than the bus
// can drain and the volume would go on moving after the mouse stopped.
constexpr DWORD kSliderSendMs = 120;


// A backstop, not the normal way a hold ends.
//
// It used to be 4s, which was the whole reason holding a button stopped on its
// own after three or four seconds: long enough to look deliberate, far too
// short for anyone actually crossing a volume range.
//
// 35s is chosen against the rate this file sets rather than anything the TV
// decides. One step per kRepeatMs is about 3.3 a second, so 30s reached 96 of a
// 0-100 scale - just short of a full sweep, which meant the ceiling was still
// occasionally the thing that stopped you. 35s clears the whole range with room
// over, so it never is.
//
// What made the short value feel necessary was the fear of a lost mouse-up
// leaving the repeat running for ever. The repeat now checks the button itself
// on every tick, which catches that within 300ms instead of 4s, so this only
// has to cover the case where even that check somehow never runs.
constexpr UINT kMaxHoldMs = 35000;

// Whether the primary mouse button is still physically down.
//
// The button, not the message. WM_LBUTTONUP arriving is the normal way a hold
// ends, and every path that can swallow it - capture loss, the pointer leaving
// the window - is handled too, but polling the hardware state is the one check
// that cannot be defeated by a message going missing.
//
// SM_SWAPBUTTON matters here: with the buttons swapped, the primary button a
// user presses reports as VK_RBUTTON, and testing VK_LBUTTON would end every
// hold instantly.
bool PrimaryButtonDown() {
    const int key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

// Segoe MDL2 Assets ships with Windows 10, Segoe Fluent Icons with Windows 11.
// Written as escapes rather than literal characters: these live in the Private
// Use Area, so a literal would be invisible in an editor and easy to corrupt.
constexpr wchar_t kGlyphRemove[] = L"\uE738";  // Remove
constexpr wchar_t kGlyphAdd[]    = L"\uE710";  // Add
constexpr wchar_t kGlyphMute[]   = L"\uE74F";  // Mute
constexpr wchar_t kGlyphVolume[] = L"\uE767";  // Volume

// The circles are deliberately the inverse of the panel: white on a dark
// backdrop, dark on a light one. Anything closer in tone washes out once the
// blur picks up whatever happens to be behind the window.
struct Palette {
    COLORREF face;
    COLORREF faceHot;
    COLORREF facePressed;
    COLORREF glyph;
    COLORREF fallbackPanel;
    COLORREF fill;        // level bar, unmuted
    COLORREF fillMuted;   // level bar, muted
    // How strongly the faint shapes are washed over the panel: the unplayed
    // part of the slider, and the row that names the sound output.
    //
    // They differ by theme, which is not fussiness. The same faint wash reads
    // quite differently the two ways round: white over a dark panel stands out
    // where dark over a light one, with bright wallpaper coming through the
    // blur behind it, all but vanishes. It did vanish - the slider's unplayed
    // half and the output row were invisible in light mode.
    BYTE groove;
    BYTE rowIdle;
    BYTE rowHot;
    BYTE listBox;   // the backing behind the open list of outputs
};

// The app's blue, taken from its icon, and the same in both themes. It used to
// lighten on a dark backdrop for legibility, which read as two different apps
// depending on the time of day.
constexpr COLORREF kBrandBlue = RGB(15, 108, 189);

Palette PaletteFor(bool dark) {
    if (dark) {
        return {RGB(255, 255, 255), RGB(228, 228, 228), RGB(200, 200, 200),
                RGB(24, 24, 24),     RGB(28, 28, 28),
                kBrandBlue,          RGB(247, 156, 156),
                70, 30, 55, 22};
    }
    return {RGB(51, 51, 51),   RGB(74, 74, 74), RGB(96, 96, 96),
            RGB(255, 255, 255), RGB(246, 246, 246),
            kBrandBlue,         RGB(155, 46, 46),
            120, 120, 150, 70};
}

// ------------------------------------------------------- blur behind
//
// Which of the several ways to ask Windows for a translucent flyout actually
// works was settled by building all six side by side over a striped backdrop
// and looking at the result:
//
//   ACCENT_ENABLE_ACRYLICBLURBEHIND (4)  flat tint, no blur
//   ACCENT_ENABLE_BLURBEHIND (3)         real blur
//   DWMWA_SYSTEMBACKDROP_TYPE            flat, with or without an extended frame
//
// State 4 is the one every acrylic sample on the web reaches for, and it is
// the one that does nothing here: Windows 11 stopped honouring it somewhere on
// the way to build 26200 and now returns success while drawing a flat fill.
// State 3 is the older Aero-era blur, still live on both Windows 10 and 11.
//
// DWMWA_SYSTEMBACKDROP_TYPE is the documented Windows 11 route and was tried
// both plain and with DwmExtendFrameIntoClientArea(-1). Neither blurred, which
// fits its documentation: it is meant for windows with a real frame, and a
// borderless WS_POPUP has none.
//
// Layering is not what breaks the blur. UpdateLayeredWindow composites over a
// blurred backdrop perfectly well - the test proved it - so the panel keeps its
// per-pixel alpha and gets the blur too.

enum AccentState { kAccentBlurBehind = 3 };
enum CompositionAttribute { kWcaAccentPolicy = 19 };

struct AccentPolicy {
    int state;
    int flags;
    unsigned gradientColor;   // AABBGGRR, not the usual RGB order
    int animationId;
};

struct CompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, CompositionAttributeData*);

bool EnableBlurBehind(HWND window) {
    static const auto setAttribute =
        reinterpret_cast<SetWindowCompositionAttributeFn>(
            reinterpret_cast<void*>(GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")));
    if (!setAttribute) return false;

    AccentPolicy policy = {};
    policy.state = kAccentBlurBehind;
    // Only state 4 ever read this, so it is left at zero: the panel does its
    // own tinting in the bitmap, where the exact alpha is visible in one place.
    policy.gradientColor = 0;

    CompositionAttributeData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
    return setAttribute(window, &data) != FALSE;
}

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes"
                     L"\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

// GDI+ draws ellipses and rectangles but not rounded rectangles, so the corners
// are four arcs joined into a closed figure. With the radius at half the height
// this is a pill, which is what makes it sit with the circles below it rather
// than beside them.
void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float width,
                    float height, float radius) {
    if (radius * 2.0f > height) radius = height / 2.0f;
    if (radius * 2.0f > width) radius = width / 2.0f;
    const float span = radius * 2.0f;
    path.AddArc(x, y, span, span, 180.0f, 90.0f);
    path.AddArc(x + width - span, y, span, span, 270.0f, 90.0f);
    path.AddArc(x + width - span, y + height - span, span, span, 0.0f, 90.0f);
    path.AddArc(x, y + height - span, span, span, 90.0f, 90.0f);
    path.CloseFigure();
}

// Asking for a font that is not installed silently gets a substitute, so the
// only way to know whether the icon font is really there is to select it and
// read back the face name GDI actually used.
const wchar_t* AvailableIconFont(HDC dc) {
    static const wchar_t* const candidates[] = {L"Segoe Fluent Icons",
                                                L"Segoe MDL2 Assets"};
    for (const wchar_t* name : candidates) {
        LOGFONTW description = {};
        description.lfHeight = -16;
        description.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(description.lfFaceName, name);

        HFONT font = CreateFontIndirectW(&description);
        if (!font) continue;
        HGDIOBJ previous = SelectObject(dc, font);
        wchar_t actual[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, previous);
        DeleteObject(font);

        if (_wcsicmp(actual, name) == 0) return name;
    }
    return nullptr;
}

Gdiplus::Color ToGdiplus(COLORREF colour) {
    return Gdiplus::Color(255, GetRValue(colour), GetGValue(colour),
                          GetBValue(colour));
}

const char* PressCommand(int index) {
    switch (index) {
        case kVolumeDown: return "press-volume-down";
        case kMute:       return "press-mute";
        case kVolumeUp:   return "press-volume-up";
        default:          return nullptr;
    }
}

const wchar_t* GlyphFor(int index, bool haveIconFont) {
    if (!haveIconFont) {
        switch (index) {
            case kVolumeDown: return L"-";
            case kMute:       return L"x";
            case kVolumeUp:   return L"+";
            default:          return L"";
        }
    }
    switch (index) {
        case kVolumeDown: return kGlyphRemove;
        case kMute:       return kGlyphMute;
        case kVolumeUp:   return kGlyphAdd;
        default:          return L"";
    }
}

}  // namespace

VolumePanel::~VolumePanel() {
    if (gdiplusToken_) Gdiplus::GdiplusShutdown(gdiplusToken_);
}

bool VolumePanel::Create(HINSTANCE instance, Sender sender, LocalCheck levelsAreLocal) {
    sender_ = std::move(sender);
    levelsAreLocal_ = std::move(levelsAreLocal);
    dark_ = SystemUsesDarkTheme();

    // GDI has no antialiasing, which is what made the circles look ragged.
    // GDI+ is a system library, so this adds no new dependency.
    Gdiplus::GdiplusStartupInput startup;
    Gdiplus::GdiplusStartup(&gdiplusToken_, &startup, nullptr);

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = Proc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    windowClass.hbrBackground = nullptr;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&windowClass);

    // Layered, so the panel controls its own per-pixel alpha.
    //
    // Without it the rule is "whatever is not painted stays transparent", and
    // that is not something GDI+ actually guarantees - it writes the bounding
    // rectangle of what it draws, so each circle left an opaque square behind
    // it. Those squares were only visible against a contrasting backdrop, which
    // is why another window behind the panel made them obvious. Supplying a
    // premultiplied ARGB bitmap removes the guesswork: alpha is exactly what
    // this code puts there.
    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClassName, L"Volume",
        WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, this);
    if (!window_) return false;

    if (HDC dc = GetDC(window_)) {
        iconFont_ = AvailableIconFont(dc);
        ReleaseDC(window_, dc);
    }

    const int corner = kCornerRound;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));
    const BOOL useDark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark,
                          sizeof(useDark));
    EnableBlurBehind(window_);
    return true;
}

void VolumePanel::Relayout(HMONITOR monitor) {
    // Asked of the monitor the panel is about to appear on, never of the
    // window.
    //
    // GetDpiForWindow was the original and it is wrong here for a reason that
    // only shows up after the fact: this window spends its life hidden and
    // unplaced, so Windows answers for whichever monitor it last belonged to.
    // Change the display scale while the panel is closed and every subsequent
    // open is drawn at the old size - and unlike the shell's own flyouts,
    // reopening does not correct it, because reopening asks the same stale
    // question again. Reported after a resolution change: every other tray
    // flyout recovered on a second click, this one never did.
    //
    // GetDpiForMonitor has no such state. It is the scale of a physical
    // display, right now, and it is the display the panel is going to be drawn
    // on - so there is nothing left to go stale.
    UINT dpiX = 96, dpiY = 96;
    if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX,
                                            &dpiY))) {
        dpiX = window_ ? GetDpiForWindow(window_) : 96;
    }
    const UINT dpi = dpiX;
    diameter_ = Scale(kBaseDiameter, dpi);
    padding_ = Scale(kBasePadding, dpi);
    width_ = padding_ * (kButtonCount + 1) + diameter_ * kButtonCount;
    FitOutputRows(monitor);
    height_ = ContentHeight();
}

bool VolumePanel::ShowsOutputs() const {
    return !passive_ && outputs_.size() >= 2 && diameter_ > 0;
}

int VolumePanel::OutputRowHeight() const { return diameter_ * 3 / 5; }

int VolumePanel::ContentHeight() const {
    int height = padding_ * 2 + diameter_ + ReadoutHeight();
    if (ShowsOutputs()) {
        height += padding_ + OutputRowHeight();
        if (outputsOpen_) height += outputRows_ * OutputRowHeight() + padding_ / 2;
    }
    return height;
}

RECT VolumePanel::OutputListRect() const {
    const int top = padding_ + ReadoutHeight() + diameter_ + padding_;
    const int rows = outputsOpen_ ? outputRows_ : 0;
    return {padding_, top, width_ - padding_, top + rows * OutputRowHeight()};
}

RECT VolumePanel::OutputRowRect() const {
    int top = padding_ + ReadoutHeight() + diameter_ + padding_;
    if (outputsOpen_) top += outputRows_ * OutputRowHeight() + padding_ / 2;
    return {padding_, top, width_ - padding_, top + OutputRowHeight()};
}

void VolumePanel::LoadOutputs() {
    outputs_.clear();
    if (CanChooseAudioOutput()) outputs_ = ListAudioOutputs();
    if (outputs_.size() < 2) outputsOpen_ = false;
}

void VolumePanel::FitOutputRows(HMONITOR monitor) {
    outputRows_ = 0;
    if (!outputsOpen_ || !ShowsOutputs()) {
        outputsScroll_ = 0;
        return;
    }
    const int count = static_cast<int>(outputs_.size());
    int rows = count < kMaxOutputRows ? count : kMaxOutputRows;
    MONITORINFO info = {sizeof(info)};
    const int rowHeight = OutputRowHeight();
    if (monitor && rowHeight > 0 && GetMonitorInfoW(monitor, &info)) {
        // Measured with no rows yet, so this is everything but the list.
        const int room = (info.rcWork.bottom - info.rcWork.top) - ContentHeight() -
                         padding_ * 2;
        const int fit = room / rowHeight;
        if (fit < rows) rows = fit;
    }
    outputRows_ = rows < 1 ? 1 : rows;
    const int last = count - outputRows_;
    if (outputsScroll_ > last) outputsScroll_ = last;
    if (outputsScroll_ < 0) outputsScroll_ = 0;
}

void VolumePanel::Refit() {
    if (!window_) return;
    const int centreX = x_ + width_ / 2;
    const int bottom = y_ + height_;
    HMONITOR monitor = MonitorFromPoint({centreX, bottom - 1}, MONITOR_DEFAULTTONEAREST);
    Relayout(monitor);
    x_ = centreX - width_ / 2;
    y_ = bottom - height_;
    MONITORINFO info = {sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) {
        if (x_ + width_ > info.rcWork.right) x_ = info.rcWork.right - width_;
        if (x_ < info.rcWork.left) x_ = info.rcWork.left;
        if (y_ + height_ > info.rcWork.bottom) y_ = info.rcWork.bottom - height_;
        if (y_ < info.rcWork.top) y_ = info.rcWork.top;
    }
    Render();
}

void VolumePanel::OutputsChanged() {
    // Left for the release to pick up if a click is still in progress.
    if (!visible() || passive_ || loadOutputsOnRelease_) return;
    LoadOutputs();
    Refit();
}

void VolumePanel::LoadDeferredOutputs() {
    if (!loadOutputsOnRelease_) return;
    loadOutputsOnRelease_ = false;
    LoadOutputs();
    hot_ = -1;
    Refit();
}

void VolumePanel::ReleaseHeld() {
    if (draggingSlider_) {
        draggingSlider_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }
    EndHold();
}

int VolumePanel::PillHeight() const {
    // Exactly half a button. Derived rather than stored, so it follows the DPI
    // the buttons were already scaled to.
    return (!readoutKnown_ || diameter_ <= 0) ? 0 : diameter_ / 2;
}

int VolumePanel::ReadoutHeight() const {
    // The pill plus the gap beneath it. Everything else measures from this, and
    // the window's own height includes it - so the translucent backdrop, which
    // is filled edge to edge and sized from the bitmap, grows to match without
    // being told.
    const int pill = PillHeight();
    return pill ? pill + padding_ / 2 : 0;
}

RECT VolumePanel::SliderRect() const {
    const int strip = PillHeight();
    if (!strip) return {0, 0, 0, 0};
    // Mute indicator on the left, percentage on the right, track between. The
    // percentage keeps a fixed width rather than one measured from the digits,
    // so the track does not shift under the thumb as the number goes from 9 to
    // 10 to 100.
    //
    // The same width is reserved on both sides, though what sits in them is not
    // the same width, because the track has to be centred on the panel. The
    // number needs more room than the glyph does, so giving each side only what
    // it needed put the middle of the track a third of a strip left of the
    // middle of the panel - and the middle button below it is centred, so a
    // thumb at 50 sitting left of that button reads as the bar being wrong.
    // The glyph and the number keep their places at the outer edges; only the
    // track moves.
    const int icon = strip;
    const int percent = strip * 8 / 5;
    const int side = percent > icon ? percent : icon;
    const int gap = padding_ / 2;
    const int left = padding_ + side + gap;
    const int right = width_ - padding_ - side - gap;
    return {left, padding_, right > left ? right : left + 1, padding_ + strip};
}

int VolumePanel::LevelAtX(int x) const {
    const RECT track = SliderRect();
    const int span = track.right - track.left;
    if (span <= 0) return 0;
    int level = MulDiv(x - track.left, 100, span);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    return level;
}

void VolumePanel::SetLevelsSettable(bool settable) {
    if (levelsSettable_ == settable) return;
    levelsSettable_ = settable;
    // Only the thumb comes and goes, so the panel keeps its size and nothing
    // moves; a redraw is enough and Refit would be wrong.
    if (visible()) Render();
}

// Whether a drag or a click on the bar can do anything right now. The bus may
// refuse levels while this PC's own output, which is what the check is for,
// takes them perfectly well.
bool VolumePanel::LevelsWork() const {
    return levelsSettable_ || (levelsAreLocal_ && levelsAreLocal_());
}

void VolumePanel::SendLevel(int level, bool force) {
    // The thumb first, and whether or not anything is sent. It used to move
    // only when a level went out, so on screen it stepped along ten times a
    // second behind the pointer.
    if (level != readoutLevel_) {
        readoutLevel_ = level;   // the read-back confirms it
        Render();
    }
    const DWORD now = GetTickCount();
    if (!force) {
        if (level == lastSentLevel_) return;
        const bool local = levelsAreLocal_ && levelsAreLocal_();
        if (!local && lastSendTick_ && now - lastSendTick_ < kSliderSendMs) return;
    }
    lastSentLevel_ = level;
    lastSendTick_ = now;
    if (sender_) {
        char command[32];
        snprintf(command, sizeof(command), "set-volume %d", level);
        sender_(command);
    }
}

RECT VolumePanel::CircleRect(int index) const {
    const int left = padding_ + index * (diameter_ + padding_);
    const int top = padding_ + ReadoutHeight();
    return {left, top, left + diameter_, top + diameter_};
}

void VolumePanel::SetReadout(bool known, int level, bool muted) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    if (readoutKnown_ == known && readoutLevel_ == level &&
        readoutMuted_ == muted) {
        return;   // nothing to redraw
    }

    // A change in whether there is a readout at all changes the window's
    // height, so the panel is laid out again before it is drawn. Render hands
    // UpdateLayeredWindow a bitmap of width_ by height_ and that call resizes
    // the window, so there is nothing else to move.
    const bool sizeChanged = (readoutKnown_ != known);
    readoutKnown_ = known;
    readoutLevel_ = level;
    readoutMuted_ = muted;
    if (!window_ || !IsWindowVisible(window_)) return;
    // Grown upwards, not downwards: the panel's bottom edge sits just above
    // the pointer, and growing the other way would slide the buttons down under
    // the cursor - which is where the click that opened it is still hovering.
    if (sizeChanged) {
        Refit();
    } else {
        Render();
    }
}


int VolumePanel::HitTest(POINT point) const {
    if (ShowsOutputs()) {
        const RECT row = OutputRowRect();
        if (PtInRect(&row, point)) return kHitOutputRow;
        if (outputsOpen_) {
            const RECT list = OutputListRect();
            if (PtInRect(&list, point) && OutputRowHeight() > 0) {
                const int index =
                    outputsScroll_ + (point.y - list.top) / OutputRowHeight();
                if (index < static_cast<int>(outputs_.size())) {
                    return kHitOutputItem + index;
                }
            }
        }
    }
    const int radius = diameter_ / 2;
    for (int index = 0; index < kButtonCount; ++index) {
        const RECT circle = CircleRect(index);
        const int dx = point.x - (circle.left + circle.right) / 2;
        const int dy = point.y - (circle.top + circle.bottom) / 2;
        if (dx * dx + dy * dy <= radius * radius) return index;
    }
    return -1;
}

void VolumePanel::Invalidate(int index) {
    // A layered window is redrawn by handing the compositor a new bitmap, not
    // by invalidating a region, so the whole panel is rebuilt. It is three
    // circles; the cost is irrelevant and there are no partial-repaint seams.
    (void)index;
    Render();
}

void VolumePanel::BeginHold(int index) {
    if (index < 0 || held_ == index) return;
    EndHold();
    held_ = index;
    if (sender_) {
        if (const char* command = PressCommand(index)) sender_(command);
    }
    SetTimer(window_, kHoldTimer, kMaxHoldMs, nullptr);
    SetTimer(window_, kRepeatTimer, kRepeatMs, nullptr);
    Invalidate(index);
}

void VolumePanel::EndHold() {
    if (held_ < 0) return;
    const int was = held_;
    held_ = -1;
    KillTimer(window_, kHoldTimer);
    KillTimer(window_, kRepeatTimer);
    if (sender_) sender_("release");
    Invalidate(was);
}

void VolumePanel::Hide() {
    // Release first: hiding with a key still held would leave the TV repeating
    // it with nothing on screen left to stop it.
    EndHold();
    if (window_ && IsWindowVisible(window_)) {
        lastHiddenTick_ = GetTickCount();
        KillTimer(window_, kPollTimer);
        KillTimer(window_, kPassiveHideTimer);
        passive_ = false;
        ShowWindow(window_, SW_HIDE);
    }
    // Read again at the next open, so nothing shown then is left over from now.
    outputs_.clear();
    outputsOpen_ = false;
    loadOutputsOnRelease_ = false;
}

void VolumePanel::StartPolling() {
    // Counted from now, so a TV is not asked again straight after the question
    // that opening the panel already sent.
    lastPollTick_ = GetTickCount();
    SetTimer(window_, kPollTimer, kLocalPollMs, nullptr);
}

void VolumePanel::KeepOpen(bool pickerNow) {
    passive_ = false;
    KillTimer(window_, kPassiveHideTimer);
    StartPolling();
    if (pickerNow) {
        LoadOutputs();
        Refit();
    } else {
        loadOutputsOnRelease_ = true;
    }
}

void VolumePanel::ShowPassive(POINT anchor) {
    if (!window_) return;
    if (IsWindowVisible(window_)) {
        // Opened by someone: theirs to close. Shown for a key: keep it up.
        if (passive_) SetTimer(window_, kPassiveHideTimer, kPassiveHideMs, nullptr);
        return;
    }
    passive_ = true;
    outputs_.clear();
    outputsOpen_ = false;
    PlaceNear(anchor);
    // No activation, and no poll: the number comes from the keys themselves
    // and the answers they prompt, and focus stays wherever it was.
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetTimer(window_, kPassiveHideTimer, kPassiveHideMs, nullptr);
}

bool VolumePanel::Toggle() {
    if (!window_) return false;
    if (IsWindowVisible(window_)) {
        // Up only because a key changed the volume. A click on the tray icon
        // means the panel is wanted, so it becomes an ordinary open panel
        // rather than closing under the pointer.
        if (passive_) {
            // The pointer is on the tray icon, not the panel, so nothing can
            // move out from under it.
            KeepOpen(true);
            SetForegroundWindow(window_);
            return true;
        }
        Hide();
        return false;
    }

    // Clicking the tray icon activates the taskbar, which deactivates this
    // panel and hides it - and that happens before the click notification
    // arrives here. So a second click always finds the panel already hidden,
    // and reopening it makes the click look like it did nothing. This is why
    // the panel could never be closed the way it was opened.
    //
    // Anything this soon after a hide is that same click finishing, not a new
    // one. The guard is cleared either way, so a genuine click a moment later
    // still opens the panel.
    const DWORD since = GetTickCount() - lastHiddenTick_;
    const bool closingClick = lastHiddenTick_ && since < kReopenGuardMs;
    lastHiddenTick_ = 0;
    if (closingClick) return false;

    POINT cursor = {};
    GetCursorPos(&cursor);
    passive_ = false;
    outputsOpen_ = false;
    outputsScroll_ = 0;
    LoadOutputs();   // before placing, so the size includes the picker's row
    PlaceNear(cursor);
    ShowWindow(window_, SW_SHOW);
    StartPolling();
    SetForegroundWindow(window_);
    return true;
}

void VolumePanel::PlaceNear(POINT point) {
    // Re-read the theme in case it changed since the panel was created.
    const bool dark = SystemUsesDarkTheme();
    if (dark != dark_) {
        dark_ = dark;
        const BOOL useDark = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark,
                              sizeof(useDark));
        EnableBlurBehind(window_);
    }

    hot_ = -1;

    // The monitor is found before the layout, not after, because the layout
    // depends on it.
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    GetMonitorInfoW(monitor, &info);

    Relayout(monitor);

    int x = point.x - width_ / 2;
    int y = point.y - height_ - 12;
    if (x < info.rcWork.left) x = info.rcWork.left;
    if (x + width_ > info.rcWork.right) x = info.rcWork.right - width_;
    if (y < info.rcWork.top) y = point.y + 12;
    if (y + height_ > info.rcWork.bottom) y = info.rcWork.bottom - height_;

    x_ = x;
    y_ = y;
    Render();   // UpdateLayeredWindow positions and sizes the window itself
}

void VolumePanel::Render() {
    if (!window_ || width_ <= 0 || height_ <= 0) return;

    const Palette palette = PaletteFor(dark_);

    // Premultiplied ARGB, which is what UpdateLayeredWindow expects.
    Gdiplus::Bitmap bitmap(width_, height_, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        // PixelOffsetModeHighQuality is the one that matters and is easy to
        // miss: without it GDI+ samples on pixel corners rather than centres,
        // and antialiased curves come out visibly steppy. On a 4K television
        // that is the difference between a circle and a polygon.
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        // The body deliberately fills the whole rectangle rather than a
        // rounded one of its own.
        //
        // The blur behind the window covers the entire window rect, so any
        // corner left at alpha zero shows raw unblurred backdrop - a second,
        // square-ish corner outside the rounded one. Drawing a rounded rect
        // here on top of the DWM's own rounding is what produced the doubled
        // corner. Filling edge to edge and letting DWMWA_WINDOW_CORNER_
        // PREFERENCE round the composited result leaves exactly one.
        //
        // The alpha is what makes the blur visible: at 210 the panel was
        // effectively opaque and the blur behind it could not be seen.
        const BYTE tint = 165;
        Gdiplus::SolidBrush backdrop(Gdiplus::Color(
            tint, GetRValue(palette.fallbackPanel),
            GetGValue(palette.fallbackPanel), GetBValue(palette.fallbackPanel)));
        graphics.FillRectangle(&backdrop, 0, 0, width_, height_);

        Gdiplus::FontFamily fontFamily(iconFont_ ? iconFont_ : L"Segoe UI");
        Gdiplus::SolidBrush ink(ToGdiplus(palette.glyph));

        // The readout: a mute indicator, a track, and the level, laid out the
        // way Windows lays out its own.
        //
        // Everything here sits on the panel backdrop rather than on a filled
        // shape, so it takes palette.face - the colour already chosen to stand
        // out from that backdrop. The glyph colour belongs on the buttons,
        // where the background is the opposite one.
        if (readoutKnown_) {
            const int strip = PillHeight();
            const RECT track = SliderRect();
            const float centreY = padding_ + strip / 2.0f;
            Gdiplus::SolidBrush faceInk(ToGdiplus(palette.face));
            Gdiplus::StringFormat layout(
                Gdiplus::StringFormat::GenericTypographic());

            // The unplayed part of the track, dim so the filled part reads as
            // the measurement rather than the groove doing.
            const float thickness = strip * 0.30f;
            Gdiplus::GraphicsPath groove;
            AddRoundedRect(groove, static_cast<float>(track.left),
                           centreY - thickness / 2.0f,
                           static_cast<float>(track.right - track.left),
                           thickness, thickness / 2.0f);
            Gdiplus::SolidBrush grooveBrush(Gdiplus::Color(
                palette.groove, GetRValue(palette.face), GetGValue(palette.face),
                GetBValue(palette.face)));
            graphics.FillPath(&grooveBrush, &groove);

            // The filled part, clipped to the groove so both ends match it
            // exactly and the leading edge stays straight.
            if (readoutLevel_ > 0) {
                graphics.SetClip(&groove);
                Gdiplus::SolidBrush level(ToGdiplus(
                    readoutMuted_ ? palette.fillMuted : palette.fill));
                graphics.FillRectangle(
                    &level, static_cast<float>(track.left),
                    centreY - thickness / 2.0f,
                    (track.right - track.left) * (readoutLevel_ / 100.0f),
                    thickness);
                graphics.ResetClip();
            }

            // The thumb, on the face colour so it reads as something to take
            // hold of rather than part of the bar. Left out entirely when a
            // level cannot be set, because that is exactly what it promises:
            // without it the bar reads as a meter, which is all it then is.
            if (LevelsWork()) {
                const float thumb = strip * 0.38f;
                const float thumbX = track.left + (track.right - track.left) *
                                                      (readoutLevel_ / 100.0f);
                graphics.FillEllipse(&faceInk, thumbX - thumb, centreY - thumb,
                                     thumb * 2.0f, thumb * 2.0f);
            }

            // Mute, on the left, shown whether or not it is on - an indicator
            // that vanishes when it is off gives no way to tell "not muted"
            // from "we do not know". A speaker when on and a crossed-out one
            // when muted, the way Windows shows it.
            const wchar_t* mark =
                iconFont_ ? (readoutMuted_ ? kGlyphMute : kGlyphVolume)
                          : (readoutMuted_ ? L"x" : L"\u2022");
            Gdiplus::FontFamily markFamily(iconFont_ ? iconFont_ : L"Segoe UI");
            Gdiplus::Font markFont(&markFamily, strip * 0.52f,
                                   Gdiplus::FontStyleRegular,
                                   Gdiplus::UnitPixel);
            Gdiplus::RectF markBox;
            graphics.MeasureString(mark, -1, &markFont, Gdiplus::PointF(0, 0),
                                   &layout, &markBox);
            // Full strength in the face colour when not muted, the same ink as
            // the number, so it keeps its contrast in either theme. It used to
            // be the face colour at a third opacity, which over the blur was
            // close to the backdrop itself and all but disappeared.
            Gdiplus::SolidBrush muteInk(ToGdiplus(
                readoutMuted_ ? palette.fillMuted : palette.face));
            graphics.DrawString(
                mark, -1, &markFont,
                Gdiplus::PointF(padding_ + (strip - markBox.Width) / 2.0f,
                                centreY - markBox.Height / 2.0f),
                &layout, &muteInk);

            // The level, on the right, right-aligned so the digits do not
            // shuffle sideways as the number gains and loses a place.
            wchar_t number[8] = {};
            swprintf_s(number, L"%d", readoutLevel_);
            Gdiplus::FontFamily textFamily(L"Segoe UI");
            Gdiplus::Font font(&textFamily, strip * 0.58f,
                               Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::RectF numberBox;
            graphics.MeasureString(number, -1, &font, Gdiplus::PointF(0, 0),
                                   &layout, &numberBox);
            graphics.DrawString(
                number, -1, &font,
                Gdiplus::PointF(width_ - padding_ - numberBox.Width,
                                centreY - numberBox.Height / 2.0f),
                &layout, &faceInk);
        }
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());

        for (int index = 0; index < kButtonCount; ++index) {
            const RECT circle = CircleRect(index);
            const COLORREF face = (index == held_)  ? palette.facePressed
                                  : (index == hot_) ? palette.faceHot
                                                    : palette.face;

            Gdiplus::SolidBrush brush(ToGdiplus(face));
            graphics.FillEllipse(&brush, static_cast<float>(circle.left) + 0.5f,
                                 static_cast<float>(circle.top) + 0.5f,
                                 diameter_ - 1.0f, diameter_ - 1.0f);

            // Built as a path and filled rather than drawn as text: DrawString
            // centres on the layout box, which for an icon font is not where
            // the visible mark sits, so glyphs stayed slightly off centre
            // however the StringFormat was configured. A path can be measured
            // by its actual ink and moved to the exact centre.
            Gdiplus::GraphicsPath path;
            path.AddString(GlyphFor(index, iconFont_ != nullptr), -1, &fontFamily,
                           Gdiplus::FontStyleRegular, diameter_ * 0.40f,
                           Gdiplus::PointF(0.0f, 0.0f), &format);

            Gdiplus::RectF inkBounds;
            if (path.GetBounds(&inkBounds) == Gdiplus::Ok && inkBounds.Width > 0.0f) {
                const float cx = (circle.left + circle.right) / 2.0f;
                const float cy = (circle.top + circle.bottom) / 2.0f;
                Gdiplus::Matrix move;
                move.Translate(cx - (inkBounds.X + inkBounds.Width / 2.0f),
                               cy - (inkBounds.Y + inkBounds.Height / 2.0f));
                path.Transform(&move);
                graphics.FillPath(&ink, &path);
            }
        }

        if (ShowsOutputs()) {
            const float row = static_cast<float>(OutputRowHeight());
            const auto faceShade = [&](BYTE alpha) {
                return Gdiplus::Color(alpha, GetRValue(palette.face),
                                      GetGValue(palette.face),
                                      GetBValue(palette.face));
            };
            const auto fillPill = [&](const RECT& area, BYTE alpha) {
                Gdiplus::GraphicsPath pill;
                AddRoundedRect(pill, static_cast<float>(area.left),
                               static_cast<float>(area.top),
                               static_cast<float>(area.right - area.left),
                               static_cast<float>(area.bottom - area.top), row / 2.0f);
                Gdiplus::SolidBrush brush(faceShade(alpha));
                graphics.FillPath(&brush, &pill);
            };
            // Glyphs are centred by their ink, the way the buttons' are, since
            // an icon font's layout box is not where its mark sits.
            Gdiplus::FontFamily glyphFamily(iconFont_ ? iconFont_ : L"Segoe UI");
            Gdiplus::StringFormat glyphFormat(
                Gdiplus::StringFormat::GenericTypographic());
            const auto drawGlyph = [&](const wchar_t* glyph, float size, float cx,
                                       float cy, const Gdiplus::Brush& brush) {
                Gdiplus::GraphicsPath path;
                path.AddString(glyph, -1, &glyphFamily, Gdiplus::FontStyleRegular,
                               size, Gdiplus::PointF(0.0f, 0.0f), &glyphFormat);
                Gdiplus::RectF bounds;
                if (path.GetBounds(&bounds) != Gdiplus::Ok || bounds.Width <= 0.0f) {
                    return;
                }
                Gdiplus::Matrix move;
                move.Translate(cx - (bounds.X + bounds.Width / 2.0f),
                               cy - (bounds.Y + bounds.Height / 2.0f));
                path.Transform(&move);
                graphics.FillPath(&brush, &path);
            };

            // Names: one line, cut short with an ellipsis rather than wrapped or
            // overflowing, and vertically centred in their row.
            Gdiplus::FontFamily textFamily(L"Segoe UI");
            Gdiplus::Font nameFont(&textFamily, row * 0.46f, Gdiplus::FontStyleRegular,
                                   Gdiplus::UnitPixel);
            Gdiplus::StringFormat nameFormat(Gdiplus::StringFormatFlagsNoWrap);
            nameFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            nameFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            Gdiplus::SolidBrush faceInk(ToGdiplus(palette.face));
            // The tick takes the text colour rather than the app's blue. The
            // blue is one colour in both themes, which is right for the slider
            // where it sits on a pale groove, and too dark for a small mark on
            // a dark panel.
            Gdiplus::SolidBrush markInk(ToGdiplus(palette.face));
            const float column = row * 0.95f;   // the tick's and the chevron's

            if (outputsOpen_) {
                const RECT list = OutputListRect();
                const float scrollbar =
                    static_cast<int>(outputs_.size()) > outputRows_ ? row * 0.35f : 0.0f;
                Gdiplus::GraphicsPath box;
                AddRoundedRect(box, static_cast<float>(list.left),
                               static_cast<float>(list.top),
                               static_cast<float>(list.right - list.left),
                               static_cast<float>(list.bottom - list.top), row / 2.0f);
                Gdiplus::SolidBrush boxBrush(faceShade(palette.listBox));
                graphics.FillPath(&boxBrush, &box);
                graphics.SetClip(&box);
                for (int slot = 0; slot < outputRows_; ++slot) {
                    const int index = outputsScroll_ + slot;
                    if (index >= static_cast<int>(outputs_.size())) break;
                    const AudioOutput& output = outputs_[index];
                    const int top = list.top + slot * OutputRowHeight();
                    const RECT item = {list.left, top, list.right, top + OutputRowHeight()};
                    if (hot_ == kHitOutputItem + index) fillPill(item, palette.rowHot);
                    if (output.isDefault) {
                        drawGlyph(iconFont_ ? kGlyphCheck : L"\u2022", row * 0.42f,
                                  item.left + column / 2.0f + row * 0.1f,
                                  top + row / 2.0f, markInk);
                    }
                    const Gdiplus::RectF text(
                        item.left + column + row * 0.1f, static_cast<float>(top),
                        (item.right - item.left) - column - row * 0.1f - row * 0.35f -
                            scrollbar,
                        row);
                    graphics.DrawString(output.name.c_str(), -1, &nameFont, text,
                                        &nameFormat, &faceInk);
                }
                graphics.ResetClip();

                // Only when there is more than fits: a thin pill showing where
                // in the list the rows on screen are.
                if (scrollbar > 0.0f) {
                    const float trackTop = list.top + row * 0.3f;
                    const float trackHeight =
                        (list.bottom - list.top) - row * 0.6f;
                    const float count = static_cast<float>(outputs_.size());
                    const float thumbHeight = trackHeight * (outputRows_ / count);
                    const float thumbTop =
                        trackTop + trackHeight * (outputsScroll_ / count);
                    const float thickness = row * 0.12f;
                    Gdiplus::GraphicsPath thumb;
                    AddRoundedRect(thumb, list.right - row * 0.3f - thickness / 2.0f,
                                   thumbTop, thickness, thumbHeight, thickness / 2.0f);
                    Gdiplus::SolidBrush thumbBrush(faceShade(110));
                    graphics.FillPath(&thumbBrush, &thumb);
                }
            }

            // The row naming the current output, with the way the list opens.
            const RECT current = OutputRowRect();
            fillPill(current, hot_ == kHitOutputRow ? palette.rowHot : palette.rowIdle);
            const wchar_t* currentName = L"No output chosen";
            for (const AudioOutput& output : outputs_) {
                if (output.isDefault) currentName = output.name.c_str();
            }
            const Gdiplus::RectF currentText(
                current.left + row * 0.5f, static_cast<float>(current.top),
                (current.right - current.left) - row * 0.5f - column,
                row);
            graphics.DrawString(currentName, -1, &nameFont, currentText, &nameFormat,
                                &faceInk);
            const wchar_t* chevron =
                iconFont_ ? (outputsOpen_ ? kGlyphChevronDown : kGlyphChevronUp)
                          : (outputsOpen_ ? L"v" : L"^");
            drawGlyph(chevron, row * 0.36f, current.right - column / 2.0f - row * 0.1f,
                      current.top + row / 2.0f, faceInk);
        }
    }

    // Copy into a DIB section, which is what UpdateLayeredWindow takes.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;   // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        Gdiplus::Rect area(0, 0, width_, height_);
        Gdiplus::BitmapData data = {};
        if (bitmap.LockBits(&area, Gdiplus::ImageLockModeRead,
                            PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
            // Row by row: GDI+ may pad or invert its stride.
            for (int y = 0; y < height_; ++y) {
                memcpy(static_cast<BYTE*>(bits) + static_cast<size_t>(y) * width_ * 4,
                       static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
                       static_cast<size_t>(width_) * 4);
            }
            bitmap.UnlockBits(&data);

            HDC memory = CreateCompatibleDC(screen);
            HGDIOBJ previous = SelectObject(memory, dib);

            POINT source = {0, 0};
            POINT position = {x_, y_};
            SIZE size = {width_, height_};
            BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(window_, screen, &position, &size, memory, &source,
                                0, &blend, ULW_ALPHA);

            SelectObject(memory, previous);
            DeleteDC(memory);
        }
    }
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK VolumePanel::Proc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    auto* panel = reinterpret_cast<VolumePanel*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_MOUSEMOVE:
            if (panel) {
                if (!panel->tracking_) {
                    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
                    TrackMouseEvent(&track);
                    panel->tracking_ = true;
                }
                POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (panel->draggingSlider_) {
                    // Rate limited inside SendLevel. The pointer is allowed to
                    // leave the window during a drag, which is why the level is
                    // clamped rather than the point rejected.
                    panel->SendLevel(panel->LevelAtX(point.x), false);
                    return 0;
                }
                const int hit = panel->HitTest(point);
                if (hit != panel->hot_) {
                    const int previous = panel->hot_;
                    panel->hot_ = hit;
                    panel->Invalidate(previous);
                    panel->Invalidate(hit);
                }
                // Sliding off a held button ends the hold, the same as letting
                // go would - otherwise the key stays down with the pointer
                // somewhere else entirely.
                if (panel->held_ >= 0 && hit != panel->held_) panel->EndHold();
            }
            return 0;
        case WM_MOUSELEAVE:
            if (panel) {
                panel->tracking_ = false;
                const int previous = panel->hot_;
                panel->hot_ = -1;
                panel->Invalidate(previous);
                panel->EndHold();
                if (GetCapture() != window) panel->LoadDeferredOutputs();
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (panel) {
                panel->lastInteractionTick_ = GetTickCount();
                POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                // The track is checked first and claims the whole gesture. A
                // drag that starts on it must not turn into a button press
                // when it passes over one on the way.
                RECT track = panel->SliderRect();
                // The thumb hangs half outside the track at either end, so the
                // grab reaches that far and no further.
                //
                // The band was tested by its height alone, which quietly made
                // the whole width of the strip a slider. The mute indicator sits
                // to the left of the track and the percentage to its right, and
                // a click on either went through LevelAtX - which clamps - so
                // clicking the number set the volume to 100 and clicking the
                // indicator set it to 0. Neither is a control, and on a
                // television at night the first of those is not a small thing.
                const int grab = (track.bottom - track.top) * 38 / 100;
                if (panel->readoutKnown() && panel->LevelsWork() &&
                    point.y >= track.top &&
                    point.y < track.bottom && point.x >= track.left - grab &&
                    point.x < track.right + grab) {
                    SetCapture(window);
                    panel->draggingSlider_ = true;
                    panel->SendLevel(panel->LevelAtX(point.x), true);
                    return 0;
                }
                const int hit = panel->HitTest(point);
                if (hit == kHitOutputRow) {
                    panel->outputsOpen_ = !panel->outputsOpen_;
                    if (panel->outputsOpen_) {
                        // Read afresh, and scrolled to the current output. The
                        // layout below keeps the scroll within the list, so an
                        // output near the end still fills the rows.
                        panel->LoadOutputs();
                        panel->outputsScroll_ = 0;
                        panel->wheelRemainder_ = 0;
                        for (size_t i = 0; i < panel->outputs_.size(); ++i) {
                            if (panel->outputs_[i].isDefault) {
                                panel->outputsScroll_ = static_cast<int>(i);
                            }
                        }
                    }
                    // The window grew or shrank upward, so the row is still the
                    // thing under the pointer.
                    panel->hot_ = panel->ShowsOutputs() ? kHitOutputRow : -1;
                    panel->Refit();
                    return 0;
                }
                if (hit >= kHitOutputItem) {
                    const size_t index = static_cast<size_t>(hit - kHitOutputItem);
                    if (index < panel->outputs_.size() &&
                        !panel->outputs_[index].isDefault) {
                        // A refusal needs no handling of its own: the list is
                        // read back below, and shows whatever Windows now has.
                        ChooseAudioOutput(panel->outputs_[index].id);
                    }
                    panel->outputsOpen_ = false;
                    panel->LoadOutputs();
                    panel->hot_ = -1;
                    panel->Refit();
                    return 0;
                }
                if (hit >= 0 && hit < kButtonCount) {
                    SetCapture(window);
                    panel->BeginHold(hit);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (panel) {
                if (GetCapture() == window) ReleaseCapture();
                if (panel->draggingSlider_) {
                    // Always sent, whatever the rate limit says. The level the
                    // pointer was left on is the one that was meant, and
                    // dropping it because it arrived too soon after the last
                    // would leave the volume somewhere nobody chose.
                    //
                    // Unless the slider has gone. With no track, every x maps
                    // to 0, and a readout withdrawn mid-drag must not become a
                    // command to silence the television.
                    POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    if (panel->readoutKnown()) {
                        panel->SendLevel(panel->LevelAtX(point.x), true);
                    }
                    panel->draggingSlider_ = false;
                    if (panel->sender_) panel->sender_("volume-settled");
                } else {
                    panel->EndHold();
                }
                panel->LoadDeferredOutputs();
            }
            return 0;
        case WM_MOUSEWHEEL:
            // Scrolls the list when it has more than fits, and nothing else.
            if (panel && panel->outputsOpen_ && panel->ShowsOutputs() &&
                static_cast<int>(panel->outputs_.size()) > panel->outputRows_) {
                panel->wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
                const int notches = panel->wheelRemainder_ / WHEEL_DELTA;
                panel->wheelRemainder_ %= WHEEL_DELTA;
                if (notches) {
                    const int last =
                        static_cast<int>(panel->outputs_.size()) - panel->outputRows_;
                    int scroll = panel->outputsScroll_ - notches;
                    if (scroll > last) scroll = last;
                    if (scroll < 0) scroll = 0;
                    panel->outputsScroll_ = scroll;
                    // The rows moved under a pointer that did not, so what it is
                    // over is worked out again.
                    POINT cursor = {};
                    GetCursorPos(&cursor);
                    ScreenToClient(window, &cursor);
                    panel->hot_ = panel->HitTest(cursor);
                    panel->Render();
                }
            }
            return 0;
        // Only reaches an open panel, and it is not the fix for the stale-DPI
        // bug - Relayout asking the monitor is. It is here so that a scale
        // change while the panel happens to be open is not left on screen at
        // the old size until it is closed and reopened.
        case WM_DPICHANGED:
            if (panel && panel->visible()) panel->Refit();
            return 0;
        // A resolution change or a rotation can leave an open panel partly off
        // the new screen, and a taskbar that moves or resizes changes the area
        // it may use. Both are put right the same way as any change of size.
        case WM_DISPLAYCHANGE:
            if (panel && panel->visible()) panel->Refit();
            return 0;
        case WM_SETTINGCHANGE:
            if (panel && panel->visible() && wParam == SPI_SETWORKAREA) panel->Refit();
            break;
        case WM_CAPTURECHANGED:
            // Capture can be taken away, and neither a hold nor a drag may
            // outlive it.
            if (panel) {
                panel->draggingSlider_ = false;
                panel->EndHold();
            }
            return 0;
        case WM_TIMER:
            if (!panel) break;
            if (wParam == kPassiveHideTimer) {
                KillTimer(window, kPassiveHideTimer);
                // Not in the middle of something: a button held or the thumb
                // being dragged means someone is using it after all.
                if (!panel->passive_) return 0;
                POINT cursor = {};
                RECT area = {};
                GetCursorPos(&cursor);
                GetWindowRect(window, &area);
                if (panel->held_ >= 0 || panel->draggingSlider_ ||
                    PtInRect(&area, cursor)) {
                    // The pointer is on it, so it may be about to be used.
                    SetTimer(window, kPassiveHideTimer, kPassiveHideMs, nullptr);
                    return 0;
                }
                panel->Hide();
                // Gone by itself, not closed by a click, so the next click on
                // the tray icon opens it rather than counting as the close.
                panel->lastHiddenTick_ = 0;
                return 0;
            }
            if (wParam == kPollTimer) {
                // Not during a hold, whose own peeks already follow the set,
                // and not during a drag, where an answer that left before the
                // drag began would yank the thumb back to where it was.
                const DWORD now = GetTickCount();
                const bool local = panel->levelsAreLocal_ && panel->levelsAreLocal_();
                if (!local) {
                    const bool quiet = !panel->lastInteractionTick_ ||
                                       now - panel->lastInteractionTick_ >= kPollQuietMs;
                    if (!quiet || now - panel->lastPollTick_ < kPollMs) return 0;
                }
                if (!panel->draggingSlider_ && panel->held_ < 0 && panel->sender_) {
                    panel->lastPollTick_ = now;
                    panel->sender_("volume-poll");
                }
                return 0;
            }
            if (wParam == kHoldTimer) {
                panel->EndHold();
            } else if (wParam == kRepeatTimer && panel->held_ >= 0) {
                // Let go the moment the button is no longer down, whether or
                // not the message that says so ever arrived. This is what lets
                // the hold ceiling be generous rather than protective.
                if (!PrimaryButtonDown()) {
                    panel->EndHold();
                    return 0;
                }
                // Re-assert the held key so the device keeps repeating it.
                if (panel->sender_) {
                    if (const char* command = PressCommand(panel->held_)) {
                        panel->sender_(command);
                    }
                }
            }
            return 0;
        case WM_ACTIVATE:
            if (!panel) return 0;
            if (LOWORD(wParam) == WA_INACTIVE) {
                // Dismiss when focus moves elsewhere, the way a menu would.
                panel->Hide();
            } else if (panel->passive_) {
                // Clicked while up only for a key: now it is open, and it stays
                // until focus moves away, like any other time.
                panel->KeepOpen(false);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && panel) {
                if (panel->outputsOpen_) {
                    panel->outputsOpen_ = false;
                    panel->hot_ = -1;
                    panel->Refit();
                } else {
                    panel->Hide();
                }
            }
            return 0;
        case WM_CLOSE:
            if (panel) panel->Hide();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace tray
