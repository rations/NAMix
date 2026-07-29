// NAMix edit controller implementation.

#include "namcontroller.h"
#include "namids.h"
#include "namview.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <cstring>

using namespace Steinberg;

namespace NAMix
{

// The one and only definition of the INamFileLoader interface ID.
DEF_CLASS_IID(INamFileLoader)

//------------------------------------------------------------------------
tresult PLUGIN_API NamController::initialize(FUnknown *context)
{
    tresult result = EditController::initialize(context);
    if (result != kResultOk)
        return result;

    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass,
                            kBypassId);

    auto *inGain =
        new Vst::RangeParameter(STR16("Input Gain"), kInputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    inGain->setPrecision(1);
    parameters.addParameter(inGain);

    auto *outGain =
        new Vst::RangeParameter(STR16("Output Gain"), kOutputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    outGain->setPrecision(1);
    parameters.addParameter(outGain);

    auto *ngThresh =
        new Vst::RangeParameter(STR16("Noise Gate"), kNoiseGateThresholdId, STR16("dB"),
                                ranges::kNgMin, ranges::kNgMax, ranges::kNgDefault);
    ngThresh->setPrecision(1);
    parameters.addParameter(ngThresh);

    auto *bass = new Vst::RangeParameter(STR16("Bass"), kBassId, nullptr, ranges::kToneMin,
                                         ranges::kToneMax, ranges::kToneDefault);
    bass->setPrecision(1);
    parameters.addParameter(bass);

    auto *middle = new Vst::RangeParameter(STR16("Middle"), kMiddleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    middle->setPrecision(1);
    parameters.addParameter(middle);

    auto *treble = new Vst::RangeParameter(STR16("Treble"), kTrebleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    treble->setPrecision(1);
    parameters.addParameter(treble);

    parameters.addParameter(STR16("Tone Stack"), nullptr, 1, 1.0, Vst::ParameterInfo::kCanAutomate,
                            kToneStackOnId);
    parameters.addParameter(STR16("Noise Gate On"), nullptr, 1, 1.0,
                            Vst::ParameterInfo::kCanAutomate, kNoiseGateOnId);

    auto *outputMode = new Vst::StringListParameter(STR16("Output Mode"), kOutputModeId);
    outputMode->appendString(STR16("Raw"));
    outputMode->appendString(STR16("Normalized"));
    outputMode->appendString(STR16("Calibrated"));
    outputMode->setNormalized(0.5); // default: Normalized
    parameters.addParameter(outputMode);

    // Slim (0 .. 1, default 0): dynamic size reduction on slimmable (A2)
    // models — a no-op on models that don't support it, matching the
    // original plug-in (whose GUI shows the knob only for slimmable models).
    auto *slim = new Vst::RangeParameter(STR16("Slim"), kSlimId, nullptr, 0.0, 1.0, 0.0);
    slim->setPrecision(2);
    parameters.addParameter(slim);

    // Input calibration (models that state their expected input level only):
    // when on, the input gain is shifted by (level - model input level).
    parameters.addParameter(STR16("Calibrate Input"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate, kCalibrateInputId);
    auto *calLevel = new Vst::RangeParameter(STR16("Input Calibration Level"),
                                             kInputCalibrationLevelId, STR16("dBu"),
                                             ranges::kCalMin, ranges::kCalMax, ranges::kCalDefault);
    calLevel->setPrecision(1);
    parameters.addParameter(calLevel);

    // Input/output level meters (processor -> editor via output parameter
    // changes). Hidden and read-only: never automated, never persisted,
    // invisible to generic parameter UIs.
    parameters.addParameter(STR16("Input Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kInputMeterId);
    parameters.addParameter(STR16("Output Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kOutputMeterId);

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamController::setComponentState(IBStream *state)
{
    // Mirror of NamProcessor::getState — keep the two in sync.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 3)
        return kResultFalse;

    const Vst::ParamID ids[10] = {
        kBypassId, kInputGainId, kOutputGainId,  kNoiseGateThresholdId, kBassId,
        kMiddleId, kTrebleId,    kToneStackOnId, kNoiseGateOnId,        kOutputModeId};
    for (Vst::ParamID id : ids) {
        double v = 0;
        if (!streamer.readDouble(v))
            return kResultFalse;
        setParamNormalized(id, v);
    }
    if (version >= 2) { // v2 appends Slim
        double v = 0;
        if (!streamer.readDouble(v))
            return kResultFalse;
        // Base-class call on purpose: the processor already applied this
        // value in its own setState, so no kMsgSetSlim round-trip is needed.
        EditController::setParamNormalized(kSlimId, v);
    }
    if (version >= 3) { // v3 appends the input-calibration pair
        double calOn = 0, calLevel = 0;
        if (!streamer.readDouble(calOn) || !streamer.readDouble(calLevel))
            return kResultFalse;
        setParamNormalized(kCalibrateInputId, calOn);
        setParamNormalized(kInputCalibrationLevelId, calLevel);
    }

    if (char8 *modelPath = streamer.readStr8()) {
        mModelPath = modelPath;
        delete[] modelPath;
    }
    if (char8 *irPath = streamer.readStr8()) {
        mIrPath = irPath;
        delete[] irPath;
    }
    return kResultOk;
}

//------------------------------------------------------------------------
// Retitle a parameter in place and let the host re-read the titles. This is
// what a generic (host-drawn) parameter UI shows in place of the editor's
// greyed-out controls: a capture without the feature shows e.g. "Slim (n/a)".
void NamController::retitleParam(Vst::ParamID tag, const char *title)
{
    Vst::Parameter *param = parameters.getParameter(tag);
    if (!param)
        return;
    UString(param->getInfo().title, USTRINGSIZE(param->getInfo().title)).fromAscii(title);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamController::notify(Vst::IMessage *message)
{
    const char *id = message ? message->getMessageID() : nullptr;
    if (id && strcmp(id, kMsgModelCaps) == 0) {
        int64 slimmable = 0, hasIn = 0, hasOut = 0;
        Vst::IAttributeList *attrs = message->getAttributes();
        attrs->getInt(kCapsSlimmableAttr, slimmable);
        attrs->getInt(kCapsInLevelAttr, hasIn);
        attrs->getInt(kCapsOutLevelAttr, hasOut);

        retitleParam(kSlimId, slimmable ? "Slim" : "Slim (n/a)");
        retitleParam(kCalibrateInputId, hasIn ? "Calibrate Input" : "Calibrate Input (n/a)");
        retitleParam(kInputCalibrationLevelId,
                     hasIn ? "Input Calibration Level" : "Input Calibration Level (n/a)");
        retitleParam(kOutputModeId, hasOut ? "Output Mode" : "Output Mode (no calibration)");
        if (componentHandler)
            componentHandler->restartComponent(Vst::kParamTitlesChanged);
        // Let the editor disable the model-gated controls too.
        if (mView)
            mView->ModelCapsChanged(slimmable != 0, hasIn != 0, hasOut != 0);
        return kResultOk;
    }
    return EditController::notify(message);
}

//------------------------------------------------------------------------
IPlugView *PLUGIN_API NamController::createView(FIDString name)
{
    if (name && strcmp(name, Vst::ViewType::kEditor) == 0)
        return new NamEditorView(this);
    return nullptr;
}

void NamController::editorAttached(Vst::EditorView *editor)
{
    mView = static_cast<NamEditorView *>(editor);
}

void NamController::editorRemoved(Vst::EditorView *editor)
{
    if (mView == static_cast<NamEditorView *>(editor))
        mView = nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamController::setParamNormalized(Vst::ParamID tag, Vst::ParamValue value)
{
    tresult result = EditController::setParamNormalized(tag, value);
    if (tag == kSlimId && result == kResultOk) {
        // Hand the new size to the processor's message thread; the RT
        // parameter queue delivers the same value for state bookkeeping only.
        IPtr<Vst::IMessage> message = owned(allocateMessage());
        if (message) {
            message->setMessageID(kMsgSetSlim);
            message->getAttributes()->setFloat(kSlimAttr, value);
            sendMessage(message);
        }
    }
    // Push every value change into the live editor (metering, automation,
    // generic UI, state load). Same UI thread as the host caller.
    if (mView && result == kResultOk)
        mView->ParamChanged(tag, value);
    return result;
}

//------------------------------------------------------------------------
tresult NamController::sendPath(const char *messageID, const char8 *path)
{
    // Forward the path to the processor over the connection. When no peer is
    // connected (e.g. a host inspecting the controller alone), the local copy
    // is still updated and kResultFalse is returned.
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(messageID);
    const char *p = path ? path : "";
    message->getAttributes()->setBinary(kMsgPathAttr, p, static_cast<uint32>(strlen(p)));
    return sendMessage(message);
}

//------------------------------------------------------------------------
tresult PLUGIN_API NamController::setModelFile(const char8 *path)
{
    mModelPath = path ? path : "";
    return sendPath(kMsgLoadModel, path);
}

tresult PLUGIN_API NamController::setIrFile(const char8 *path)
{
    mIrPath = path ? path : "";
    return sendPath(kMsgLoadIr, path);
}

//------------------------------------------------------------------------
tresult NamController::copyPath(const std::string &src, char8 *buffer, int32 bufferSize)
{
    if (!buffer || bufferSize <= static_cast<int32>(src.size()))
        return kResultFalse;
    memcpy(buffer, src.c_str(), src.size() + 1);
    return kResultOk;
}

tresult PLUGIN_API NamController::getModelFile(char8 *buffer, int32 bufferSize)
{
    return copyPath(mModelPath, buffer, bufferSize);
}

tresult PLUGIN_API NamController::getIrFile(char8 *buffer, int32 bufferSize)
{
    return copyPath(mIrPath, buffer, bufferSize);
}

} // namespace NAMix
