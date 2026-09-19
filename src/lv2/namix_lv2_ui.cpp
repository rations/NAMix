// SPDX-License-Identifier: MIT
//
// NAMix_ui.so — the LV2 ui:X11UI half.
//
// Like its DSP sibling this is a HOST rather than a second editor: it instantiates the same
// NamController and the same NamEditorView a VST3 host gets, and supplies the three host objects
// the pair expects — an IComponentHandler, an IPlugFrame, and the Linux::IRunLoop that IPlugFrame
// carries. Not one line of the 900-line panel changes, and neither does the X11 window class
// beneath it.
//
// THE RUN LOOP IS THE WHOLE TRICK, and it is why there is no Lv2X11Ui class here.
//
// X11PlugView already implements everything an LV2 X11 UI needs and is the code proved in real
// hosts: it opens its own Display, inherits the parent's visual, depth and colormap (the BadMatch
// that makes a REAPER pane come up blank), installs a non-fatal chaining X error handler, does the
// XEmbed mapping handshake, double buffers, and follows a host that resizes the parent without
// routing through onSize(). What it does NOT have is a clock: on Linux a VST3 plug-in owns no
// thread and borrows the host's Linux::IRunLoop for its X file descriptor and its 33 ms tick.
//
// LV2 supplies that clock under a different name. The host calls LV2UI_Idle_Interface::idle() at
// least thirty times a second, and idle() is a run loop with one subscriber. So this file supplies
// the IRunLoop, and the SAME window class serves both formats. Porting a separate Lv2X11Ui instead
// would have meant a second copy of that window code, and a second place for the visual fix to be
// forgotten.
//
// WHAT AN LV2 HOST DOES WITH THE WINDOW, verified against suil rather than assumed: suil's
// x11_in_gtk3.c forward_size_request() reads the child's XSizeHints with XGetNormalHints, clamps
// its allocation to them, and XResizeWindows the child. So the hints are how a UI states its
// limits. This editor is a fixed size — X11PlugView does not override canResize — so the hints it
// sets are min == max, which is the strongest thing it can say.

#include "lv2message.h"
#include "namixlv2.h"

#include "namcontroller.h"
#include "namview.h"
#include "platform/respath.h"

#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/gui/iplugview.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

using namespace Steinberg;
using namespace NAMix;
using namespace NAMix::lv2;

namespace
{

// The editor's own tick, matching the 33 ms X11PlugView asks its VST3 host for. idle() is only
// promised at 30 Hz, so a tick is taken whenever one is due rather than counted.
constexpr auto kTickInterval = std::chrono::milliseconds(33);

//------------------------------------------------------------------------
class NamixUi;

// The host end of the edit loop. The editor calls performEdit when the user moves a control, and
// the value goes out to the plug-in as an ordinary control port write — which is what makes the
// host's own automation see it, exactly as IComponentHandler does under VST3.
//
// beginEdit / endEdit have an LV2 analogue in ui:touch, which is declared nowhere here: it is an
// optional feature whose only effect is to tell a host that an automation gesture is in progress,
// and the plug-in has no behaviour riding on it. Left out deliberately rather than forgotten.
class Lv2ComponentHandler : public Vst::IComponentHandler
{
public:
    explicit Lv2ComponentHandler(NamixUi &owner) : mOwner(owner)
    {
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE;
    tresult PLUGIN_API endEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Non-deleting: a member of the UI, held by the controller in a raw pointer, and outliving
    // everything that touches it.
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NamixUi &mOwner;
};

//------------------------------------------------------------------------
// The processor's peer, seen from the controller's side. Every controller -> processor message —
// the model load, the IR load and Slim — arrives here and goes out as one atom on the plug-in's
// control port.
class ProcessorPeer : public Vst::IConnectionPoint
{
public:
    explicit ProcessorPeer(NamixUi &owner) : mOwner(owner)
    {
    }

