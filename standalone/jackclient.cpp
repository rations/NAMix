// JackClient implementation. See jackclient.h.

#include "jackclient.h"

#include "namids.h"

#include "pluginterfaces/vst/ivstcomponent.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace Steinberg;

namespace NAMix
{

//------------------------------------------------------------------------
JackClient::~JackClient()
{
    close();
}

//------------------------------------------------------------------------
bool JackClient::open(const char *clientName, Vst::IAudioProcessor *processor,
                      Vst::IComponent *component)
{
    if (!processor || !component)
        return false;
    mProcessor = processor;
    mComponent = component;

    jack_status_t status = static_cast<jack_status_t>(0);
    mClient = jack_client_open(clientName, JackNoStartServer, &status);
    if (!mClient) {
        fprintf(stderr, "NAMix: cannot connect to JACK (is jackd running?)\n");
        return false;
    }

    mSampleRate = static_cast<double>(jack_get_sample_rate(mClient));
    mBlockSize.store(static_cast<int>(jack_get_buffer_size(mClient)), std::memory_order_relaxed);

    mInPort = jack_port_register(mClient, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    mOutPorts[0] =
        jack_port_register(mClient, "out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    mOutPorts[1] =
        jack_port_register(mClient, "out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!mInPort || !mOutPorts[0] || !mOutPorts[1]) {
        fprintf(stderr, "NAMix: cannot register JACK ports\n");
        close();
        return false;
    }

    // All RT-side allocation happens here, before the process callback can
    // run: the bus and channel-pointer arrays, and parameter queues sized for
    // every parameter the plug-in exposes.
    //
    // bufferSamples is deliberately 0. That is what tells HostProcessData it
    // does NOT own the sample buffers, because process() below points the
    // buses straight at JACK's own memory each block. Passing mBlockSize here
    // instead makes it allocate a buffer per channel and set channelBufferOwner
    // — and then unprepare() runs delete[] on whatever the pointers hold at
    // close, which by then is JACK's memory, while the buffers it really
    // allocated leak.
    if (!mProcessData.prepare(*component, 0, Vst::kSample32)) {
        fprintf(stderr, "NAMix: cannot prepare the process buffers\n");
        close();
        return false;
    }

    mProcessData.numSamples = mBlockSize.load(std::memory_order_relaxed);
    mProcessData.symbolicSampleSize = Vst::kSample32;
    mProcessData.inputParameterChanges = &mInputChanges;
    mProcessData.outputParameterChanges = &mOutputChanges;

    if (jack_set_process_callback(mClient, processTrampoline, this) != 0) {
        fprintf(stderr, "NAMix: cannot install the JACK process callback\n");
        close();
        return false;
    }
    // Must be registered before jack_activate() (jack.h says so explicitly).
    if (jack_set_buffer_size_callback(mClient, bufferSizeTrampoline, this) != 0)
        fprintf(stderr, "NAMix: cannot install the JACK buffer-size callback - the processor "
                        "will stay set up for the size it started with\n");

    if (jack_activate(mClient) != 0) {
        fprintf(stderr, "NAMix: cannot activate the JACK client\n");
        close();
        return false;
    }

    // Auto-connect to the system's physical ports, so the standalone makes
    // sound out of the box. Failures here are not fatal: the user can wire it
    // up in a patchbay instead.
    if (const char **ins = jack_get_ports(mClient, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsPhysical | JackPortIsOutput)) {
        if (ins[0])
            jack_connect(mClient, ins[0], jack_port_name(mInPort));
        jack_free(ins);
    }
    if (const char **outs = jack_get_ports(mClient, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsPhysical | JackPortIsInput)) {
        for (int i = 0; i < 2 && outs[i]; ++i)
            jack_connect(mClient, jack_port_name(mOutPorts[i]), outs[i]);
        jack_free(outs);
    }

    printf("NAMix: JACK connected at %.0f Hz, %d frames\n", mSampleRate,
           mBlockSize.load(std::memory_order_relaxed));
    return true;
}

//------------------------------------------------------------------------
void JackClient::close()
{
    if (mClient) {
        jack_deactivate(mClient);
        jack_client_close(mClient);
        mClient = nullptr;
    }
    mInPort = nullptr;
    mOutPorts[0] = mOutPorts[1] = nullptr;
    mProcessData.unprepare();
    mProcessor = nullptr;
    mComponent = nullptr;
}

//------------------------------------------------------------------------
// UI thread (producer).
bool JackClient::pushParameter(Vst::ParamID id, Vst::ParamValue value)
{
    const uint32_t write = mRingWrite.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % kRingSize;
    if (next == mRingRead.load(std::memory_order_acquire))
        return false; // full; drop rather than block either thread
    mRing[write] = {id, value};
    mRingWrite.store(next, std::memory_order_release);
    return true;
}

//------------------------------------------------------------------------
// RT thread (consumer). addParameterData/addPoint only reuse the queues and
// points reserved during prepare(), so this does not allocate.
void JackClient::drainParameterRing()
{
    uint32_t read = mRingRead.load(std::memory_order_relaxed);
    const uint32_t write = mRingWrite.load(std::memory_order_acquire);
    while (read != write) {
        const Change &change = mRing[read];
        int32 index = 0;
        if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(change.id, index)) {
            int32 pointIndex = 0;
            queue->addPoint(0, change.value, pointIndex);
        }
        read = (read + 1) % kRingSize;
    }
    mRingRead.store(read, std::memory_order_release);
}

//------------------------------------------------------------------------
// RT thread. Only stores into atomics — the controller is a UI-thread object
// and must never be called from here.
void JackClient::publishMeters()
{
    const int32 count = mOutputChanges.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::IParamValueQueue *queue = mOutputChanges.getParameterData(i);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points < 1)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;
        switch (queue->getParameterId()) {
            case kInputMeterId:
                mInputMeter.store(value, std::memory_order_relaxed);
                break;
            case kOutputMeterId:
                mOutputMeter.store(value, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
int JackClient::processTrampoline(jack_nframes_t nframes, void *arg)
{
    return static_cast<JackClient *>(arg)->process(nframes);
}

//------------------------------------------------------------------------
// JACK's notification thread, with the process cycle suspended. It would be
// legal to do the whole reconfiguration here, but setupProcessing and
// setActive are VST3 main-thread calls and the plug-in's message thread may be
// in the middle of a load, so all this does is record the new size for the run
// loop to act on.
int JackClient::bufferSizeTrampoline(jack_nframes_t nframes, void *arg)
{
    static_cast<JackClient *>(arg)->mNewBlockSize.store(static_cast<int>(nframes),
                                                        std::memory_order_release);
    return 0;
}

//------------------------------------------------------------------------
int JackClient::takeBufferSizeChange()
{
    const int size = mNewBlockSize.exchange(0, std::memory_order_acquire);
    // JACK announces the size once at activation too; only a real move counts.
    if (size <= 0 || size == mBlockSize.load(std::memory_order_relaxed))
        return 0;
    return size;
}

//------------------------------------------------------------------------
bool JackClient::suspendProcessing()
{
    if (!mClient)
        return true; // no audio thread to race with

    mSuspended.store(true, std::memory_order_release);

    // Two cycles, not one: the first may already have been inside process()
    // when the flag went up, so only the second is guaranteed to have seen it.
    const uint32_t start = mCycle.load(std::memory_order_acquire);
    for (int attempt = 0; attempt < 200; ++attempt) { // ~2 s
        if (mCycle.load(std::memory_order_acquire) - start >= 2)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The audio thread is not running (a stopped server, or freewheeling).
    // Say so and leave the processor alone rather than reconfiguring it
    // underneath a callback that might still come back.
    mSuspended.store(false, std::memory_order_release);
    return false;
}

//------------------------------------------------------------------------
void JackClient::resumeProcessing(int blockSize)
{
    if (blockSize > 0)
        mBlockSize.store(blockSize, std::memory_order_relaxed);
    mSuspended.store(false, std::memory_order_release);
}

//------------------------------------------------------------------------
// JACK real-time thread. Nothing here allocates, locks, or logs.
int JackClient::process(jack_nframes_t nframes)
{
    float *outL = static_cast<float *>(jack_port_get_buffer(mOutPorts[0], nframes));
    float *outR = static_cast<float *>(jack_port_get_buffer(mOutPorts[1], nframes));
    if (!outL || !outR)
        return 0;

    // Suspended: the UI thread is reconfiguring the processor and must not be
    // raced. Silence is the honest output — a buffer-size change already puts
    // a gap in the audio flow, and stale samples would be worse.
    if (!mProcessor || mSuspended.load(std::memory_order_acquire)) {
        memset(outL, 0, nframes * sizeof(float));
        memset(outR, 0, nframes * sizeof(float));
        mCycle.fetch_add(1, std::memory_order_release);
        return 0;
    }

    float *in = static_cast<float *>(jack_port_get_buffer(mInPort, nframes));
    if (!in) {
        mCycle.fetch_add(1, std::memory_order_release);
        return 0;
    }

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    drainParameterRing();

    // Loop, never clamp. JACK's buffer size can change under a running client,
    // and the processor was set up for mBlockSize: handing it more would break
    // that contract, and truncating to mBlockSize would leave the rest of the
    // block holding whatever JACK's buffer had in it from the previous cycle,
    // which is stale audio rather than a dropout.
    const jack_nframes_t chunk =
        static_cast<jack_nframes_t>(mBlockSize.load(std::memory_order_relaxed));
    jack_nframes_t done = 0;
    while (done < nframes) {
        const int32 n = static_cast<int32>(std::min<jack_nframes_t>(chunk, nframes - done));

        // Point the VST3 bus buffers straight at JACK's, so no copy is needed.
        if (mProcessData.inputs && mProcessData.inputs[0].numChannels > 0)
            mProcessData.inputs[0].channelBuffers32[0] = in + done;
        if (mProcessData.outputs && mProcessData.outputs[0].numChannels > 1) {
            mProcessData.outputs[0].channelBuffers32[0] = outL + done;
            mProcessData.outputs[0].channelBuffers32[1] = outR + done;
        }
        mProcessData.numSamples = n;

        mProcessor->process(mProcessData);

        publishMeters();
        // The queued edits belong to the top of the JACK block, not to every
        // chunk of it; replaying them would re-apply the same point n times.
        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        done += static_cast<jack_nframes_t>(n);
    }

    // Every exit from this callback bumps the cycle counter, including the
    // early ones: suspendProcessing() waits on it, and a path that skipped it
    // would look like an audio thread that had stopped responding.
    mCycle.fetch_add(1, std::memory_order_release);
    return 0;
}

} // namespace NAMix
