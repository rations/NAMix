// X11PlugView implementation. See x11plugview.h for the run-loop contract.

#include "x11plugview.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xutil.h>

#include <cstdio>
#include <cstring>

namespace Steinberg
{

namespace
{
// ~30 Hz, matching the original plug-in's meter refresh.
constexpr Linux::TimerInterval kTimerMs = 33;

// X wheel events arrive as presses of buttons 4 (up) and 5 (down).
constexpr unsigned int kWheelUp = 4;
constexpr unsigned int kWheelDown = 5;

// XEmbed (https://standards.freedesktop.org/xembed-spec/): _XEMBED_INFO is two
// CARD32s, {version, flags}. XEMBED_MAPPED asks the embedder to map us.
constexpr unsigned long kXEmbedVersion = 0;
constexpr unsigned long kXEmbedMapped = 1UL << 0;

// How many timer ticks to wait for an embedder to map us before doing it
// ourselves (see ensureMapped).
constexpr int kMapFallbackTicks = 6; // ~200 ms at 33 ms
} // namespace

//------------------------------------------------------------------------
X11PlugView::X11PlugView(Vst::EditController *controller, ViewRect *size)
    : Vst::EditorView(controller, size)
{
}

X11PlugView::~X11PlugView()
{
    // removedFromParent() is the normal teardown path; this only covers a
    // view destroyed while still attached by a non-conforming host.
    closeWindow();
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11PlugView::isPlatformTypeSupported(FIDString type)
{
    if (type && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0)
        return kResultTrue;
    return kResultFalse;
}

//------------------------------------------------------------------------
bool X11PlugView::openWindow(::Window parent)
{
    // The host does not share its Display connection, so open our own. Its
    // file descriptor is what gets registered with the run loop below.
    mDisplay = XOpenDisplay(nullptr);
    if (!mDisplay) {
        fprintf(stderr, "NAMix: cannot open an X display for the editor\n");
        return false;
    }

    const int screen = DefaultScreen(mDisplay);
    Visual *visual = DefaultVisual(mDisplay, screen);
    const int depth = DefaultDepth(mDisplay, screen);
    const unsigned width = static_cast<unsigned>(rect.getWidth());
    const unsigned height = static_cast<unsigned>(rect.getHeight());

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixmap = None; // we paint every pixel ourselves
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       LeaveWindowMask | StructureNotifyMask | PropertyChangeMask;

    mWindow = XCreateWindow(mDisplay, parent, 0, 0, width, height, 0, depth, InputOutput, visual,
                            CWBackPixmap | CWBorderPixel | CWEventMask, &attrs);
    if (!mWindow) {
        fprintf(stderr, "NAMix: cannot create the editor window\n");
        XCloseDisplay(mDisplay);
        mDisplay = nullptr;
        return false;
    }

    // Announce XEmbed support BEFORE the parent's connection can see the
    // window, because a strict embedder reads this the moment it gets the
    // CreateNotify. The SDK's own reference host does exactly that and calls
    // it a fatal error if the property is missing.
    //
    // flags is 0, NOT XEMBED_MAPPED: under XEmbed the embedder owns mapping,
    // and a window already mapped at CreateNotify is an error to that same
    // reference host. ensureMapped() below covers embedders that never get
    // round to mapping us.
    mXEmbedInfoAtom = XInternAtom(mDisplay, "_XEMBED_INFO", False);
    if (mXEmbedInfoAtom != None) {
        const unsigned long info[2] = {kXEmbedVersion, 0};
        XChangeProperty(mDisplay, mWindow, mXEmbedInfoAtom, mXEmbedInfoAtom, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(info), 2);
    }
    XFlush(mDisplay);

    mTarget = cairo_xlib_surface_create(mDisplay, mWindow, visual, static_cast<int>(width),
                                        static_cast<int>(height));
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width),
                                         static_cast<int>(height));
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "NAMix: cannot create the editor drawing surfaces\n");
        closeWindow();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------
void X11PlugView::closeWindow()
{
    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDisplay) {
        if (mWindow) {
            XDestroyWindow(mDisplay, mWindow);
            mWindow = 0;
        }
        XCloseDisplay(mDisplay);
        mDisplay = nullptr;
    }
    mWindow = 0;
}

//------------------------------------------------------------------------
void X11PlugView::attachedToParent()
{
    const ::Window parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(systemWindow));
    if (!parent)
        return;

    if (!mWindow && !openWindow(parent))
        return;

    // The run loop belongs to the host and reaches us through IPlugFrame.
    // Without it the editor cannot receive X events or tick, so say so
    // loudly rather than presenting a window that never repaints.
    if (plugFrame) {
        Linux::IRunLoop *runLoop = nullptr;
        if (plugFrame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void **>(&runLoop)) ==
                kResultTrue &&
            runLoop) {
            mRunLoop = owned(runLoop);
        }
    }

    if (mRunLoop) {
        if (mRunLoop->registerEventHandler(this, ConnectionNumber(mDisplay)) == kResultTrue)
            mEventHandlerRegistered = true;
        else
            fprintf(stderr, "NAMix: the host refused to register the editor's event handler\n");

        if (mRunLoop->registerTimer(this, kTimerMs) == kResultTrue)
            mTimerRegistered = true;
        else
            fprintf(stderr, "NAMix: the host refused to register the editor's timer\n");
    } else {
        fprintf(stderr, "NAMix: this host provides no Linux::IRunLoop; "
                        "the editor cannot receive events\n");
    }

    onAttached();
    mDirty = true;

    // Notify the controller (EditorView::attachedToParent -> editorAttached)
    // only once the window exists, so it may immediately push values in.
    Vst::EditorView::attachedToParent();

    redraw();
}

