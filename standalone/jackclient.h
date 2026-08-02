// JackClient — mono-in / stereo-out JACK audio for the NAMix standalone.
//
// The process callback runs on JACK's real-time thread and obeys the same
// contract the plug-in's own process() does: no allocation, no locks, no
// logging, no file I/O. Every VST3 process structure is allocated once in
// open(); the sample buffers are JACK's own, which the bus pointers are aimed
// at each block rather than copied through.
//
// Two things have to cross the RT boundary, and neither may touch a lock or
// the edit controller from the audio thread:
//
//   UI -> RT   parameter edits made in the editor. They arrive on the UI
//              thread through the host's IComponentHandler and are pushed
//              into a single-producer/single-consumer ring, which the audio
//              thread drains into the VST3 input parameter queues at the top
//              of each block. Sharing a ParameterChanges between the two
//              threads instead would be a plain data race.
//
//   RT -> UI   meter values, which the plug-in publishes through
//              outputParameterChanges. The audio thread only stores them in
//              atomics; the UI thread reads those and calls the controller,
//              because IEditController must never be called from RT.

#pragma once

#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <jack/jack.h>

#include <atomic>
#include <cstdint>

namespace NAMix
{

//------------------------------------------------------------------------
class JackClient
{
public:
    ~JackClient();

    // Connects to a running JACK server and starts processing. `processor`
    // must already be set up and activated.
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component);
    void close();

    double sampleRate() const
    {
        return mSampleRate;
    }
    int blockSize() const
    {
        return mBlockSize;
    }

    // UI thread: queue a normalized parameter change for the next block.
    // Returns false if the ring is full (the change is then dropped, which is
    // preferable to blocking the UI or the audio thread).
    bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    // UI thread: the most recent meter values published by the plug-in.
    double inputMeter() const
    {
        return mInputMeter.load(std::memory_order_relaxed);
    }
    double outputMeter() const
    {
        return mOutputMeter.load(std::memory_order_relaxed);
    }

private:
    static int processTrampoline(jack_nframes_t nframes, void *arg);
    int process(jack_nframes_t nframes);
    void drainParameterRing(); // RT thread
    void publishMeters();      // RT thread

    // SPSC ring: written only by the UI thread, read only by the audio thread.
    static constexpr uint32_t kRingSize = 512; // power of two
    struct Change {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    Change mRing[kRingSize] = {};
    std::atomic<uint32_t> mRingWrite{0};
    std::atomic<uint32_t> mRingRead{0};

    std::atomic<double> mInputMeter{0.0};
    std::atomic<double> mOutputMeter{0.0};

    jack_client_t *mClient = nullptr;
    jack_port_t *mInPort = nullptr;
    jack_port_t *mOutPorts[2] = {nullptr, nullptr};

    Steinberg::Vst::IAudioProcessor *mProcessor = nullptr;
    Steinberg::Vst::IComponent *mComponent = nullptr;

    double mSampleRate = 48000.0;
    int mBlockSize = 1024;

    // Pre-allocated VST3 process plumbing, owned by the audio thread once
    // open() returns.
    Steinberg::Vst::HostProcessData mProcessData;
    Steinberg::Vst::ParameterChanges mInputChanges;
    Steinberg::Vst::ParameterChanges mOutputChanges;
};

} // namespace NAMix