    tresult PLUGIN_API connect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage *message) SMTG_OVERRIDE;

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IConnectionPoint *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NamixUi &mOwner;
};

//------------------------------------------------------------------------
// IPlugFrame, and the Linux::IRunLoop it carries.
//
// The editor asks IPlugFrame for IRunLoop and then registers one event handler on its X
// connection's file descriptor and one timer. Both registrations land here, and idle() is what
// services them: the file descriptor is offered on every idle because X11PlugView::onFDIsSet does
// nothing but drain whatever XPending reports, so calling it with an empty queue costs one round
// trip and cannot misbehave; the timer fires when its interval has elapsed.
class Lv2Frame : public IPlugFrame, public Linux::IRunLoop
{
public:
    explicit Lv2Frame(NamixUi &owner) : mOwner(owner)
    {
    }

    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) SMTG_OVERRIDE;

    //---from Linux::IRunLoop---------
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *handler,
                                            Linux::FileDescriptor fd) SMTG_OVERRIDE
    {
        if (!handler)
            return kInvalidArgument;
        mEventHandler = handler;
        mFd = fd;
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *handler) SMTG_OVERRIDE
    {
        if (handler == mEventHandler)
            mEventHandler = nullptr;
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler *handler,
                                     Linux::TimerInterval milliseconds) SMTG_OVERRIDE
    {
        if (!handler)
            return kInvalidArgument;
        mTimerHandler = handler;
        mTimerInterval = std::chrono::milliseconds(static_cast<long long>(milliseconds));
        mNextTick = std::chrono::steady_clock::now();
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *handler) SMTG_OVERRIDE
    {
        if (handler == mTimerHandler)
            mTimerHandler = nullptr;
        return kResultTrue;
    }

    // One turn of the loop, driven by the host's idle().
    void pump()
    {
        if (mEventHandler)
            mEventHandler->onFDIsSet(mFd);
        if (!mTimerHandler)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < mNextTick)
            return;
        // Advance from NOW rather than by adding the interval to the last deadline: a host that
        // stalled for a second would otherwise owe us thirty ticks and fire them back to back.
        mNextTick = now + mTimerInterval;
        mTimerHandler->onTimer();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame *>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) {
            *obj = static_cast<Linux::IRunLoop *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NamixUi &mOwner;
    Linux::IEventHandler *mEventHandler = nullptr;
    Linux::FileDescriptor mFd = -1;
    Linux::ITimerHandler *mTimerHandler = nullptr;
    std::chrono::milliseconds mTimerInterval = kTickInterval;
    std::chrono::steady_clock::time_point mNextTick;
};

//------------------------------------------------------------------------
class NamixUi
{
public:
    NamixUi(LV2UI_Write_Function write, LV2UI_Controller controller)
        : mHostApp("NAMix LV2 UI"), mHandler(*this), mPeer(*this), mFrame(*this), mWrite(write),
          mWriteController(controller)
    {
    }

    bool open(const char *bundlePath, ::Window parent, const LV2UI_Resize *resize,
              LV2_URID_Map *map);
    void close();
    int idle();

    ::Window widget() const
    {
        return mView ? mView->nativeWindow() : 0;
    }

    void portEvent(std::uint32_t port, std::uint32_t size, std::uint32_t format,
                   const void *buffer);

    // Lv2ComponentHandler / ProcessorPeer / Lv2Frame call these.
    void writeParameter(Vst::ParamID id, double normalized);
    void sendToProcessor(Vst::IMessage *message);
    bool resizeTo(int width, int height);

private:
    void applySizeHints();

    HostApp mHostApp;
    Lv2ComponentHandler mHandler;
    ProcessorPeer mPeer;
    Lv2Frame mFrame;

    LV2UI_Write_Function mWrite = nullptr;
    LV2UI_Controller mWriteController = nullptr;
    LV2_URID_Map *mMap = nullptr;
    MessageUris mUris;
    const LV2UI_Resize *mResize = nullptr;

    IPtr<NamController> mController;
    IPtr<NamEditorView> mView;

    // A connection of our own, used for nothing but the size hints on the editor's window. The
    // editor owns its own Display and does not share it, and X window ids are server-global, so a
    // second connection can set a property on a window the first one made.
    ::Display *mHintDisplay = nullptr;

