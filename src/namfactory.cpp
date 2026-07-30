// NAMix plug-in factory.

#include "namcontroller.h"
#include "namids.h"
#include "namprocessor.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory_constexpr.h"

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail, 2)

DEF_CLASS(NAMix::NamProcessorUID, Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
          stringPluginName, Steinberg::Vst::kDistributable, "Fx|Distortion", FULL_VERSION_STR,
          kVstVersionString, NAMix::NamProcessor::createInstance, nullptr)

DEF_CLASS(NAMix::NamControllerUID, Steinberg::PClassInfo::kManyInstances,
          kVstComponentControllerClass, stringPluginName "Controller", 0, "", FULL_VERSION_STR,
          kVstVersionString, NAMix::NamController::createInstance, nullptr)

END_FACTORY
