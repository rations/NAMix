// NAMix processor implementation. See namprocessor.h for the threading and
// real-time contract.

#include "namprocessor.h"
#include "namids.h"
#include "resamplingnam.h"

#include "NAM/get_dsp.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>

// Flush-to-zero / denormals-are-zero, re-armed on every process() call: JACK
// does not set FTZ/DAZ on client process threads, and subnormals in the NAM
// feedback/filter paths would stall the CPU and blow the RT deadline.
#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define NAMIX_HAVE_SSE_DENORMAL 1
#endif

static inline void nam_set_denormal_mode(void)
{
#ifdef NAMIX_HAVE_SSE_DENORMAL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

using namespace Steinberg;

namespace NAMix
{

namespace
{

inline double denorm(double norm, double min, double max)
{
    return min + norm * (max - min);
}

inline double dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Map a linear peak to the meter's normalized 0 .. 1 display range.
inline double peakToMeterNorm(double peak)
{
    const double db = 20.0 * std::log10(std::max(peak, 1e-7));
    const double norm = (db - ranges::kMeterMinDb) / (ranges::kMeterMaxDb - ranges::kMeterMinDb);
    return norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
}

// Write one point to an output parameter queue. RT-safe under the SDK host
// implementation: ParameterChanges is pre-sized by the host and every queue
// pre-reserves points at construction — the point-count guard keeps us inside
// that reserve so addPoint never grows the vector on the RT thread.
inline void writeOutputPoint(Vst::IParameterChanges *outChanges, Vst::ParamID id,
                             Vst::ParamValue value, Steinberg::int32 sampleOffset)
{
    if (!outChanges)
        return;
    Steinberg::int32 queueIndex = 0;
    Vst::IParamValueQueue *queue = outChanges->addParameterData(id, queueIndex);
    if (!queue || queue->getPointCount() >= 4)
        return;
    Steinberg::int32 pointIndex = 0;
    queue->addPoint(sampleOffset, value, pointIndex);
}

} // namespace

//------------------------------------------------------------------------
NamProcessor::NamProcessor()
{
    setControllerClass(FUID::fromTUID(NamControllerUID));
    // Trigger detects level on the model input and drives the Gain stage
    // that attenuates the model output.
    mNoiseGateTrigger.AddListener(&mNoiseGateGain);
}

NamProcessor::~NamProcessor() = default;

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::initialize(FUnknown *context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    addAudioInput(STR16("Input"), Vst::SpeakerArr::kMono);
    addAudioOutput(STR16("Output"), Vst::SpeakerArr::kStereo);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::setBusArrangements(Vst::SpeakerArrangement *inputs, int32 numIns,
                                                    Vst::SpeakerArrangement *outputs, int32 numOuts)
{
    // Accepted layouts: mono or stereo in (channel 0 is used), mono or
    // stereo out (mono result is copied to every output channel).
    if (numIns != 1 || numOuts != 1)
        return kResultFalse;
    if (inputs[0] != Vst::SpeakerArr::kMono && inputs[0] != Vst::SpeakerArr::kStereo)
        return kResultFalse;
    if (outputs[0] != Vst::SpeakerArr::kMono && outputs[0] != Vst::SpeakerArr::kStereo)
        return kResultFalse;
    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::setupProcessing(Vst::ProcessSetup &setup)
{
    tresult result = AudioEffect::setupProcessing(setup);
    if (result != kResultOk)
        return result;

    mSampleRate = setup.sampleRate;
    mMaxBlockSize = setup.maxSamplesPerBlock;

    mWorkBufInput.assign(static_cast<size_t>(mMaxBlockSize), 0.0);
    mWorkBufOutput.assign(static_cast<size_t>(mMaxBlockSize), 0.0);
    mWorkPtrInput = mWorkBufInput.data();
    mWorkPtrOutput = mWorkBufOutput.data();

    mToneStack.Reset(mSampleRate, mMaxBlockSize);
    mNoiseGateTrigger.SetSampleRate(mSampleRate);

    // Message thread: safe to touch both current and pending objects here —
    // processing is guaranteed inactive during setupProcessing.
    if (mModel)
        mModel->Reset(mSampleRate, mMaxBlockSize);
    if (!mIrPath.empty())
        loadIr(mIrPath); // the IR is resampled at load time for the new rate

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::setActive(TBool state)
{
    if (state) {
        // Pre-run the gate/tone-stack once so their internal buffers are
        // sized before the first RT block (AudioDSPTools allocates lazily).
        const size_t n = static_cast<size_t>(mMaxBlockSize);
        std::fill(mWorkBufInput.begin(), mWorkBufInput.end(), 0.0);
        DSP_SAMPLE **warm = &mWorkPtrInput;
        warm = mNoiseGateTrigger.Process(warm, 1, n);
        warm = mNoiseGateGain.Process(warm, 1, n);
        mToneStack.Process(warm, 1, static_cast<int>(n));
    }
    return AudioEffect::setActive(state);
}

//------------------------------------------------------------------------
uint32 PLUGIN_API NamProcessor::getLatencySamples()
{
    return mLatency.load(std::memory_order_relaxed);
}

//------------------------------------------------------------------------
void NamProcessor::handleParameterChanges(Vst::IParameterChanges *changes)
{
    if (!changes)
        return;
    int32 numParams = changes->getParameterCount();
    for (int32 i = 0; i < numParams; ++i) {
        Vst::IParamValueQueue *queue = changes->getParameterData(i);
        if (!queue)
            continue;
        Vst::ParamValue value;
        int32 sampleOffset;
        int32 numPoints = queue->getPointCount();
        if (numPoints < 1 || queue->getPoint(numPoints - 1, sampleOffset, value) != kResultTrue)
            continue;
        switch (queue->getParameterId()) {
            case kBypassId:
                mBypass.store(value, std::memory_order_relaxed);
                break;
            case kInputGainId:
                mInputGainNorm.store(value, std::memory_order_relaxed);
                break;
            case kOutputGainId:
                mOutputGainNorm.store(value, std::memory_order_relaxed);
                break;
            case kNoiseGateThresholdId:
                mNgThresholdNorm.store(value, std::memory_order_relaxed);
                break;
            case kBassId:
                mBassNorm.store(value, std::memory_order_relaxed);
                break;
            case kMiddleId:
                mMiddleNorm.store(value, std::memory_order_relaxed);
                break;
            case kTrebleId:
                mTrebleNorm.store(value, std::memory_order_relaxed);
                break;
            case kToneStackOnId:
                mToneStackOn.store(value, std::memory_order_relaxed);
                break;
            case kNoiseGateOnId:
                mNoiseGateOn.store(value, std::memory_order_relaxed);
                break;
            case kOutputModeId:
                mOutputModeNorm.store(value, std::memory_order_relaxed);
                break;
            // Slim is only recorded here (for getState); the DSP application
            // is NOT RT-safe and arrives separately via kMsgSetSlim.
            case kSlimId:
                mSlimNorm.store(value, std::memory_order_relaxed);
                break;
            case kCalibrateInputId:
                mCalibrateInput.store(value, std::memory_order_relaxed);
                break;
            case kInputCalibrationLevelId:
                mCalLevelNorm.store(value, std::memory_order_relaxed);
                break;
        }
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::process(Vst::ProcessData &data)
{
    nam_set_denormal_mode();

    handleParameterChanges(data.inputParameterChanges);

    // Swap in any pending model/IR (published by the message thread).
    if (mModelPending.exchange(false, std::memory_order_acq_rel)) {
        // The retired model is MOVED BACK into mPendingModel so its
        // destruction happens on the message thread at the next load, not
        // here on the RT thread.
        std::swap(mModel, mPendingModel);
        if (auto *r = static_cast<ResamplingNAM *>(mModel.get()))
            mLatency.store(static_cast<uint32>(r->GetLatency()), std::memory_order_relaxed);
        else
            mLatency.store(0, std::memory_order_relaxed);
    }
    if (mIRPending.exchange(false, std::memory_order_acq_rel))
        std::swap(mIR, mPendingIR);

    if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1)
        return kResultOk;
    if (!data.inputs[0].channelBuffers32 || !data.outputs[0].channelBuffers32)
        return kResultOk;

    const int32 numSamples = std::min(data.numSamples, mMaxBlockSize);
    float *in = data.inputs[0].channelBuffers32[0];
    float *out = data.outputs[0].channelBuffers32[0];

    if (mBypass.load(std::memory_order_relaxed) > 0.5) {
        if (out != in)
            memcpy(out, in, static_cast<size_t>(numSamples) * sizeof(float));
    } else {
        applyDsp(in, out, numSamples);
    }

    // Mono result -> every remaining output channel.
    for (int32 ch = 1; ch < data.outputs[0].numChannels; ++ch) {
        float *dst = data.outputs[0].channelBuffers32[ch];
        if (dst && dst != out)
            memcpy(dst, out, static_cast<size_t>(numSamples) * sizeof(float));
    }
    data.outputs[0].silenceFlags = 0;

    // Input/output level meters: per-block peak -> normalized display value,
    // pushed to the editor via output parameter changes. RT-safe (no alloc;
    // the host pre-sizes outputParameterChanges, and the writes are skipped
    // when it is null). One point per meter per block.
    if (data.outputParameterChanges) {
        double inPeak = 0.0, outPeak = 0.0;
        for (int32 i = 0; i < numSamples; ++i) {
            inPeak = std::max(inPeak, std::fabs(static_cast<double>(in[i])));
            outPeak = std::max(outPeak, std::fabs(static_cast<double>(out[i])));
        }
        writeOutputPoint(data.outputParameterChanges, kInputMeterId, peakToMeterNorm(inPeak), 0);
        writeOutputPoint(data.outputParameterChanges, kOutputMeterId, peakToMeterNorm(outPeak), 0);
    }

    return kResultOk;
}

//------------------------------------------------------------------------
void NamProcessor::applyDsp(float *in, float *out, int32 numSamples)
{
    const double calLevelDbu =
        denorm(mCalLevelNorm.load(std::memory_order_relaxed), ranges::kCalMin, ranges::kCalMax);
    double inputGainDb =
        denorm(mInputGainNorm.load(std::memory_order_relaxed), ranges::kGainMin, ranges::kGainMax);
    // Input calibration (models that state their expected input level only):
    // shift the interface's level to the level the capture was made at.
    if (mModel && mModel->HasInputLevel() && mCalibrateInput.load(std::memory_order_relaxed) > 0.5)
        inputGainDb += calLevelDbu - mModel->GetInputLevel();
    const double outputGainDb =
        denorm(mOutputGainNorm.load(std::memory_order_relaxed), ranges::kGainMin, ranges::kGainMax);
    const double inputGain = dbToLinear(inputGainDb);
    const double outputGain = dbToLinear(outputGainDb);
    const bool ngOn = mNoiseGateOn.load(std::memory_order_relaxed) > 0.5;
    const bool tsOn = mToneStackOn.load(std::memory_order_relaxed) > 0.5;

    // 1. float -> double with input gain.
    for (int32 i = 0; i < numSamples; ++i)
        mWorkBufInput[static_cast<size_t>(i)] = static_cast<DSP_SAMPLE>(in[i]) * inputGain;

    // 2. Noise-gate trigger (level detection; returns the gated signal).
    DSP_SAMPLE **processingInput = &mWorkPtrInput;
    if (ngOn) {
        const double ngThreshDb = denorm(mNgThresholdNorm.load(std::memory_order_relaxed),
                                         ranges::kNgMin, ranges::kNgMax);
        const dsp::noise_gate::TriggerParams triggerParams(0.01, ngThreshDb, 0.1, 0.005, 0.01,
                                                           0.05);
        mNoiseGateTrigger.SetParams(triggerParams);
        processingInput =
            mNoiseGateTrigger.Process(&mWorkPtrInput, 1, static_cast<size_t>(numSamples));
    }

    // 3. NAM model (mono, double precision). NAM_SAMPLE and DSP_SAMPLE are
    // both double by default; the cast only satisfies the type system.
    DSP_SAMPLE **modelOutput = processingInput;
    if (mModel) {
        mModel->process(reinterpret_cast<NAM_SAMPLE **>(processingInput),
                        reinterpret_cast<NAM_SAMPLE **>(&mWorkPtrOutput), numSamples);
        modelOutput = &mWorkPtrOutput;
    } else {
        const DSP_SAMPLE *src = processingInput[0];
        for (int32 i = 0; i < numSamples; ++i)
            mWorkBufOutput[static_cast<size_t>(i)] = src[i];
        modelOutput = &mWorkPtrOutput;
    }

    // 4. Noise-gate gain (applies the envelope to the model output).
    DSP_SAMPLE **gateOutput = modelOutput;
    if (ngOn)
        gateOutput = mNoiseGateGain.Process(modelOutput, 1, static_cast<size_t>(numSamples));

    // 5. Tone stack.
    DSP_SAMPLE **tsOutput = gateOutput;
    if (tsOn) {
        mToneStack.SetParam("bass", denorm(mBassNorm.load(std::memory_order_relaxed),
                                           ranges::kToneMin, ranges::kToneMax));
        mToneStack.SetParam("middle", denorm(mMiddleNorm.load(std::memory_order_relaxed),
                                             ranges::kToneMin, ranges::kToneMax));
        mToneStack.SetParam("treble", denorm(mTrebleNorm.load(std::memory_order_relaxed),
                                             ranges::kToneMin, ranges::kToneMax));
        tsOutput = mToneStack.Process(gateOutput, 1, numSamples);
    }

    // 6. IR convolution.
    DSP_SAMPLE **irOutput = tsOutput;
    if (mIR)
        irOutput = mIR->Process(tsOutput, 1, static_cast<size_t>(numSamples));

    // 7. double -> float with output gain and output mode.
    double modeGain = 1.0;
    const int outMode =
        static_cast<int>(mOutputModeNorm.load(std::memory_order_relaxed) * 2.0 + 0.5);
    if (outMode == 1 && mModel && mModel->HasLoudness()) {
        // Normalized: bring the model's measured loudness to -18 dB.
        modeGain = dbToLinear(-18.0 - mModel->GetLoudness());
    } else if (outMode == 2 && mModel && mModel->HasOutputLevel()) {
        // Calibrated: the model's stated output level relative to the
        // user's input calibration level (matches the original plug-in).
        modeGain = dbToLinear(mModel->GetOutputLevel() - calLevelDbu);
    }
    const double finalGain = outputGain * modeGain;
    const DSP_SAMPLE *finalBuf = irOutput[0];
    for (int32 i = 0; i < numSamples; ++i)
        out[i] = static_cast<float>(finalBuf[i] * finalGain);
}

//------------------------------------------------------------------------
// Tell the controller which optional features the loaded capture supports,
// so it can retitle the unsupported parameters. Message thread only; runs
// after every model load or clear. Capabilities are read from the new model
// BEFORE it is staged for the RT swap, so no model slot is touched here.
void NamProcessor::sendModelCaps(bool slimmable, bool hasInputLevel, bool hasOutputLevel)
{
    Steinberg::IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgModelCaps);
    Vst::IAttributeList *attrs = message->getAttributes();
    attrs->setInt(kCapsSlimmableAttr, slimmable ? 1 : 0);
    attrs->setInt(kCapsInLevelAttr, hasInputLevel ? 1 : 0);
    attrs->setInt(kCapsOutLevelAttr, hasOutputLevel ? 1 : 0);
    sendMessage(message);
}

//------------------------------------------------------------------------
bool NamProcessor::loadModel(const std::string &path)
{
    if (path.empty()) {
        mPendingModel.reset();
        mModelPath.clear();
        mModelPending.store(true, std::memory_order_release);
        sendModelCaps(false, false, false);
        return true;
    }
    try {
        auto raw = nam::get_dsp(std::filesystem::path(path));
        if (!raw || raw->NumInputChannels() != 1 || raw->NumOutputChannels() != 1)
            return false;
        auto wrapped = std::make_unique<ResamplingNAM>(std::move(raw), mSampleRate);
        wrapped->Reset(mSampleRate, mMaxBlockSize);
        // Slimmable (A2) models: bake the current Slim setting into the new
        // model before it is staged for the RT swap.
        if (auto *s = wrapped->GetSlimmableModel())
            s->SetSlimmableSize(mSlimNorm.load(std::memory_order_relaxed));
        const bool slimmable = wrapped->GetSlimmableModel() != nullptr;
        const bool hasIn = wrapped->HasInputLevel();
        const bool hasOut = wrapped->HasOutputLevel();
        mPendingModel = std::move(wrapped);
        mModelPath = path;
        mModelPending.store(true, std::memory_order_release);
        sendModelCaps(slimmable, hasIn, hasOut);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

//------------------------------------------------------------------------
bool NamProcessor::loadIr(const std::string &path)
{
    if (path.empty()) {
        mPendingIR.reset();
        mIrPath.clear();
        mIRPending.store(true, std::memory_order_release);
        return true;
    }
    try {
        auto ir = std::make_unique<dsp::ImpulseResponse>(path.c_str(), mSampleRate);
        if (ir->GetWavState() != dsp::wav::LoadReturnCode::SUCCESS)
            return false;
        mPendingIR = std::move(ir);
        mIrPath = path;
        mIRPending.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::notify(Vst::IMessage *message)
{
    if (!message)
        return kInvalidArgument;

    const char *id = message->getMessageID();

    // Slim: apply on this (message) thread — SetSlimmableSize stages a
    // partial network rebuild that the model's own process() installs
    // RT-safely, but the staging itself allocates and must stay off-RT.
    if (id && strcmp(id, kMsgSetSlim) == 0) {
        double v = 0.0;
        if (message->getAttributes()->getFloat(kSlimAttr, v) != kResultOk)
            return kResultFalse;
        mSlimNorm.store(v, std::memory_order_relaxed);
        applySlim(v);
        return kResultOk;
    }

    const bool isModel = id && strcmp(id, kMsgLoadModel) == 0;
    const bool isIr = id && strcmp(id, kMsgLoadIr) == 0;
    if (!isModel && !isIr)
        return AudioEffect::notify(message);

    const void *data = nullptr;
    uint32 size = 0;
    std::string path;
    if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data &&
        size > 0)
        path.assign(static_cast<const char *>(data), size);

    const bool ok = isModel ? loadModel(path) : loadIr(path);
    return ok ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------
void NamProcessor::applySlim(double v)
{
    // Message thread only (serialized with loadModel/setState by the host).
    // While a freshly staged model awaits the RT pending-swap the two model
    // slots may be exchanged at any instant, so leave them alone: the staged
    // model was already built with the current Slim value in loadModel().
    if (mModelPending.load(std::memory_order_acquire))
        return;
    if (auto *r = dynamic_cast<ResamplingNAM *>(mModel.get()))
        if (auto *s = r->GetSlimmableModel())
            s->SetSlimmableSize(v);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::setState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 3)
        return kResultFalse;

    double values[10] = {0};
    for (double &v : values)
        if (!streamer.readDouble(v))
            return kResultFalse;

    mBypass.store(values[0], std::memory_order_relaxed);
    mInputGainNorm.store(values[1], std::memory_order_relaxed);
    mOutputGainNorm.store(values[2], std::memory_order_relaxed);
    mNgThresholdNorm.store(values[3], std::memory_order_relaxed);
    mBassNorm.store(values[4], std::memory_order_relaxed);
    mMiddleNorm.store(values[5], std::memory_order_relaxed);
    mTrebleNorm.store(values[6], std::memory_order_relaxed);
    mToneStackOn.store(values[7], std::memory_order_relaxed);
    mNoiseGateOn.store(values[8], std::memory_order_relaxed);
    mOutputModeNorm.store(values[9], std::memory_order_relaxed);

    // Version 2 appends Slim (v1 states default to 0). Stored before the
    // model loads below so a restored slimmable model is built with it.
    double slim = 0.0;
    if (version >= 2 && !streamer.readDouble(slim))
        return kResultFalse;
    mSlimNorm.store(slim, std::memory_order_relaxed);

    // Version 3 appends the input-calibration pair.
    double calOn = 0.0, calLevel = 0.6; // defaults: off, 12 dBu
    if (version >= 3 && (!streamer.readDouble(calOn) || !streamer.readDouble(calLevel)))
        return kResultFalse;
    mCalibrateInput.store(calOn, std::memory_order_relaxed);
    mCalLevelNorm.store(calLevel, std::memory_order_relaxed);

    // Paths (written with writeStr8: int32 length + bytes). Missing entries
    // (old/foreign state) are tolerated: paths simply stay empty.
    if (char8 *modelPath = streamer.readStr8()) {
        loadModel(modelPath);
        delete[] modelPath;
    }
    if (char8 *irPath = streamer.readStr8()) {
        loadIr(irPath);
        delete[] irPath;
    }
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamProcessor::getState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    streamer.writeInt32(3); // state version (2 = +Slim, 3 = +input calibration)

    streamer.writeDouble(mBypass.load(std::memory_order_relaxed));
    streamer.writeDouble(mInputGainNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mOutputGainNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mNgThresholdNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mBassNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mMiddleNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mTrebleNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mToneStackOn.load(std::memory_order_relaxed));
    streamer.writeDouble(mNoiseGateOn.load(std::memory_order_relaxed));
    streamer.writeDouble(mOutputModeNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mSlimNorm.load(std::memory_order_relaxed));       // v2
    streamer.writeDouble(mCalibrateInput.load(std::memory_order_relaxed)); // v3
    streamer.writeDouble(mCalLevelNorm.load(std::memory_order_relaxed));   // v3

    streamer.writeStr8(mModelPath.c_str());
    streamer.writeStr8(mIrPath.c_str());
    return kResultOk;
}

} // namespace NAMix