    // Forge scratch for one outgoing message. The UI thread is the only writer.
    std::vector<std::uint8_t> mScratch;
    LV2_Atom_Forge mForge = {};
};

//------------------------------------------------------------------------
tresult PLUGIN_API Lv2ComponentHandler::performEdit(Vst::ParamID id, Vst::ParamValue value)
{
    mOwner.writeParameter(id, value);
    return kResultOk;
}

tresult PLUGIN_API ProcessorPeer::notify(Vst::IMessage *message)
{
    mOwner.sendToProcessor(message);
    return kResultOk;
}

tresult PLUGIN_API Lv2Frame::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize)
        return kInvalidArgument;
    if (!mOwner.resizeTo(newSize->getWidth(), newSize->getHeight()))
        return kResultFalse;
    // The SDK's own sequence: the host resizes, then tells the view, in this call.
    view->onSize(newSize);
    return kResultTrue;
}

//------------------------------------------------------------------------
bool NamixUi::open(const char *bundlePath, ::Window parent, const LV2UI_Resize *resize,
                   LV2_URID_Map *map)
{
    mMap = map;
    mResize = resize;
    mUris.map(map);
    if (!mUris.valid()) {
        fprintf(stderr, "NAMix: the host provides no usable urid:map for the editor\n");
        return false;
    }
    mScratch.resize(64 * 1024);
    lv2_atom_forge_init(&mForge, map);

    // An LV2 bundle keeps its art beside its binaries and hands the directory over here, so the
    // VST3 bundle layout respath.cpp otherwise derives does not apply. Set before anything can
    // load a resource, which is why it is the first thing in this function that does any work.
    if (bundlePath && *bundlePath)
        setResourceDirOverride(bundlePath);

    mController = owned(new (std::nothrow) NamController());
    if (!mController)
        return false;
    if (mController->initialize(&mHostApp) != kResultOk) {
        fprintf(stderr, "NAMix: the controller refused to initialise\n");
        return false;
    }
    mController->connect(&mPeer);
    mController->setComponentHandler(&mHandler);

    // Created directly rather than through createView(), because the LV2UI_Widget this UI hands
    // back is the editor's own X window id and createView's IPlugView* has no way to say what that
    // is. It is the same object createView would have made — see NamController::createView — and
    // it still registers itself with the controller through the attach hook.
    mView = owned(new (std::nothrow) NamEditorView(mController));
    if (!mView)
        return false;
    if (mView->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        fprintf(stderr, "NAMix: this build has no X11 editor\n");
        return false;
    }
    mView->setFrame(&mFrame);
    if (mView->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(parent)),
                        kPlatformTypeX11EmbedWindowID) != kResultOk) {
        fprintf(stderr, "NAMix: the editor could not embed in the host's window\n");
        return false;
    }

    mHintDisplay = XOpenDisplay(nullptr);
    if (!mHintDisplay) {
        // Not fatal. Without hints a host simply does not know the editor's size and may offer one
        // it cannot use well; the editor still draws at its own size either way.
        fprintf(stderr, "NAMix: cannot open a second X connection for the editor's size hints; "
                        "the host will not be told the editor's size\n");
    }
    applySizeHints();

    ViewRect size = {};
    mView->getSize(&size);
    if (resize && resize->ui_resize)
        resize->ui_resize(resize->handle, size.getWidth(), size.getHeight());

    // The panel draws the model and IR paths, and neither is a parameter: under VST3 they arrive
    // through setComponentState, which LV2 has no equivalent of. So ask.
    Message request;
    request.setMessageID(kMsgLv2RequestState);
    sendToProcessor(&request);
    return true;
}

//------------------------------------------------------------------------
void NamixUi::close()
{
    if (mView) {
        mView->removed();
        mView->setFrame(nullptr);
        mView = nullptr;
    }
    if (mController) {
        mController->setComponentHandler(nullptr);
        mController->disconnect(&mPeer);
        mController->terminate();
        mController = nullptr;
    }
    if (mHintDisplay) {
        XCloseDisplay(mHintDisplay);
        mHintDisplay = nullptr;
    }
}

