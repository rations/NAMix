// RunLoop — the host side of Linux::IRunLoop, for the NAMix standalone.
//
// On Linux a VST3 plug-in owns no event loop; the host must provide one, and
// hand it to the plug-in through IPlugFrame::queryInterface. That is exactly
// what the plug-in's editor asks for, so the standalone has to implement it to
// show its own editor at all.
//
// This object is therefore both:
//   * Linux::IRunLoop  — the plug-in registers file descriptors and timers;
//   * IPlugFrame       — what the host passes to IPlugView::setFrame, and the
//                        object the plug-in queries the run loop from.
//
// The loop itself is a select() over the host's X connection plus every
// descriptor the plug-in registered, with the timeout set from the nearest due
// timer. Registrations are allowed to change while a callback is running (a
// plug-in unregisters its handler from inside removed()), so the iteration
// works on copies.
//
// Reference: the SDK's own editorhost sample implements the same interfaces at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace NAMix
{

//------------------------------------------------------------------------
class RunLoop : public Steinberg::IPlugFrame, public Steinberg::Linux::IRunLoop
{
public:
    using XEventCallback = std::function<void(const XEvent &)>;

    explicit RunLoop(::Display *display);

    // Called for every X event on the host's own connection.
    void setXEventCallback(XEventCallback cb)
    {
        mXCallback = std::move(cb);
    }

    void run();
    void stop()
    {
        mRunning = false;
    }

    //---from IPlugFrame--------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) SMTG_OVERRIDE;

    //---from Linux::IRunLoop---------
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                                       Steinberg::Linux::FileDescriptor fd)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                                Steinberg::Linux::TimerInterval ms) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler)
        SMTG_OVERRIDE;

    //---from FUnknown----------------
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct EventEntry {
        Steinberg::Linux::IEventHandler *handler;
        int fd;
    };
    struct TimerEntry {
        Steinberg::Linux::ITimerHandler *handler;
        std::chrono::milliseconds interval;
        Clock::time_point next;
    };

    // Milliseconds until the soonest timer is due (0 if one is already due,
    // -1 if there are no timers at all).
    int fireDueTimersAndGetTimeout();

    ::Display *mDisplay = nullptr;
    XEventCallback mXCallback;
    std::vector<EventEntry> mEventHandlers;
    std::vector<TimerEntry> mTimers;
    bool mRunning = false;
};

} // namespace NAMix
