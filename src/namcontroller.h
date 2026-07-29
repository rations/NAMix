// NAMix edit controller — parameters plus the INamFileLoader interface that
// lets GUI-less hosts load .nam models and .wav impulse responses.

#pragma once

#include "inamfileloader.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <string>

namespace NAMix
{

class NamEditorView;

//------------------------------------------------------------------------
class NamController : public Steinberg::Vst::EditController, public INamFileLoader
{
public:
    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IEditController *)new NamController();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    // Overridden to forward Slim changes to the processor as a message: its
    // DSP application (a partial network rebuild) must run on the message
    // thread, not on the RT thread the parameter queue feeds.
    Steinberg::tresult PLUGIN_API
    setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;
    // Receives the processor's model-capability message and retitles the
    // parameters the loaded capture does not support ("… (n/a)").
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *message) SMTG_OVERRIDE;

    // Native X11 editor (IPlugView). The live view is tracked through the
    // EditorView attach hooks; setParamNormalized and the model-caps message
    // push updates to it. All run on the host's UI/run-loop thread (host
    // contract), so no locking is needed around mView.
    Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;
    void editorAttached(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;
    void editorRemoved(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;

    // INamFileLoader
    Steinberg::tresult PLUGIN_API setModelFile(const Steinberg::char8 *path) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setIrFile(const Steinberg::char8 *path) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getModelFile(Steinberg::char8 *buffer,
                                               Steinberg::int32 bufferSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getIrFile(Steinberg::char8 *buffer,
                                            Steinberg::int32 bufferSize) SMTG_OVERRIDE;

    //---Interface---------
    OBJ_METHODS(NamController, EditController)
    DEFINE_INTERFACES
    DEF_INTERFACE(INamFileLoader)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)

private:
    void retitleParam(Steinberg::Vst::ParamID tag, const char *title);
    Steinberg::tresult sendPath(const char *messageID, const Steinberg::char8 *path);
    static Steinberg::tresult copyPath(const std::string &src, Steinberg::char8 *buffer,
                                       Steinberg::int32 bufferSize);

    std::string mModelPath;
    std::string mIrPath;
    NamEditorView *mView = nullptr; // live editor, host UI/run-loop thread only
};

} // namespace NAMix