//------------------------------------------------------------------------
// State the editor's size where a host will actually look for it: suil reads the child window's
// XSizeHints and clamps its allocation to them before resizing us (src/x11_in_gtk3.c,
// forward_size_request).
//
// This editor is a fixed size, so min == max == the editor's own rectangle. X11PlugView does not
// override canResize, which means CPluginView::canResize returns kResultFalse and the VST3 build
// tells its host exactly the same thing.
void NamixUi::applySizeHints()
{
    if (!mHintDisplay || !mView)
        return;
    const ::Window window = mView->nativeWindow();
    if (!window)
        return;

    ViewRect size = {};
    if (mView->getSize(&size) != kResultOk)
        return;

    XSizeHints hints = {};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = size.getWidth();
    hints.min_height = hints.max_height = size.getHeight();
    XSetWMNormalHints(mHintDisplay, window, &hints);
    XFlush(mHintDisplay);
}

//------------------------------------------------------------------------
bool NamixUi::resizeTo(int width, int height)
{
    // Hints BEFORE the request, so a host that clamps has the new limits to clamp against rather
    // than the outgoing ones.
    applySizeHints();
    if (mResize && mResize->ui_resize)
        mResize->ui_resize(mResize->handle, width, height);
    return true;
}

//------------------------------------------------------------------------
int NamixUi::idle()
{
    mFrame.pump();
    // Always alive: an embedded UI has no close affordance of its own, and a non-zero answer makes
    // a host stop calling idle() for good — which would freeze the editor rather than report an
    // error.
    return 0;
}

//------------------------------------------------------------------------
void NamixUi::writeParameter(Vst::ParamID id, double normalized)
{
    if (!mWrite)
        return;
    for (int i = 0; i < kControlInCount; ++i) {
        if (controlPortParam(i) != id)
            continue;
        const float plain = static_cast<float>(controlPlain(controlSpec(i), normalized));
        mWrite(mWriteController, kPortControlFirst + static_cast<std::uint32_t>(i), sizeof(float),
               0, &plain);
        return;
    }
    // Anything else is a parameter with no port: the read-only feedback block, which travels the
    // other way. Not something the editor edits, so there is nothing to report.
}

//------------------------------------------------------------------------
void NamixUi::sendToProcessor(Vst::IMessage *message)
{
    if (!mWrite || !message)
        return;
    auto *concrete = static_cast<Message *>(message);
    lv2_atom_forge_set_buffer(&mForge, mScratch.data(),
                              static_cast<std::uint32_t>(mScratch.size()));
    if (!forgeMessage(mForge, mUris, message, concrete->attributes())) {
        fprintf(stderr, "NAMix: an editor message did not fit and was dropped (%s)\n",
                concrete->id().c_str());
        return;
    }
    const auto *atom = reinterpret_cast<const LV2_Atom *>(mScratch.data());
    mWrite(mWriteController, kPortAtomIn, lv2_atom_total_size(atom), mUris.atomEventTransfer, atom);
}

