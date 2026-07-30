#pragma once

#include "pluginterfaces/base/fplatform.h"

#define MAJOR_VERSION_STR "0"
#define MAJOR_VERSION_INT 0
#define SUB_VERSION_STR "5"
#define SUB_VERSION_INT 5
#define RELEASE_NUMBER_STR "0"
#define RELEASE_NUMBER_INT 0
#define BUILD_NUMBER_STR "1"
#define BUILD_NUMBER_INT 1

#define FULL_VERSION_STR                                                                           \
    MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR "." BUILD_NUMBER_STR
#define VERSION_STR MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR

#define stringPluginName "NAMix"
#define stringOriginalFilename "NAMix.vst3"
#if SMTG_PLATFORM_64
#define stringFileDescription stringPluginName " (64Bit)"
#else
#define stringFileDescription stringPluginName
#endif
#define stringCompanyName "rations"
#define stringCompanyWeb "https://github.com/rations"
#define stringCompanyEmail "mailto:ehqcar@proton.me"
#define stringLegalCopyright "MIT licence; NAM DSP core (C) Steven Atkinson"
#define stringLegalTrademarks "VST is a trademark of Steinberg Media Technologies GmbH"
