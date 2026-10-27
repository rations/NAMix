// X11PlugView — reusable base class for X11-embedded VST3 plug-in editors.
//
// Implements the kPlatformTypeX11EmbedWindowID contract from
// pluginterfaces/gui/iplugview.h: the host passes an X11 Window ID (as a
// void*) to IPlugView::attached(); the plug-in creates one child window inside
// it and paints that window itself. Coordinates are physical pixels.
//
// The structural difference from a BView-based editor is the RUN LOOP. On
// Haiku every window owns a message-loop thread, so a plug-in can simply run
// there. On X11 the plug-in owns NO thread at all — the SDK's own comment on
// Linux::IRunLoop says it plainly: "On Linux the host has to provide this
// interface to the plug-in as there's no global event run loop defined as on
// other platforms." So this class:
//
//   * asks IPlugFrame for Linux::IRunLoop;
//   * registers itself as a Linux::IEventHandler on the X connection's file
//     descriptor, so the host calls onFDIsSet() when X events are readable;
//   * registers itself as a Linux::ITimerHandler at ~30 Hz, which is what
//     drives meter decay and repaints (the counterpart of a BMessageRunner);
//   * unregisters both in removedFromParent() BEFORE tearing the window down.
//
// Painting is deliberately deferred: X events only ever set a dirty flag, and
// the actual draw happens on the next timer tick. Painting straight out of an
// event handler is how a plug-in ends up re-entering the host's run loop, and
// it is the classic reason an editor works in one host and hangs in another.
//
// Drawing is double-buffered: subclasses paint into an offscreen image surface
// through onDraw(), and the result is blitted to the window in one step.
//
// Subclasses must do all windowing-dependent setup in onAttached() rather than
// in their constructor, so that createView() stays harmless in headless hosts
// (the validator creates and destroys views without ever attaching them).

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cairo/cairo.h>

#include <X11/Xlib.h>

namespace Steinberg
{

//------------------------------------------------------------------------
class X11PlugView : public Vst::EditorView, public Linux::IEventHandler, public Linux::ITimerHandler
{
public:
    X11PlugView(Vst::EditController *controller, ViewRect *size = nullptr);
    ~X11PlugView() override;

    // Modifier keys held during a pointer event, as a mask of these bits.
    // Only the two a fine-adjust gesture needs are carried.
    enum PointerMod : unsigned {
        kPointerShift = 1u << 0,   // ShiftMask
        kPointerControl = 1u << 1, // ControlMask
    };

    //---from CPluginView-------------
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API attached(void *parent, FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API onSize(ViewRect *newSize) SMTG_OVERRIDE;
    tresult PLUGIN_API canResize() SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect *rect) SMTG_OVERRIDE;
    void attachedToParent() SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

    //---from Linux::IEventHandler----
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor fd) SMTG_OVERRIDE;

    //---from Linux::ITimerHandler----
    void PLUGIN_API onTimer() SMTG_OVERRIDE;

    //---Interface--------------------
    OBJ_METHODS(X11PlugView, Vst::EditorView)
    DEFINE_INTERFACES
    DEF_INTERFACE(Linux::IEventHandler)
    DEF_INTERFACE(Linux::ITimerHandler)
    END_DEFINE_INTERFACES(Vst::EditorView)
    REFCOUNT_METHODS(Vst::EditorView)

protected:
    // --- subclass hooks, all called on the host's run-loop thread ---

    // The window and its drawing surfaces now exist. Load resources here.
    virtual void onAttached()
    {
    }
    // The window is about to go away. Release anything tied to it.
    virtual void onRemoved()
    {
    }

    // Paint the whole editor. `cr` targets the offscreen buffer.
    virtual void onDraw(cairo_t *cr) = 0;

    // The host changed the embedded window's physical pixel dimensions.
    virtual void onResize(int width, int height)
    {
        (void)width, (void)height;
    }

    // Pointer input. Buttons are X button numbers (1 = left, 2 = middle,
    // 3 = right). Wheel steps arrive as delta +1 (up) or -1 (down). `mods`
    // is a PointerMod mask. `doubleClick` is set on the second press of the
    // same button, close in time and place to the first: X has no
    // double-click event of its own, so it is derived here once rather than
    // by every editor.
    virtual void onMouseDown(int x, int y, int button, unsigned mods, bool doubleClick)
    {
        (void)x, (void)y, (void)button, (void)mods, (void)doubleClick;
    }
    virtual void onMouseUp(int x, int y, int button)
    {
        (void)x, (void)y, (void)button;
    }
    virtual void onMouseMove(int x, int y, unsigned mods)
    {
        (void)x, (void)y, (void)mods;
    }
    virtual void onMouseLeave()
    {
    }
    virtual void onMouseWheel(int x, int y, int delta, unsigned mods)
    {
        (void)x, (void)y, (void)delta, (void)mods;
    }