//------------------------------------------------------------------------
void NamixUi::portEvent(std::uint32_t port, std::uint32_t size, std::uint32_t format,
                        const void *buffer)
{
    if (!buffer || !mController)
        return;

    // format 0 is the implicit ui:floatProtocol: a control port, a single float. Host data is
    // untrusted, so the size is checked before the payload is touched.
    if (format == 0) {
        if (size != sizeof(float))
            return;
        const double plain = static_cast<double>(*static_cast<const float *>(buffer));
        if (port >= kPortControlFirst && port < kPortFeedbackFirst) {
            const int index = static_cast<int>(port - kPortControlFirst);
            mController->setParamNormalized(controlPortParam(index),
                                            controlNorm(controlSpec(index), plain));
            return;
        }
        if (port >= kPortFeedbackFirst && port < kPortLatency) {
            // The two meters. These reach the UI only because the bundle asks for them with
            // ui:portNotification — the UI extension says a host calls port_event for control
            // INPUTS by default, and without those declarations this block would never run while
            // everything else worked. They are already normalized: the processor publishes them
            // as 0..1.
            const int index = static_cast<int>(port - kPortFeedbackFirst);
            mController->setParamNormalized(kFeedbackIds[index], plain);
        }
        return;
    }

    if (format != mUris.atomEventTransfer || port != kPortAtomOut)
        return;
    if (size < sizeof(LV2_Atom))
        return;
    const auto *atom = static_cast<const LV2_Atom *>(buffer);
    if (!lv2_atom_forge_is_object_type(&mForge, atom->type))
        return;
    Message *message = parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), mUris);
    if (!message)
        return;

    if (message->id() == kMsgLv2State) {
        // The processor's own state blob, read by the controller's own reader. This is the call a
        // VST3 host makes, arriving by the only route LV2 leaves open.
        const void *data = nullptr;
        uint32 blobSize = 0;
        if (message->getAttributes()->getBinary(kLv2StateAttr, data, blobSize) == kResultOk &&
            data && blobSize > 0) {
            MemoryStream stream(const_cast<void *>(data), static_cast<TSize>(blobSize));
            mController->setComponentState(&stream);
        }
    } else {
        mController->notify(message);
    }
    message->release();
}

//------------------------------------------------------------------------
// The LV2 UI entry points.
//------------------------------------------------------------------------
LV2UI_Handle uiInstantiate(const LV2UI_Descriptor *, const char *pluginUri, const char *bundlePath,
                           LV2UI_Write_Function write, LV2UI_Controller controller,
                           LV2UI_Widget *widget, const LV2_Feature *const *features)
{
    if (!widget || !pluginUri || std::strcmp(pluginUri, kPluginUri) != 0)
        return nullptr;
    *widget = nullptr;

    ::Window parent = 0;
    const LV2UI_Resize *resize = nullptr;
    LV2_URID_Map *map = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_UI__parent) == 0)
            parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(features[i]->data));
        else if (std::strcmp(features[i]->URI, LV2_UI__resize) == 0)
            resize = static_cast<const LV2UI_Resize *>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<LV2_URID_Map *>(features[i]->data);
    }
    if (!parent) {
        fprintf(stderr, "NAMix: the host supplied no ui:parent for an X11 UI\n");
        return nullptr;
    }
    if (!map) {
        fprintf(stderr, "NAMix: the host supplied no urid:map for the editor\n");
        return nullptr;
    }

    auto *ui = new (std::nothrow) NamixUi(write, controller);
    if (!ui)
        return nullptr;
    // Any failure returns NULL, so the host falls back to its own generic controls rather than
    // showing a dead rectangle.
    if (!ui->open(bundlePath, parent, resize, map) || !ui->widget()) {
        ui->close();
        delete ui;
        return nullptr;
    }

    *widget = reinterpret_cast<LV2UI_Widget>(static_cast<uintptr_t>(ui->widget()));
    return static_cast<LV2UI_Handle>(ui);
}

void uiCleanup(LV2UI_Handle handle)
{
    auto *ui = static_cast<NamixUi *>(handle);
    if (!ui)
        return;
    ui->close();
    delete ui;
}

void uiPortEvent(LV2UI_Handle handle, std::uint32_t port, std::uint32_t size, std::uint32_t format,
                 const void *buffer)
{
    if (auto *ui = static_cast<NamixUi *>(handle))
        ui->portEvent(port, size, format, buffer);
}

int uiIdle(LV2UI_Handle handle)
{
    auto *ui = static_cast<NamixUi *>(handle);
    return ui ? ui->idle() : 1;
}

const LV2UI_Idle_Interface kIdleInterface = {uiIdle};

const void *uiExtensionData(const char *uri)
{
    if (uri && std::strcmp(uri, LV2_UI__idleInterface) == 0)
        return &kIdleInterface;
    return nullptr;
}

const LV2UI_Descriptor kUiDescriptor = {
    kUiUri, uiInstantiate, uiCleanup, uiPortEvent, uiExtensionData,
};

} // namespace

extern "C" {

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &kUiDescriptor : nullptr;
}

} // extern "C"
