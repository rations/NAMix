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

    //---from CPluginView-------------
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
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

    // Pointer input. Buttons are X button numbers (1 = left, 2 = middle,
    // 3 = right). Wheel steps arrive as delta +1 (up) or -1 (down).
    virtual void onMouseDown(int x, int y, int button)
    {
        (void)x, (void)y, (void)button;
    }
    virtual void onMouseUp(int x, int y, int button)
    {
        (void)x, (void)y, (void)button;
    }
    virtual void onMouseMove(int x, int y)
    {
        (void)x, (void)y;
    }
    virtual void onMouseLeave()
    {
    }
    virtual void onMouseWheel(int x, int y, int delta)
    {
        (void)x, (void)y, (void)delta;
    }

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

private:
    bool openWindow(::Window parent);
    void closeWindow();
    void drainEvents();
    void redraw();
    void mapWindow();
    void ensureMapped();

    ::Display *mDisplay = nullptr;
    ::Window mWindow = 0;
    Atom mXEmbedInfoAtom = None;
    cairo_surface_t *mTarget = nullptr; // xlib surface for mWindow
    cairo_surface_t *mBuffer = nullptr; // offscreen image surface

    IPtr<Linux::IRunLoop> mRunLoop;
    bool mEventHandlerRegistered = false;
    bool mTimerRegistered = false;
    bool mDirty = true;
    bool mMapped = false;
    int mTicksUnmapped = 0;
};

//------------------------------------------------------------------------
} // namespace Steinberg
