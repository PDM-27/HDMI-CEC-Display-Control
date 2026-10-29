// VolumePanel.h - a small always-on-top window holding the volume buttons.
//
// This exists because a Win32 popup menu cannot do what volume needs. A menu
// dismisses the moment anything in it is clicked, which makes repeated presses
// impossible. Opened by left-clicking the tray icon; the right-click menu still
// holds everything else.
//
// The circles are drawn straight onto this window and hit-tested by distance
// from their centres. There are deliberately no child BUTTON controls: a child
// HWND owns a rectangular patch of the window and has to fill it, which fights
// a translucent parent. That conflict is what produced the visible squares
// behind the buttons, and it also made the corners of those squares clickable
// even though nothing was drawn there.
#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "AudioOutputs.h"

namespace tray {

class VolumePanel {
public:
    // Called with a pipe command name, e.g. "press-volume-up" or "release".
    using Sender = std::function<void(const char*)>;

    // Whether volume is going to this PC's own output right now, where setting
    // a level is a local call rather than a frame on a slow bus.
    using LocalCheck = std::function<bool()>;

    ~VolumePanel();

    bool Create(HINSTANCE instance, Sender sender, LocalCheck levelsAreLocal);

    // Shows the panel near the cursor, or closes it if the click that arrived
    // is the one that dismissed it. Returns true if the panel was opened, so
    // the caller can warm the CEC link only when there is a panel to use.
    bool Toggle();
    void Hide();

    // Shows the panel without taking focus, anchored at a point - normally the
    // tray icon - and hides it again a couple of seconds later unless someone
    // starts using it. For volume changed from a keyboard, a knob, or a remote:
    // the number should be seen, and whatever has focus, a game above all, must
    // keep it. Calling it again while it shows this way restarts the countdown;
    // a panel someone opened by hand is left exactly as it is.
    void ShowPassive(POINT anchor);

    bool visible() const { return window_ && IsWindowVisible(window_); }

    // Windows' sound outputs may have changed: one came or went, or another
    // became the default. An open picker is brought up to date; a closed or
    // key-shown panel has nothing to update.
    void OutputsChanged();

    // Lets go of a held button or a slider drag, sending the release, without
    // closing the panel. For when volume is about to go somewhere else.
    void ReleaseHeld();

    // The volume level to show above the buttons, or nothing.
    //
    // Deliberately a single call that carries "we do not know" as a state
    // rather than a level of -1 or a stale last value. A number that has gone
    // quietly wrong is worse than no number: it looks like information. When
    // this is called with known=false the panel returns to exactly the size and
    // shape it has always had, so a television that cannot report its volume
    // shows no gap where a readout would have been.
    void SetReadout(bool known, int level, bool muted);

    bool readoutKnown() const { return readoutKnown_; }

    // Whether the thing that owns volume will act on an absolute level at all.
    //
    // Called false when the device has answered <Set Audio Volume Level> with
    // <Feature Abort>, and the bar then loses its thumb and stops being
    // draggable: it still shows the level, which is worth having, but it no
    // longer offers a gesture that cannot work. Measured on a Polk Mini AX
    // soundbar, which reports its volume faithfully and ignores every level
    // sent to it, so the thumb sprang back to the real number each time a drag
    // ended. Stepping there with volume keys instead was considered and
    // dropped: a drag of sixty steps is six seconds of key presses, and a
    // control that takes six seconds is not a control.
    //
    // Volume routed to this PC's own output is unaffected, because that path
    // sets a level through Windows and never touches the bus. See LocalCheck.
    void SetLevelsSettable(bool settable);

    // True while the slider is being dragged. The thumb belongs to the pointer
    // for the whole gesture, so nothing arriving from the television may move
    // it until the drag ends.
    bool dragging() const { return draggingSlider_; }

private:
    // Themes, lays out, and positions the panel just above a point, kept on the
    // monitor that point is on. Shared by opening from a click and by
    // ShowPassive, which differ only in where they point and whether they take
    // focus.
    void PlaceNear(POINT point);

    // A panel up only for a volume key that someone has now clicked, or
    // clicked the tray icon for: it becomes an ordinary open panel, polling,
    // and closing when focus leaves rather than on a countdown. The picker
    // appears with it, straight away or, when the click that did this is still
    // in progress on the panel, once the button is let go - see
    // loadOutputsOnRelease_.
    void KeepOpen(bool pickerNow);

    // ---- the sound output picker
    //
    // A row under the buttons names the current output. Clicking it opens the
    // list of outputs above it, inside this same window: a second popup would
    // be a second window to keep translucent, themed, on screen, and in step
    // with this one's focus, and every one of those is a way for it to go
    // wrong. Growing this window upward keeps the row under the pointer.

    // Only when opened by hand, only where Windows allows the choice, and only
    // with something to choose between.
    bool ShowsOutputs() const;
    int OutputRowHeight() const;
    RECT OutputRowRect() const;
    RECT OutputListRect() const;

    // The window's height for what is showing now.
    int ContentHeight() const;

