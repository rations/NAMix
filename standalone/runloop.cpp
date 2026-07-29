// RunLoop implementation. See runloop.h.

#include "runloop.h"

#include <sys/select.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace Steinberg;

namespace NAMix
{

//------------------------------------------------------------------------
RunLoop::RunLoop(::Display *display) : mDisplay(display)
{
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) {
        *obj = static_cast<Linux::IRunLoop *>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        addRef();
        return kResultOk;
    }

    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
// The editor is fixed-size, so a resize request should never arrive. Accepting
// it silently would leave the window and the view disagreeing about the size.
tresult PLUGIN_API RunLoop::resizeView(IPlugView * /*view*/, ViewRect * /*newSize*/)
{
    return kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::registerEventHandler(Linux::IEventHandler *handler,
                                                 Linux::FileDescriptor fd)
{
    if (!handler || fd < 0)
        return kInvalidArgument;
    mEventHandlers.push_back({handler, fd});
    return kResultTrue;
}

tresult PLUGIN_API RunLoop::unregisterEventHandler(Linux::IEventHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mEventHandlers.size();
    mEventHandlers.erase(
        std::remove_if(mEventHandlers.begin(), mEventHandlers.end(),
                       [handler](const EventEntry &e) { return e.handler == handler; }),
        mEventHandlers.end());
    return mEventHandlers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval ms)
{
    if (!handler || ms == 0)
        return kInvalidArgument;
    const std::chrono::milliseconds interval(ms);
    mTimers.push_back({handler, interval, Clock::now() + interval});
    return kResultTrue;
}

tresult PLUGIN_API RunLoop::unregisterTimer(Linux::ITimerHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mTimers.size();
    mTimers.erase(std::remove_if(mTimers.begin(), mTimers.end(),
                                 [handler](const TimerEntry &t) { return t.handler == handler; }),
                  mTimers.end());
    return mTimers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
int RunLoop::fireDueTimersAndGetTimeout()
{
    if (mTimers.empty())
        return -1;

    const Clock::time_point now = Clock::now();

    // Fire from a snapshot: a handler may register or unregister timers, which
    // would otherwise invalidate the iteration.
    std::vector<Linux::ITimerHandler *> due;
    for (TimerEntry &t : mTimers) {
        if (t.next <= now) {
            due.push_back(t.handler);
            // Skip missed firings rather than trying to catch up in a burst.
            t.next = now + t.interval;
        }
    }
    for (Linux::ITimerHandler *handler : due) {
        // The handler may have been unregistered by an earlier callback.
        const bool live =
            std::any_of(mTimers.begin(), mTimers.end(),
                        [handler](const TimerEntry &t) { return t.handler == handler; });
        if (live)
            handler->onTimer();
    }

    if (mTimers.empty())
        return -1;

    Clock::time_point soonest = mTimers.front().next;
    for (const TimerEntry &t : mTimers)
        soonest = std::min(soonest, t.next);

    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(soonest - Clock::now()).count();
    return delta < 0 ? 0 : static_cast<int>(delta);
}

//------------------------------------------------------------------------
void RunLoop::run()
{
    mRunning = true;
    const int xFd = mDisplay ? ConnectionNumber(mDisplay) : -1;

    while (mRunning) {
        // Anything already queued on our own connection is handled first:
        // select() would not report the fd as readable for events Xlib has
        // already buffered.
        if (mDisplay) {
            while (XPending(mDisplay)) {
                XEvent event;
                XNextEvent(mDisplay, &event);
                if (mXCallback)
                    mXCallback(event);
                if (!mRunning)
                    return;
            }
            XFlush(mDisplay);
        }

        const int timeoutMs = fireDueTimersAndGetTimeout();
        if (!mRunning)
            return;

        fd_set readSet;
        FD_ZERO(&readSet);
        int maxFd = -1;
        if (xFd >= 0) {
            FD_SET(xFd, &readSet);
            maxFd = xFd;
        }
        const std::vector<EventEntry> handlers = mEventHandlers; // snapshot
        for (const EventEntry &e : handlers) {
            FD_SET(e.fd, &readSet);
            maxFd = std::max(maxFd, e.fd);
        }
        if (maxFd < 0)
            break; // nothing left to wait on

        timeval tv;
        timeval *tvp = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvp = &tv;
        }

        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, tvp);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue; // timeout: loop round and fire the timers

        for (const EventEntry &e : handlers) {
            if (!FD_ISSET(e.fd, &readSet))
                continue;
            // Still registered? A previous callback may have removed it.
            const bool live =
                std::any_of(mEventHandlers.begin(), mEventHandlers.end(),
                            [&e](const EventEntry &c) { return c.handler == e.handler; });
            if (live)
                e.handler->onFDIsSet(e.fd);
            if (!mRunning)
                return;
        }
    }
}

} // namespace NAMix
