// namix-standalone — run the NAMix VST3 without a DAW.
//
// This is a deliberately small host: one top-level X window, the plug-in's own
// editor embedded inside it, a run loop the plug-in can register with, and a
// JACK client feeding the processor. It exists so NAMix is usable on its own,
// not as a general-purpose host.
//
// The plug-in is loaded as a bundle through the SDK's module loader rather
// than linked in. That matters: the editor locates its art and fonts with
// dladdr() relative to its own .so, so it must genuinely be a loaded module
// for the resource paths to resolve the same way they do inside a DAW.
//
// Threading: everything except the JACK callback runs on this thread. The
// editor, the controller and the run loop are all single-threaded here, which
// is the same contract a DAW provides.

#include "jackclient.h"
#include "runloop.h"

#include "namids.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace Steinberg;

namespace
{

constexpr int kWinW = 600;
constexpr int kWinH = 400;

NAMix::RunLoop *gRunLoop = nullptr;

void onSignal(int)
{
    if (gRunLoop)
        gRunLoop->stop();
}

//------------------------------------------------------------------------
// The host end of the VST3 edit loop. The editor calls performEdit() when the
// user moves a control; we forward the value to the processor through the
// JACK client's lock-free ring.
class ComponentHandler : public Vst::IComponentHandler
{
public:
    ComponentHandler(NAMix::JackClient &jack, Vst::IEditController *controller)
        : mJack(jack), mController(controller)
    {
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE
    {
        mJack.pushParameter(id, value);
        return kResultOk;
    }

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
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NAMix::JackClient &mJack;
    Vst::IEditController *mController;
};

//------------------------------------------------------------------------
// Feeds the meter values the audio thread published back into the controller,
// which is what makes the editor's meters move. Runs as a run-loop timer on
// the UI thread, so no IEditController call ever happens on the RT thread.
class MeterPump : public Linux::ITimerHandler
{
public:
    MeterPump(NAMix::JackClient &jack, Vst::IEditController *controller)
        : mJack(jack), mController(controller)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        if (!mController)
            return;
        mController->setParamNormalized(NAMix::kInputMeterId, mJack.inputMeter());
        mController->setParamNormalized(NAMix::kOutputMeterId, mJack.outputMeter());
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
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
    NAMix::JackClient &mJack;
    Vst::IEditController *mController;
};

} // namespace

//------------------------------------------------------------------------
// Where to look for NAMix.vst3 when no path is given on the command line.
// The standalone is a host: it loads the same bundle a DAW would, rather than
// linking the plug-in in, so that dladdr-based resource lookup behaves
// identically in both. That means it has to be able to FIND the bundle --
// covering the release tarball (bundle beside the binary), the build tree, and
// a system or per-user VST3 install.
static std::string findBundle()
{
    std::vector<std::string> candidates;

    if (const char *env = getenv("NAMIX_VST3"))
        candidates.emplace_back(env);

    char exe[4096] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        std::string dir(exe, static_cast<size_t>(n));
        const size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos)
            dir.resize(slash);
        candidates.push_back(dir + "/NAMix.vst3");              // release tarball
        candidates.push_back(dir + "/VST3/Release/NAMix.vst3"); // build tree
    }

    if (const char *home = getenv("HOME"))
        candidates.push_back(std::string(home) + "/.vst3/NAMix.vst3");
    candidates.push_back("/usr/local/lib/vst3/NAMix.vst3");
    candidates.push_back("/usr/lib/vst3/NAMix.vst3");

    std::error_code ec;
    for (const auto &path : candidates)
        if (std::filesystem::is_directory(path, ec))
            return path;

    return candidates.empty() ? std::string() : candidates.front();
}

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const std::string modulePath = argc > 1 ? std::string(argv[1]) : findBundle();

    // The host context must be published BEFORE the plug-in is instantiated:
    // ComponentBase::allocateMessage() asks it for IMessage instances, so
    // without one every controller->processor message (model path, IR path,
    // Slim) is silently dropped and only parameter changes get through. That
    // presents as a plug-in whose knobs work but which never loads a model.
    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string error;
    auto module = VST3::Hosting::Module::create(modulePath, error);
    if (!module) {
        fprintf(stderr, "namix-standalone: cannot load %s\n  %s\n", modulePath.c_str(),
                error.c_str());
        fprintf(stderr,
                "usage: %s [path to NAMix.vst3]\n"
                "  Searched, in order: $NAMIX_VST3, alongside this binary,\n"
                "  the build tree, ~/.vst3, /usr/local/lib/vst3, /usr/lib/vst3.\n",
                argv[0]);
        return 1;
    }

    // --- instantiate the plug-in -------------------------------------
    auto factory = module->getFactory();
    IPtr<Vst::PlugProvider> provider;
    for (auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        provider = owned(new Vst::PlugProvider(factory, classInfo, true));
        if (provider->initialize())
            break;
        provider = nullptr;
    }
    if (!provider) {
        fprintf(stderr, "namix-standalone: no audio effect class in %s\n", modulePath.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    Vst::IEditController *controller = provider->getController();
    if (!component || !controller) {
        fprintf(stderr, "namix-standalone: the plug-in did not provide both parts\n");
        return 1;
    }

    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!processor) {
        fprintf(stderr, "namix-standalone: the plug-in has no IAudioProcessor\n");
        return 1;
    }

    // --- audio -------------------------------------------------------
    NAMix::JackClient jack;

    // A first connection just to learn the server's rate and block size, so
    // setupProcessing can be told the truth before the component is activated.
    jack_status_t status = static_cast<jack_status_t>(0);
    jack_client_t *probe = jack_client_open("NAMix-probe", JackNoStartServer, &status);
    double sampleRate = 48000.0;
    int blockSize = 1024;
    if (probe) {
        sampleRate = static_cast<double>(jack_get_sample_rate(probe));
        blockSize = static_cast<int>(jack_get_buffer_size(probe));
        jack_client_close(probe);
    } else {
        fprintf(stderr, "namix-standalone: no JACK server; "
                        "continuing with the editor only\n");
    }

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "namix-standalone: the plug-in rejected the process setup\n");
        return 1;
    }

    component->setActive(true);
    processor->setProcessing(true);

    ComponentHandler handler(jack, controller);
    controller->setComponentHandler(&handler);

    if (probe && !jack.open("NAMix", processor, component))
        fprintf(stderr, "namix-standalone: continuing without audio\n");

    // --- window and editor -------------------------------------------
    ::Display *display = XOpenDisplay(nullptr);
    if (!display) {
        fprintf(stderr, "namix-standalone: cannot open the X display\n");
        return 1;
    }

    const int screen = DefaultScreen(display);
    ::Window window =
        XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, kWinW, kWinH, 0,
                            BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "NAMix");
    XSelectInput(display, window, StructureNotifyMask | SubstructureNotifyMask);

    // Fixed size: the editor does not resize, so tell the window manager.
    XSizeHints hints = {};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = kWinW;
    hints.min_height = hints.max_height = kWinH;
    XSetWMNormalHints(display, window, &hints);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDelete, 1);
    XMapWindow(display, window);
    XFlush(display);

    NAMix::RunLoop runLoop(display);
    gRunLoop = &runLoop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    IPtr<IPlugView> view = owned(controller->createView(Vst::ViewType::kEditor));
    if (view && view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue) {
        view->setFrame(&runLoop);
        if (view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(window)),
                           kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            fprintf(stderr, "namix-standalone: the editor refused to attach\n");
            view = nullptr;
        }
    } else {
        fprintf(stderr, "namix-standalone: the plug-in has no X11 editor\n");
        view = nullptr;
    }

    MeterPump meters(jack, controller);
    runLoop.registerTimer(&meters, 33);

    runLoop.setXEventCallback([&](const XEvent &event) {
        if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wmDelete)
            runLoop.stop();
    });

    runLoop.run();

    // --- teardown ----------------------------------------------------
    runLoop.unregisterTimer(&meters);
    if (view) {
        view->removed();
        view = nullptr;
    }
    jack.close();
    processor->setProcessing(false);
    component->setActive(false);
    controller->setComponentHandler(nullptr);

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    gRunLoop = nullptr;
    // Retract the host context before it leaves scope, so nothing can reach a
    // dangling pointer during static destruction.
    Vst::PluginContextFactory::instance().setPluginContext(nullptr);
    return 0;
}
