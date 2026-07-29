// NAMix processor — audio component (mono in, stereo out).
//
// DSP chain (derived from NeuralAmpModelerPlugin by Steven Atkinson): input
// gain -> noise-gate trigger -> NAM model -> noise-gate gain -> tone stack ->
// IR convolution -> output gain/mode.
//
// Real-time contract: process() never allocates, locks, or does file I/O.
// Model/IR loading happens on the message thread (IConnectionPoint::notify or
// setState) and is handed to the RT thread via atomic pending-swap. Retired
// objects are freed on the message thread at the next load/destruction, never
// on the RT thread.

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "ImpulseResponse.h"
#include "NAM/dsp.h"
#include "NoiseGate.h"
#include "ToneStack.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace NAMix
{

class ResamplingNAM;

//------------------------------------------------------------------------
class NamProcessor : public Steinberg::Vst::AudioEffect
{
public:
    NamProcessor();
    ~NamProcessor() override;

    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IAudioProcessor *)new NamProcessor();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement *outputs,
                                                     Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) SMTG_OVERRIDE;

    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;

    // Controller -> processor file-load messages (message thread).
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *message) SMTG_OVERRIDE;

private:
    void handleParameterChanges(Steinberg::Vst::IParameterChanges *changes);
    void applyDsp(float *in, float *out, Steinberg::int32 numSamples);
    bool loadModel(const std::string &path); // message thread only
    bool loadIr(const std::string &path);    // message thread only
    void applySlim(double v);                // message thread only
    void sendModelCaps(bool slimmable, bool hasInputLevel,
                       bool hasOutputLevel); // message thread only

    // Normalized parameter values (written by RT param handling AND by
    // setState on the message thread; atomics keep the accesses tear-free).
    std::atomic<double> mBypass{0.0};
    std::atomic<double> mInputGainNorm{0.5};   // plain 0 dB
    std::atomic<double> mOutputGainNorm{0.5};  // plain 0 dB
    std::atomic<double> mNgThresholdNorm{0.2}; // plain -80 dB
    std::atomic<double> mBassNorm{0.5};        // plain 5
    std::atomic<double> mMiddleNorm{0.5};      // plain 5
    std::atomic<double> mTrebleNorm{0.5};      // plain 5
    std::atomic<double> mToneStackOn{1.0};
    std::atomic<double> mNoiseGateOn{1.0};
    std::atomic<double> mOutputModeNorm{0.5}; // index 1 = Normalized
    std::atomic<double> mSlimNorm{0.0};       // applied off-RT via kMsgSetSlim
    std::atomic<double> mCalibrateInput{0.0}; // toggle, default off
    std::atomic<double> mCalLevelNorm{0.6};   // plain 12 dBu in -60 .. 60

    // Model/IR, hot-swapped from the message thread.
    std::unique_ptr<nam::DSP> mPendingModel;
    std::unique_ptr<nam::DSP> mModel;
    std::atomic<bool> mModelPending{false};

    std::unique_ptr<dsp::ImpulseResponse> mPendingIR;
    std::unique_ptr<dsp::ImpulseResponse> mIR;
    std::atomic<bool> mIRPending{false};

    // Latency reported to the host, updated when a model swaps in.
    std::atomic<Steinberg::uint32> mLatency{0};

    dsp::noise_gate::Trigger mNoiseGateTrigger;
    dsp::noise_gate::Gain mNoiseGateGain;
    dsp::tone_stack::BasicNamToneStack mToneStack;

    // Pre-allocated double-precision work buffers (sized in setupProcessing).
    std::vector<DSP_SAMPLE> mWorkBufInput;
    std::vector<DSP_SAMPLE> mWorkBufOutput;
    DSP_SAMPLE *mWorkPtrInput = nullptr;
    DSP_SAMPLE *mWorkPtrOutput = nullptr;

    // Persisted file paths (message thread only).
    std::string mModelPath;
    std::string mIrPath;

    double mSampleRate = 48000.0;
    Steinberg::int32 mMaxBlockSize = 2048;
};

} // namespace NAMix