    // Reads the outputs from Windows again. Closes the list if there is no
    // longer anything to choose between.
    void LoadOutputs();

    // How many list rows fit: never more than kMaxOutputRows, never taller than
    // the monitor allows, never fewer than one. The rest scroll.
    void FitOutputRows(HMONITOR monitor);

    // After anything that changes the panel's size - the readout appearing,
    // the picker opening, a scale, resolution, or rotation change - lays it out
    // again and puts it back on screen with its bottom centre where it was.
    void Refit();

    std::vector<AudioOutput> outputs_;
    bool outputsOpen_ = false;
    int outputsScroll_ = 0;    // the first list row showing
    int outputRows_ = 0;       // list rows showing, while open
    int wheelRemainder_ = 0;   // touchpads scroll in fractions of a notch
    // A click on a key-shown panel turns it into an open one, and the picker
    // growing under that same click would move the buttons out from under it.
    // So the picker waits for the button to come up.
    bool loadOutputsOnRelease_ = false;

    // Shows the picker that loadOutputsOnRelease_ held back, if it did.
    void LoadDeferredOutputs();

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam);

    // Sizes are rebuilt from the DPI of whichever monitor the panel is about to
    // appear on, so it stays sharp on mixed-DPI setups.
    //
    // The monitor is passed in rather than asked of the window, and that is the
    // whole point of the parameter. A hidden window has not been placed
    // anywhere yet, so GetDpiForWindow answers for wherever it was last left -
    // which after a scale change is a stale number that nothing ever corrects.
    void Relayout(HMONITOR monitor);

    // Builds the whole panel as a premultiplied ARGB bitmap and hands it to
    // UpdateLayeredWindow. Replaces WM_PAINT entirely: per-pixel alpha is
    // the only way to be sure nothing is drawn outside the circles.
    void Render();

    // Index of the circle under a point, or -1. Circular rather than
    // rectangular, so the gaps between buttons do nothing - which is what
    // anyone looking at three circles expects.
    int HitTest(POINT point) const;

    // Height of the readout band above the buttons, or zero when there is
    // nothing to show. Everything else in the layout is measured from it.
    int ReadoutHeight() const;

    // The readout strip, exactly half a button tall, or zero when there is
    // nothing to show in it.
    int PillHeight() const;

    // The draggable part of the strip: the track between the mute indicator on
    // the left and the percentage on the right.
    RECT SliderRect() const;

    // Turns an x within the track into a 0-100 level.
    int LevelAtX(int x) const;

    // Moves the thumb to a level at once, on every call, and sends it. The
    // thumb is only drawing, so it keeps up with the pointer however fast that
    // moves. Sending is another matter for a TV: a drag produces a mouse move
    // per pixel and each send is a CEC frame, so there a level goes at most
    // every kSliderSendMs, and only when it has changed. To this PC's own
    // output every change goes straight through, the way Windows' slider does.
    void SendLevel(int level, bool force);
    RECT CircleRect(int index) const;
    void Invalidate(int index);

    // Holding a button makes the TV repeat the key by itself, so a hold costs
    // two CEC frames in total rather than two per step. The release must always
    // follow, so every path that can end a hold routes through EndHold.
    void BeginHold(int index);
    void EndHold();

    HWND window_ = nullptr;

    // What to show above the buttons. readoutKnown_ false means show nothing.
    bool readoutKnown_ = false;
    int readoutLevel_ = 0;
    bool readoutMuted_ = false;

    // Whether an absolute level reaches anything. See SetLevelsSettable.
    bool levelsSettable_ = true;
    bool LevelsWork() const;

    // Slider drag state. draggingSlider_ is separate from held_, which belongs
    // to the buttons: a drag that began on the track must not be read as a
    // button press when it passes over one.
    bool draggingSlider_ = false;
    int lastSentLevel_ = -1;
    DWORD lastSendTick_ = 0;

    // When a button or the slider was last touched. The poll stays out of the
    // way for a moment after, so it never queues a frame ahead of a click.
    DWORD lastInteractionTick_ = 0;
    // When the level was last asked for while open. See kPollTimer.
    DWORD lastPollTick_ = 0;

    // Starts the open panel's readout poll.
    void StartPolling();
    // Null if neither Windows icon font is installed, in which case the buttons
    // fall back to plain characters rather than rendering tofu boxes.
    const wchar_t* iconFont_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    Sender sender_;
    LocalCheck levelsAreLocal_;

    int width_ = 0;
    int height_ = 0;
    int diameter_ = 0;
    int padding_ = 0;
    int x_ = 0;
    int y_ = 0;
    // When the panel last hid itself, so a tray click arriving just afterwards
    // is recognised as the close rather than treated as a fresh open.
    DWORD lastHiddenTick_ = 0;
    int hot_ = -1;
    // Showing only because a volume key was used, rather than opened by hand.
    bool passive_ = false;
    int held_ = -1;
    bool tracking_ = false;
    bool dark_ = false;
};

}  // namespace tray
