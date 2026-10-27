// X11PlugView implementation. See x11plugview.h for the run-loop contract.

#include "x11plugview.h"

#include "pluginterfaces/base/keycodes.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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

// One trace summary line per ~1 s, and the threshold above which a repaint is
// interesting enough to log on its own (i.e. the editor was quiet and then
// something woke it up).
constexpr unsigned long kSummaryTicks = 30;
constexpr unsigned long kQuietRedrawTicks = 15;

// Two presses of one button count as a double-click when the second follows
// within this many milliseconds and this many pixels of the first. X itself
// defines no double-click, so these are this editor's own choice, not a
// value read from the desktop's settings.
constexpr unsigned long kDoubleClickMs = 400;
constexpr int kDoubleClickSlop = 4;

unsigned pointerMods(unsigned int state)
{
    unsigned mods = 0;
    if (state & ShiftMask)
        mods |= X11PlugView::kPointerShift;
    if (state & ControlMask)
        mods |= X11PlugView::kPointerControl;
    return mods;
}

//------------------------------------------------------------------------
// One X keysym to one VirtualKeyCodes value (pluginterfaces/base/keycodes.h),
// or 0 for a key that has no virtual code and is therefore carried by its
// character instead. Only the keys a text field needs are mapped: the point of
// this table is editing, not a general key API.
int16 virtualKeyFromKeySym(KeySym sym)
{
    switch (sym) {
        case XK_BackSpace:
            return KEY_BACK;
        case XK_Tab:
            return KEY_TAB;
        case XK_Return:
            return KEY_RETURN;
        case XK_KP_Enter:
            return KEY_ENTER;
        case XK_Escape:
            return KEY_ESCAPE;
        case XK_Delete:
        case XK_KP_Delete:
            return KEY_DELETE;
        case XK_Left:
        case XK_KP_Left:
            return KEY_LEFT;
        case XK_Right:
        case XK_KP_Right:
            return KEY_RIGHT;
        case XK_Home:
        case XK_KP_Home:
            return KEY_HOME;
        case XK_End:
        case XK_KP_End:
            return KEY_END;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// Non-fatal X error handling.
//
// Xlib's default error handler calls exit(), so one BadWindow — a host that
// destroys its container before calling removed(), a stale resource id after a
// re-embed — takes the whole host process down with it. Replace it with a
// handler that logs and returns.
//
// Ownership/threading: gOurDisplays, gPreviousErrorHandler and gErrorHandlerOnce
// are process-wide and guarded by gErrorMutex. Displays are added in
// openWindow() and removed in closeWindow(), both of which run on the host's
// run-loop thread; the handler itself can be entered from any thread that makes
// an X call, which is why the lock is taken there too. Errors on a display that
// is not ours are forwarded to whatever handler the host had installed, so this
// never swallows the host's own diagnostics.
std::mutex gErrorMutex;
std::vector<::Display *> gOurDisplays;
XErrorHandler gPreviousErrorHandler = nullptr;
std::once_flag gErrorHandlerOnce;

// Counts errors on our own connections. X requests are asynchronous, so a
// rejected CreateWindow does not fail in place — the only way to find out is to
// sample this, round-trip, and sample it again.
std::atomic<unsigned long> gErrorCount{0};

int xErrorHandler(::Display *display, XErrorEvent *event)
{
    bool ours = false;
    XErrorHandler previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(gErrorMutex);
        for (::Display *d : gOurDisplays) {
            if (d == display) {
                ours = true;
                break;
            }
        }
        previous = gPreviousErrorHandler;
    }

    if (!ours && previous)
        return previous(display, event);

    gErrorCount.fetch_add(1);

    char text[128];
    text[0] = '\0';
    XGetErrorText(display, event->error_code, text, sizeof(text));
    fprintf(stderr, "NAMix: X error %u (%s) on request %u.%u, resource 0x%lx — ignored\n",
            static_cast<unsigned>(event->error_code), text,
            static_cast<unsigned>(event->request_code), static_cast<unsigned>(event->minor_code),
            static_cast<unsigned long>(event->resourceid));
    return 0;
}

void registerDisplay(::Display *display)
{
    std::call_once(gErrorHandlerOnce,
                   [] { gPreviousErrorHandler = XSetErrorHandler(xErrorHandler); });
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gOurDisplays.push_back(display);
}

void unregisterDisplay(::Display *display)
{
    std::lock_guard<std::mutex> lock(gErrorMutex);
    for (size_t i = 0; i < gOurDisplays.size(); ++i) {
        if (gOurDisplays[i] == display) {
            gOurDisplays.erase(gOurDisplays.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

//------------------------------------------------------------------------
// Trace helpers. Everything here is inert unless NAMIX_X11_TRACE is set.
const char *eventName(int type)
{
    // Indexed by the X protocol event codes in X11/X.h (KeyPress == 2 ...
    // GenericEvent == 35).
    // clang-format off
    static const char *const kNames[] = {
        "0",              "1",              "KeyPress",         "KeyRelease",
        "ButtonPress",    "ButtonRelease",  "MotionNotify",     "EnterNotify",
        "LeaveNotify",    "FocusIn",        "FocusOut",         "KeymapNotify",
        "Expose",         "GraphicsExpose", "NoExpose",         "VisibilityNotify",
        "CreateNotify",   "DestroyNotify",  "UnmapNotify",      "MapNotify",
        "MapRequest",     "ReparentNotify", "ConfigureNotify",  "ConfigureRequest",
        "GravityNotify",  "ResizeRequest",  "CirculateNotify",  "CirculateRequest",
        "PropertyNotify", "SelectionClear", "SelectionRequest", "SelectionNotify",
        "ColormapNotify", "ClientMessage",  "MappingNotify",    "GenericEvent"};
    // clang-format on
    if (type < 0 || type >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
        return "?";
    return kNames[type];
}

const char *mapStateName(int state)
{
    switch (state) {
        case IsUnmapped:
            return "IsUnmapped";
        case IsUnviewable:
            return "IsUnviewable";
        case IsViewable:
            return "IsViewable";
        default:
            return "?";
    }
}

const char *visibilityName(int state)
{
    switch (state) {
        case VisibilityUnobscured:
            return "Unobscured";
        case VisibilityPartiallyObscured:
            return "PartiallyObscured";
        case VisibilityFullyObscured:
            return "FullyObscured";
        default:
            return "?";
    }
}
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
// CPluginView::attached() always reports success, which would leave a host
// believing in an editor that does not exist. Report what actually happened
// instead: a host that is told the attach failed can fall back to its generic
// parameter panel rather than showing an empty rectangle.
tresult PLUGIN_API X11PlugView::attached(void *parent, FIDString type)
{
    const tresult result = CPluginView::attached(parent, type);
    if (result != kResultOk)
        return result;
    return isWindowOpen() ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11PlugView::onSize(ViewRect *newSize)
{
    if (!newSize || newSize->getWidth() <= 0 || newSize->getHeight() <= 0)
        return kInvalidArgument;
    const tresult result = Vst::EditorView::onSize(newSize);
    if (result != kResultTrue && result != kResultOk)
        return result;
    resizeSurfaces(static_cast<unsigned>(newSize->getWidth()),
                   static_cast<unsigned>(newSize->getHeight()));
    return kResultTrue;
}

// Keep the panel's 3:2 layout intact while allowing the host to resize the
// editor. Whichever dimension changed less determines the resulting size.
tresult PLUGIN_API X11PlugView::checkSizeConstraint(ViewRect *requested)
{
    if (!requested)
        return kInvalidArgument;
    constexpr double aspect = 1.5;
    const int width = requested->getWidth();
    const int height = requested->getHeight();
    if (width <= 0 || height <= 0)
        return kResultFalse;

    const int constrainedWidth = static_cast<int>(std::lround(height * aspect));
    const int constrainedHeight = static_cast<int>(std::lround(width / aspect));
    const ViewRect &current = getRect();
    if (std::abs(width - current.getWidth()) >= std::abs(height - current.getHeight()))
        requested->right = requested->left + constrainedWidth;
    else
        requested->bottom = requested->top + constrainedHeight;
    return kResultTrue;
}

//------------------------------------------------------------------------
void X11PlugView::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    fputs("NAMix/x11: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    // The host's stderr is usually a pipe, so without this the interesting
    // lines are still sitting in libc's buffer when the user hits the bug.
    fflush(stderr);
}

//------------------------------------------------------------------------
// One line per X event, logged BEFORE the "is this for our window" filter and
// before the dispatch switch, so events the editor currently ignores (XEmbed
// client messages above all) still show up.
void X11PlugView::traceEvent(const XEvent &event) const
{
    if (!mTrace)
        return;

    const char *mine = (event.xany.window == mWindow) ? "self" : "other";
    switch (event.type) {
        case Expose:
            trace("ev %s %s win=0x%lx %dx%d+%d+%d count=%d", eventName(event.type), mine,
                  event.xany.window, event.xexpose.width, event.xexpose.height, event.xexpose.x,
                  event.xexpose.y, event.xexpose.count);
            break;
        case VisibilityNotify:
            trace("ev %s %s win=0x%lx state=%s", eventName(event.type), mine, event.xany.window,
                  visibilityName(event.xvisibility.state));
            break;
        case ConfigureNotify:
            trace("ev %s %s win=0x%lx %dx%d+%d+%d above=0x%lx", eventName(event.type), mine,
                  event.xany.window, event.xconfigure.width, event.xconfigure.height,
                  event.xconfigure.x, event.xconfigure.y, event.xconfigure.above);
            break;
        case ReparentNotify:
            trace("ev %s %s win=0x%lx newparent=0x%lx at +%d+%d", eventName(event.type), mine,
                  event.xany.window, event.xreparent.parent, event.xreparent.x, event.xreparent.y);
            break;
        case PropertyNotify: {
            char *name = XGetAtomName(event.xany.display, event.xproperty.atom);
            trace("ev %s %s win=0x%lx atom=%s state=%s", eventName(event.type), mine,
                  event.xany.window, name ? name : "?",
                  event.xproperty.state == PropertyNewValue ? "NewValue" : "Delete");
            if (name)
                XFree(name);
            break;
        }
        case ClientMessage: {
            char *name = XGetAtomName(event.xany.display, event.xclient.message_type);
            trace("ev %s %s win=0x%lx type=%s fmt=%d data=%ld,%ld,%ld,%ld,%ld",
                  eventName(event.type), mine, event.xany.window, name ? name : "?",
                  event.xclient.format, event.xclient.data.l[0], event.xclient.data.l[1],
                  event.xclient.data.l[2], event.xclient.data.l[3], event.xclient.data.l[4]);
            if (name)
                XFree(name);
            break;
        }
        case MotionNotify:
            // Compressed below and far too frequent to log one by one.
            break;
        default:
            trace("ev %s %s win=0x%lx", eventName(event.type), mine, event.xany.window);
            break;
    }
}

//------------------------------------------------------------------------
// The ground truth the event stream cannot give us: whether the server thinks
// our window is viewable, where it is, and how it is stacked against the other
// plug-ins' windows in the host's container.
void X11PlugView::traceWindowState(const char *when) const
{
    if (!mTrace || !mDisplay || !mWindow)
        return;

    XWindowAttributes attrs;
    if (XGetWindowAttributes(mDisplay, mWindow, &attrs) == 0) {
        trace("%s: cannot read our own window attributes", when);
        return;
    }
    trace("%s: self 0x%lx %s %dx%d+%d+%d mapped=%d dirty=%d", when, mWindow,
          mapStateName(attrs.map_state), attrs.width, attrs.height, attrs.x, attrs.y,
          mMapped ? 1 : 0, mDirty ? 1 : 0);

    const ::Window parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(systemWindow));
    if (!parent)
        return;

    XWindowAttributes parentAttrs;
    if (XGetWindowAttributes(mDisplay, parent, &parentAttrs) == 0) {
        trace("%s: cannot read parent 0x%lx attributes", when, parent);
        return;
    }
    trace("%s: parent 0x%lx %s %dx%d+%d+%d", when, parent, mapStateName(parentAttrs.map_state),
          parentAttrs.width, parentAttrs.height, parentAttrs.x, parentAttrs.y);

    // Siblings, bottom-to-top: this is what tells us whether another plug-in's
    // window is simply stacked over ours.
    ::Window root = 0, treeParent = 0, *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(mDisplay, parent, &root, &treeParent, &children, &count) == 0)
        return;
    for (unsigned int i = 0; i < count && i < 8; ++i) {
        XWindowAttributes child;
        if (XGetWindowAttributes(mDisplay, children[i], &child) == 0)
            continue;
        trace("%s:   child[%u] 0x%lx %s %dx%d+%d+%d%s", when, i, children[i],
              mapStateName(child.map_state), child.width, child.height, child.x, child.y,
              children[i] == mWindow ? "  <-- us" : "");
    }
    if (count > 8)
        trace("%s:   ... %u more children", when, count - 8);
    if (children)
        XFree(children);
}

//------------------------------------------------------------------------
bool X11PlugView::openWindow(::Window parent)
{
    mTrace = std::getenv("NAMIX_X11_TRACE") != nullptr;

    // The host does not share its Display connection, so open our own. Its
    // file descriptor is what gets registered with the run loop below.
    mDisplay = XOpenDisplay(nullptr);
    if (!mDisplay) {
        fprintf(stderr, "NAMix: cannot open an X display for the editor\n");
        return false;
    }
    // From here on an X error on this connection is ours to survive rather than
    // the host's to die of.
    registerDisplay(mDisplay);

    const int screen = DefaultScreen(mDisplay);
    const unsigned width = static_cast<unsigned>(rect.getWidth());
    const unsigned height = static_cast<unsigned>(rect.getHeight());

    // Inherit the parent's visual, depth and colormap rather than taking the
    // screen defaults. The X protocol says of CreateWindow's colormap
    // attribute: "If CopyFromParent is specified, the parent's colormap is
    // copied ... However, the window must have the same visual type as the
    // parent (or a Match error results)". Leaving the colormap at its default
    // of CopyFromParent while asking for the screen's default visual therefore
    // fails outright against a host whose container uses a different visual —
    // a 32-bit ARGB one, say, which is what compositing toolkits hand out — and
    // the editor window is then never created at all. The SDK's own reference
    // host passes CWColormap for the same reason.
    ::Window colormapRoot = RootWindow(mDisplay, screen);
    Visual *visual = DefaultVisual(mDisplay, screen);
    int depth = DefaultDepth(mDisplay, screen);
    Colormap colormap = DefaultColormap(mDisplay, screen);

    XWindowAttributes parentAttrs;
    if (XGetWindowAttributes(mDisplay, parent, &parentAttrs) != 0 && parentAttrs.visual) {
        visual = parentAttrs.visual;
        depth = parentAttrs.depth;
        colormap = parentAttrs.colormap;
        if (parentAttrs.root)
            colormapRoot = parentAttrs.root;
    } else {
        fprintf(stderr, "NAMix: cannot read the host window's attributes; falling back "
                        "to the screen's default visual\n");
    }

    // A parent with no colormap of its own cannot lend us one either ("the
    // parent must not have a colormap of None"), so make one for its visual.
    if (colormap == None) {
        colormap = XCreateColormap(mDisplay, colormapRoot, visual, AllocNone);
        mOwnedColormap = colormap;
    }

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixmap = None; // we paint every pixel ourselves
    attrs.border_pixel = 0;         // required whenever our depth differs from the parent's
    attrs.colormap = colormap;
    // KeyPressMask departs from the SDK, which says a view "must not handle
    // keyboard events by the means of platform callbacks, but let the host
    // pass them to the view" (iplugview.h). Selecting the mask costs nothing
    // on its own: X delivers a key to this window only while it holds the
    // input focus, and focus is taken only while a text field is open (see
    // setKeyboardFocus). At every other moment keys reach the host exactly as
    // before, so its key commands are untouched.
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       LeaveWindowMask | StructureNotifyMask | PropertyChangeMask | KeyPressMask;

    const unsigned long errorsBefore = gErrorCount.load();
    mWindow = XCreateWindow(mDisplay, parent, 0, 0, width, height, 0, depth, InputOutput, visual,
                            CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask, &attrs);

    // XCreateWindow allocates the id locally and sends the request; a rejected
    // request surfaces later as a protocol error, never as a null id. Round-trip
    // once and check, because every later call — ChangeProperty, the cairo
    // surface, XMapWindow — would otherwise be issued against a window that does
    // not exist, and the editor would appear as a silent blank rectangle.
    XSync(mDisplay, False);
    if (!mWindow || gErrorCount.load() != errorsBefore) {
        fprintf(stderr,
                "NAMix: the host rejected the editor window (parent 0x%lx, depth %d, "
                "visual 0x%lx)\n",
                parent, depth, visual ? static_cast<unsigned long>(visual->visualid) : 0UL);
        mWindow = 0; // never created, so there is nothing to destroy
        closeWindow();
        return false;
    }
    trace("openWindow: created 0x%lx (%ux%u) depth=%d visual=0x%lx in parent 0x%lx, fd=%d", mWindow,
          width, height, depth, visual ? static_cast<unsigned long>(visual->visualid) : 0UL, parent,
          ConnectionNumber(mDisplay));

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
    mSurfaceWidth = width;
    mSurfaceHeight = height;
    onResize(static_cast<int>(width), static_cast<int>(height));

    return true;
}

//------------------------------------------------------------------------
// Take the X input focus for this window, or give it straight back.
//
// This departs from the SDK on purpose. iplugview.h says a view "must not
// handle keyboard events by the means of platform callbacks, but let the host
// pass them to the view", and the editor does implement IPlugView::onKeyDown
// for that route. But whether a host routes keys to an embedded view at all is
// the host's policy, and on Linux many do not: the SDK's own editorhost
// selects KeyPressMask on its window and never calls onKeyDown anywhere in its
// source. A text field that cannot be typed into is not a field.
//
// What the SDK rule protects is the host's key commands, and that is kept:
// focus is taken only while a text field is open, and handed back to whoever
// held it the moment the field closes. onKeyDown stays implemented, so a host
// that does route keys keeps working unchanged.
//
// Who holds the focus is asked of the server each time rather than tracked
// from FocusIn/FocusOut. Those events do not say what they seem to: taking the
// focus from the host's container was traced (NAMIX_X11_TRACE) sending this
// window a FocusOut immediately before its FocusIn, and treating that as a
// loss meant the focus was never handed back.
void X11PlugView::setKeyboardFocus(bool wanted)
{
    if (!mDisplay || !mWindow)
        return;
    ::Window current = None;
    int currentRevert = RevertToParent;
    XGetInputFocus(mDisplay, &current, &currentRevert);

    if (wanted) {
        if (mKeyFocus && current == mWindow)
            return; // already ours
        // XSetInputFocus on a window that is not viewable is a BadMatch, and
        // an unmapped editor is an ordinary state here (see ensureMapped).
        XWindowAttributes attrs;
        if (XGetWindowAttributes(mDisplay, mWindow, &attrs) == 0 || attrs.map_state != IsViewable)
            return;
        // Whatever held the focus just before this is what gets it back. A
        // re-take, after the host or window manager moved the focus away from
        // an open field, remembers where it was moved to.
        if (current != mWindow) {
            mPrevFocus = current;
            mPrevRevert = currentRevert;
        }
        XSetInputFocus(mDisplay, mWindow, RevertToParent, CurrentTime);
        XFlush(mDisplay);
        mKeyFocus = true;
        trace("keyboard focus taken, previous=0x%lx", mPrevFocus);
        return;
    }

    if (!mKeyFocus)
        return;
    mKeyFocus = false;
    const ::Window prev = mPrevFocus;
    mPrevFocus = None;
    // Hand back only what is still ours. If the host or window manager has
    // moved the focus since, it is not this editor's to move again.
    if (current != mWindow) {
        trace("keyboard focus already moved to 0x%lx; nothing to release", current);
        return;
    }
    // PointerRoot and None are legal focus values in their own right and are
    // handed back as they are; a real window may have been destroyed while we
    // held the focus, so it is probed first rather than trusted.
    if (prev == PointerRoot || prev == None) {
        XSetInputFocus(mDisplay, prev, mPrevRevert, CurrentTime);
    } else if (prev != mWindow) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(mDisplay, prev, &attrs) != 0)
            XSetInputFocus(mDisplay, prev, mPrevRevert, CurrentTime);
    }
    XFlush(mDisplay);
    trace("keyboard focus released to 0x%lx", prev);
}

//------------------------------------------------------------------------
void X11PlugView::closeWindow()
{
    // Never leave the focus pointed at a window that is about to stop existing.
    setKeyboardFocus(false);

    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDisplay) {
        trace("closeWindow: destroying 0x%lx", mWindow);
        if (mWindow) {
            XDestroyWindow(mDisplay, mWindow);
            mWindow = 0;
        }
        if (mOwnedColormap != None) {
            XFreeColormap(mDisplay, mOwnedColormap);
            mOwnedColormap = None;
        }
        unregisterDisplay(mDisplay);
        XCloseDisplay(mDisplay);
        mDisplay = nullptr;
    }
    mWindow = 0;
    mMapped = false;
    mTicksUnmapped = 0;
}

//------------------------------------------------------------------------
void X11PlugView::attachedToParent()
{
    const ::Window parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(systemWindow));
    if (!parent)
        return;

    mTrace = std::getenv("NAMIX_X11_TRACE") != nullptr;
    trace("attachedToParent: parent=0x%lx existing window=0x%lx mapped=%d", parent, mWindow,
          mMapped ? 1 : 0);

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
    traceWindowState("attached");
}

//------------------------------------------------------------------------
void X11PlugView::removedFromParent()
{
    trace("removedFromParent: window=0x%lx mapped=%d", mWindow, mMapped ? 1 : 0);

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
    ++mFdCount;
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
        ++mEventCount;
        traceEvent(event);
        if (event.xany.window != mWindow)
            continue;

        switch (event.type) {
            case ConfigureNotify:
                if (event.xconfigure.width > 0 && event.xconfigure.height > 0 &&
                    (static_cast<unsigned>(event.xconfigure.width) != mSurfaceWidth ||
                     static_cast<unsigned>(event.xconfigure.height) != mSurfaceHeight)) {
                    resizeSurfaces(static_cast<unsigned>(event.xconfigure.width),
                                   static_cast<unsigned>(event.xconfigure.height));
                    mDirty = true;
                }
                break;

            case Expose:
                // Coalesce: only the last expose in a burst needs a repaint,
                // and the repaint itself happens on the next tick.
                if (event.xexpose.count == 0)
                    mDirty = true;
                break;

            case ButtonPress: {
                const XButtonEvent &b = event.xbutton;
                if (b.button == kWheelUp || b.button == kWheelDown) {
                    onMouseWheel(b.x, b.y, b.button == kWheelUp ? 1 : -1, pointerMods(b.state));
                    break;
                }
                // The server clock is a 32-bit CARD32 carried in an unsigned
                // long, so the difference is taken in 32 bits to stay right
                // across its wrap.
                const bool doubleClick =
                    b.button == mLastPressButton &&
                    static_cast<uint32_t>(b.time - mLastPressTime) <= kDoubleClickMs &&
                    std::abs(b.x - mLastPressX) <= kDoubleClickSlop &&
                    std::abs(b.y - mLastPressY) <= kDoubleClickSlop;
                // A double-click consumes its pair, so a third press starts a
                // new one instead of reading as a second double-click.
                mLastPressButton = doubleClick ? 0 : b.button;
                mLastPressTime = b.time;
                mLastPressX = b.x;
                mLastPressY = b.y;
                onMouseDown(b.x, b.y, static_cast<int>(b.button), pointerMods(b.state),
                            doubleClick);
                break;
            }

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
                onMouseMove(latest.xmotion.x, latest.xmotion.y, pointerMods(latest.xmotion.state));
                break;
            }

            case LeaveNotify:
                onMouseLeave();
                break;

            case KeyPress: {
                // XLookupString applies the shift and lock state and yields
                // the Latin-1 byte. Only ASCII is passed on: the fields this
                // feeds take numbers, and a multi-byte answer is a character
                // none of them could store.
                XKeyEvent ke = event.xkey;
                char text[8] = {0};
                KeySym sym = NoSymbol;
                const int n = XLookupString(&ke, text, sizeof(text) - 1, &sym, nullptr);
                const unsigned char byte = (n >= 1) ? static_cast<unsigned char>(text[0]) : 0;
                const char16 ch = (byte >= 0x20 && byte < 0x7F) ? static_cast<char16>(byte) : 0;
                // KeyModifier's own mapping (pluginterfaces/base/keycodes.h):
                // kCommandKey is "Windows: ctrl key" and kControlKey is
                // "Windows: win key", so ControlMask is kCommandKey here and
                // Mod4 (Super) is kControlKey.
                int16 mods = 0;
                if (ke.state & ShiftMask)
                    mods |= kShiftKey;
                if (ke.state & ControlMask)
                    mods |= kCommandKey;
                if (ke.state & Mod1Mask)
                    mods |= kAlternateKey;
                if (ke.state & Mod4Mask)
                    mods |= kControlKey;
                if (onKeyDownNative(ch, virtualKeyFromKeySym(sym), mods))
                    mDirty = true;
                break;
            }

            case MapNotify:
                mMapped = true;
                mDirty = true;
                break;

            case UnmapNotify:
                mMapped = false;
                trace("UnmapNotify: unmapped by someone else, ticksUnmapped=%d", mTicksUnmapped);
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
    ++mTickCount;
    ++mTicksSinceRedraw;

    if (mTrace && mTickCount % kSummaryTicks == 0) {
        trace("tick %lu: fdCalls=%lu events=%lu redraws=%lu pending=%d dirty=%d mapped=%d",
              mTickCount, mFdCount, mEventCount, mRedrawCount, mDisplay ? XPending(mDisplay) : -1,
              mDirty ? 1 : 0, mMapped ? 1 : 0);
        traceWindowState("tick");
    }

    ensureMapped();
    onTick();
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void X11PlugView::mapWindow()
{
    if (!mDisplay || !mWindow)
        return;
    if (mMapped) {
        trace("mapWindow: skipped, we believe we are already mapped");
        return;
    }
    trace("mapWindow: mapping 0x%lx ourselves", mWindow);
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
    trace("ensureMapped: no embedder mapped us after %d ticks", mTicksUnmapped);
    mapWindow();
}

//------------------------------------------------------------------------
void X11PlugView::resizeSurfaces(unsigned width, unsigned height)
{
    if (!mDisplay || !mWindow || width == 0 || height == 0 ||
        (width == mSurfaceWidth && height == mSurfaceHeight))
        return;

    // The host has already resized the embedded X window before onSize() or
    // ConfigureNotify arrives. Sending another XResizeWindow here feeds a
    // second resize back into the host while it is dragging the frame and can
    // leave stale/interleaved ConfigureNotify events behind.
    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, static_cast<int>(width), static_cast<int>(height));
    if (mBuffer)
        cairo_surface_destroy(mBuffer);
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width),
                                         static_cast<int>(height));
    if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "NAMix: cannot resize the editor drawing buffer\n");
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
        return;
    }
    mSurfaceWidth = width;
    mSurfaceHeight = height;
    onResize(static_cast<int>(width), static_cast<int>(height));
    mDirty = true;
}

//------------------------------------------------------------------------
void X11PlugView::redraw()
{
    if (!mBuffer || !mTarget || !mDisplay)
        return;
    // A repaint after a long quiet spell is the interesting one: it is what
    // "the editor came back" looks like in the log.
    if (mTicksSinceRedraw >= kQuietRedrawTicks)
        trace("redraw after %lu quiet ticks", mTicksSinceRedraw);
    ++mRedrawCount;
    mTicksSinceRedraw = 0;
    mDirty = false;

    // Compose into the offscreen buffer...
    cairo_t *cr = cairo_create(mBuffer);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        onDraw(cr);
    }
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