    // Keyboard, taken from the PLATFORM window rather than handed in by the
    // host. Return true if the key was consumed. See setKeyboardFocus() in the
    // .cpp for why this exists alongside IPlugView::onKeyDown. The three
    // arguments are IPlugView::onKeyDown's own -- `key` an ASCII character or
    // 0, `keyCode` a VirtualKeyCodes value from pluginterfaces/base/keycodes.h
    // or 0, `modifiers` a KeyModifier mask -- so a subclass can route both to
    // one handler.
    virtual bool onKeyDownNative(char16 key, int16 keyCode, int16 modifiers)
    {
        (void)key, (void)keyCode, (void)modifiers;
        return false;
    }

    // Take the X input focus, or hand it straight back to whoever had it.
    // Called by the editor around a text field, and never held for longer
    // than one is open -- see the .cpp.
    void setKeyboardFocus(bool wanted);

    // ~30 Hz, immediately before a repaint is considered. Animation (meter
    // decay) belongs here.
    virtual void onTick()
    {
    }

    // Request a repaint on the next tick. Cheap; call it freely.
    void invalidate()
    {
        mDirty = true;
    }

    bool isWindowOpen() const
    {
        return mWindow != 0;
    }

public:
    // The child window this view paints, for a caller that has to hand it to
    // something other than IPlugView::attached().
    //
    // It exists for the LV2 build: ui:X11UI's contract is that the
    // LV2UI_Widget a UI returns from instantiate() IS an X window id, so the
    // wrapper has to be able to ask. A VST3 host never needs this — it passes
    // the parent in and never sees the child.
    ::Window nativeWindow() const
    {
        return mWindow;
    }

private:
    bool openWindow(::Window parent);
    void closeWindow();
    void drainEvents();
    void redraw();
    void mapWindow();
    void ensureMapped();
    void resizeSurfaces(unsigned width, unsigned height);

    // Diagnostics, enabled by setting NAMIX_X11_TRACE in the environment. They
    // exist because the embedding contract is host-specific and only partly
    // written down: when an editor misbehaves in one host, the first question
    // is always "which X events did we actually get, and was the window
    // viewable", and that has to be answerable from a user's machine.
    void trace(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void traceEvent(const XEvent &event) const;
    void traceWindowState(const char *when) const;

    ::Display *mDisplay = nullptr;
    ::Window mWindow = 0;
    // Only set when the parent had no colormap to share and we had to make one;
    // a borrowed parent colormap must not be freed.
    Colormap mOwnedColormap = None;
    Atom mXEmbedInfoAtom = None;
    cairo_surface_t *mTarget = nullptr; // xlib surface for mWindow
    cairo_surface_t *mBuffer = nullptr; // offscreen image surface
    unsigned mSurfaceWidth = 0, mSurfaceHeight = 0;

    IPtr<Linux::IRunLoop> mRunLoop;
    bool mEventHandlerRegistered = false;
    bool mTimerRegistered = false;
    bool mDirty = true;
    bool mMapped = false;
    int mTicksUnmapped = 0;

    // The last left/middle/right press, for double-click detection. Time is
    // the X server's millisecond clock from the event itself, never the
    // local clock, so a slow round trip cannot turn one click into two.
    unsigned int mLastPressButton = 0;
    unsigned long mLastPressTime = 0;
    int mLastPressX = 0, mLastPressY = 0;

    // Keyboard focus, held only while a text field is open. mPrevFocus is
    // whatever had the focus when we took it, so it can be given back rather
    // than left wherever we put it.
    bool mKeyFocus = false;
    ::Window mPrevFocus = None;
    int mPrevRevert = RevertToParent;

    bool mTrace = false;
    unsigned long mTickCount = 0;
    unsigned long mFdCount = 0;
    unsigned long mEventCount = 0;
    unsigned long mRedrawCount = 0;
    unsigned long mTicksSinceRedraw = 0;
};

//------------------------------------------------------------------------
} // namespace Steinberg