//------------------------------------------------------------------------
void X11PlugView::removedFromParent()
{
    // Controller first, so it stops touching this view before anything is
    // torn down.
    Vst::EditorView::removedFromParent();

    // Then the run loop: unregistering BEFORE the window and display go away
    // is what stops the host calling onFDIsSet() on a closed connection.
    if (mRunLoop) {
        if (mEventHandlerRegistered)
            mRunLoop->unregisterEventHandler(this);
        if (mTimerRegistered)
            mRunLoop->unregisterTimer(this);
    }
    mEventHandlerRegistered = false;
    mTimerRegistered = false;
    mRunLoop = nullptr;

    onRemoved();
    closeWindow();
}

//------------------------------------------------------------------------
void X11PlugView::onFDIsSet(Linux::FileDescriptor /*fd*/)
{
    drainEvents();
}

//------------------------------------------------------------------------
void X11PlugView::drainEvents()
{
    if (!mDisplay)
        return;

    while (XPending(mDisplay)) {
        XEvent event;
        XNextEvent(mDisplay, &event);
        if (event.xany.window != mWindow)
            continue;

        switch (event.type) {
            case Expose:
                // Coalesce: only the last expose in a burst needs a repaint,
                // and the repaint itself happens on the next tick.
                if (event.xexpose.count == 0)
                    mDirty = true;
                break;

            case ButtonPress:
                if (event.xbutton.button == kWheelUp || event.xbutton.button == kWheelDown) {
                    onMouseWheel(event.xbutton.x, event.xbutton.y,
                                 event.xbutton.button == kWheelUp ? 1 : -1);
                } else {
                    onMouseDown(event.xbutton.x, event.xbutton.y,
                                static_cast<int>(event.xbutton.button));
                }
                break;

            case ButtonRelease:
                if (event.xbutton.button != kWheelUp && event.xbutton.button != kWheelDown)
                    onMouseUp(event.xbutton.x, event.xbutton.y,
                              static_cast<int>(event.xbutton.button));
                break;

            case MotionNotify: {
                // Compress motion: only the most recent position matters for
                // a knob drag, and X can deliver these far faster than 30 Hz.
                XEvent latest = event;
                while (XPending(mDisplay)) {
                    XEvent peek;
                    XPeekEvent(mDisplay, &peek);
                    if (peek.type != MotionNotify || peek.xany.window != mWindow)
                        break;
                    XNextEvent(mDisplay, &latest);
                }
                onMouseMove(latest.xmotion.x, latest.xmotion.y);
                break;
            }

            case LeaveNotify:
                onMouseLeave();
                break;

            case MapNotify:
                mMapped = true;
                mDirty = true;
                break;

            case UnmapNotify:
                mMapped = false;
                break;

            case PropertyNotify:
                // Reaper sets _XEMBED_INFO on the plug-in's window rather than
                // mapping it, and expects the plug-in to map itself in
                // response. VSTGUI carries the same workaround, flagged
                // "needed for Reaper"; without it the editor never appears.
                if (mXEmbedInfoAtom != None && event.xproperty.atom == mXEmbedInfoAtom)
                    mapWindow();
                break;

            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
void X11PlugView::onTimer()
{
    ensureMapped();
    onTick();
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void X11PlugView::mapWindow()
{
    if (!mDisplay || !mWindow || mMapped)
        return;
    XMapWindow(mDisplay, mWindow);
    XFlush(mDisplay);
    mMapped = true;
    mDirty = true;
}

//------------------------------------------------------------------------
// Hosts differ on who maps the plug-in's window. A strict XEmbed embedder maps
// it (and rejects a window that mapped itself); Reaper signals via
// _XEMBED_INFO; and some hosts do neither. Waiting a few ticks and then
// mapping ourselves is what makes all three work: by then a conforming
// embedder has already mapped us and this is a no-op.
void X11PlugView::ensureMapped()
{
    if (mMapped || !mDisplay || !mWindow)
        return;
    if (++mTicksUnmapped < kMapFallbackTicks)
        return;
    mapWindow();
}

//------------------------------------------------------------------------
void X11PlugView::redraw()
{
    if (!mBuffer || !mTarget || !mDisplay)
        return;
    mDirty = false;

    // Compose into the offscreen buffer...
    cairo_t *cr = cairo_create(mBuffer);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS)
        onDraw(cr);
    cairo_destroy(cr);

    // ...then blit it to the window in one operation, so no partially drawn
    // frame is ever visible.
    cairo_t *out = cairo_create(mTarget);
    if (cairo_status(out) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(out, mBuffer, 0.0, 0.0);
        cairo_paint(out);
    }
    cairo_destroy(out);

    cairo_surface_flush(mTarget);
    XFlush(mDisplay);
}

//------------------------------------------------------------------------
} // namespace Steinberg
