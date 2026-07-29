// INamFileLoader — host-discoverable file-loading interface for NAMix.
//
// VST3 parameters are normalized floats only, so a model (.nam) or impulse
// response (.wav) path cannot travel as a parameter. The original NAM GUI has
// two folder buttons for this; a GUI-less host needs another route. This
// interface is implemented by NAMix's edit controller and discovered by hosts
// via queryInterface. A host that finds it can offer file pickers (or REPL
// commands); a host that does not know it is unaffected. The controller
// forwards the path to the processor over the SDK's IConnectionPoint message
// channel, and both paths persist in the plug-in state.
//
// The iid below is shared with the Haiku sibling port: it is one interface,
// implemented by the same plug-in on two operating systems. Hosts that already
// know it keep working unchanged. Frozen — never renumber.

#pragma once

#include "pluginterfaces/base/funknown.h"

namespace NAMix
{

//------------------------------------------------------------------------
class INamFileLoader : public Steinberg::FUnknown
{
public:
    // Set the model / IR file (absolute path, UTF-8). nullptr or "" clears.
    virtual Steinberg::tresult PLUGIN_API setModelFile(const Steinberg::char8 *path) = 0;
    virtual Steinberg::tresult PLUGIN_API setIrFile(const Steinberg::char8 *path) = 0;

    // Get the current path (UTF-8, empty string when nothing is loaded).
    // Returns kResultFalse if the buffer is too small.
    virtual Steinberg::tresult PLUGIN_API getModelFile(Steinberg::char8 *buffer,
                                                       Steinberg::int32 bufferSize) = 0;
    virtual Steinberg::tresult PLUGIN_API getIrFile(Steinberg::char8 *buffer,
                                                    Steinberg::int32 bufferSize) = 0;

    static const Steinberg::FUID iid;
};

DECLARE_CLASS_IID(INamFileLoader, 0x3BC74B8B, 0x68C2480B, 0xAF09BD1C, 0x668B595D)

} // namespace NAMix
