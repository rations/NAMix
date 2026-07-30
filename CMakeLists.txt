# NAMix — Neural Amp Modeler for Linux, a raw VST3 plug-in (no framework).
#
# The DSP core is the MIT-licensed NeuralAmpModelerCore + AudioDSPTools by
# Steven Atkinson (git submodules), with Eigen (MPL2) as the linear-algebra
# backend. The editor is a Steinberg IPlugView embedded into the host's X11
# window and painted with Cairo/FreeType — no plug-in or GUI framework.
#
# Builds two ways:
#   - standalone: point VST3_SDK_DIR at a VST 3 SDK checkout (default: the
#     in-tree vst3sdk/ submodule) and configure this directory directly;
#   - as a subdirectory of a project that already builds that SDK (the parent
#     must provide the SDK targets and public_sdk_SOURCE_DIR).

cmake_minimum_required(VERSION 3.25.0)

project(smtg-namix
    VERSION 0.5.0
    DESCRIPTION "NAMix neural amp modeler VST3 for Linux"
)

if(NOT TARGET sdk)
    set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk"
        CACHE PATH "VST 3 SDK source tree")
    if(NOT EXISTS ${VST3_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "VST 3 SDK not found at '${VST3_SDK_DIR}' - run "
            "'git submodule update --init --recursive' or set -DVST3_SDK_DIR=<path>")
    endif()

    # Headless SDK: core libraries plus the validator (part of the hosting
    # examples), no VSTGUI (we draw the editor ourselves), no plug-in examples.
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "editor is drawn directly" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "plug-in examples not needed" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install manually instead" FORCE)

    add_subdirectory(${VST3_SDK_DIR} vst3sdk)

    # The SDK's plugin helper functions read these directory-scoped variables
    # (smtg_target_add_library_main silently adds no module entry point without
    # public_sdk_SOURCE_DIR); the SDK sets them only in its own scope.
    set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    set(pluginterfaces_SOURCE_DIR "${VST3_SDK_DIR}/pluginterfaces")
endif()

set(NAMIX_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Editor graphics stack: Cairo drawing, FreeType text, PNG layers and runtime
# SVG rasterisation. Kept as its own library so the offline render tool below
# can exercise exactly the code the editor paints with, without X11 or VST3.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2 fontconfig)
# The editor embeds into the host's X11 window; cairo-xlib paints it.
pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)

add_library(NamGfx STATIC
    ${NAMIX_ROOT}/src/gfx/canvas.cpp
    ${NAMIX_ROOT}/src/gfx/fontstack.cpp
    ${NAMIX_ROOT}/src/gfx/image.cpp
    ${NAMIX_ROOT}/src/gfx/svg.cpp
    ${NAMIX_ROOT}/src/platform/respath.cpp
)
target_include_directories(NamGfx PUBLIC ${NAMIX_ROOT}/src)
target_link_libraries(NamGfx PUBLIC PkgConfig::CAIRO ${CMAKE_DL_LIBS})
target_compile_features(NamGfx PUBLIC cxx_std_17)
set_target_properties(NamGfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -Wall for our own code; the vendored NanoSVG headers are compiled as-is.
target_compile_options(NamGfx PRIVATE -Wall)

# ---------------------------------------------------------------------------
# NAM DSP core as a static library, linked --whole-archive: each network
# architecture (WaveNet, LSTM, ConvNet) self-registers via a file-scope static
# initialiser, and the linker would otherwise drop those object files from the
# plug-in .so, breaking model loading at runtime.
# ---------------------------------------------------------------------------
add_library(NamDsp STATIC
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/activations.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/container.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/conv1d.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/convnet.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/dsp.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/get_dsp.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/lstm.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/ring_buffer.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/util.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/wavenet/model.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/wavenet/a2_fast.cpp
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM/wavenet/slimmable.cpp
    ${NAMIX_ROOT}/AudioDSPTools/dsp/dsp.cpp
    ${NAMIX_ROOT}/AudioDSPTools/dsp/ImpulseResponse.cpp
    ${NAMIX_ROOT}/AudioDSPTools/dsp/NoiseGate.cpp
    ${NAMIX_ROOT}/AudioDSPTools/dsp/RecursiveLinearFilter.cpp
    ${NAMIX_ROOT}/AudioDSPTools/dsp/wav.cpp
    ${NAMIX_ROOT}/deps/tonestack/ToneStack.cpp
)

target_compile_definitions(NamDsp PUBLIC NAM_ENABLE_A2_FAST)

target_include_directories(NamDsp PUBLIC
    ${NAMIX_ROOT}/NeuralAmpModelerCore
    ${NAMIX_ROOT}/NeuralAmpModelerCore/NAM
    ${NAMIX_ROOT}/NeuralAmpModelerCore/Dependencies/nlohmann
    ${NAMIX_ROOT}/AudioDSPTools/dsp
    ${NAMIX_ROOT}/eigen
    ${NAMIX_ROOT}/deps/tonestack
)

# C++20: NAM's slimmable wavenet uses std::atomic<std::shared_ptr>, a C++20
# library feature. The plug-in itself stays at the SDK's C++17 baseline.
target_compile_features(NamDsp PUBLIC cxx_std_20)
target_compile_options(NamDsp PRIVATE -ffast-math -fPIC -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# The plug-in
# ---------------------------------------------------------------------------
smtg_add_vst3plugin(NAMix
    src/inamfileloader.h
    src/namids.h
    src/namgeometry.h
    src/version.h
    src/resamplingnam.h
    src/namprocessor.h
    src/namprocessor.cpp
    src/namcontroller.h
    src/namcontroller.cpp
    src/namview.h
    src/namview.cpp
    src/filebrowser.h
    src/filebrowser.cpp
    src/platform/x11plugview.h
    src/platform/x11plugview.cpp
    src/namfactory.cpp
)

smtg_target_configure_version_file(NAMix)

# Editor art -> bundle Contents/Resources/img/. The ten icons ship as the
# original SVGs (rasterised at runtime); the seven raster layers are PNG32
# imports of the upstream masters. Both sets come from gui/import_assets.sh.
file(GLOB NAMIX_IMG CONFIGURE_DEPENDS
    "${NAMIX_ROOT}/resources/img/*.png"
    "${NAMIX_ROOT}/resources/img/*.svg")
smtg_target_add_plugin_resources(NAMix
    RESOURCES ${NAMIX_IMG}
    OUTPUT_SUBDIRECTORY img
)

# Title (Michroma) and body (Roboto) fonts, loaded straight out of the bundle
# by FreeType — no system font installation needed.
smtg_target_add_plugin_resources(NAMix
    RESOURCES
        "${NAMIX_ROOT}/resources/fonts/Michroma-Regular.ttf"
        "${NAMIX_ROOT}/resources/fonts/Roboto-Regular.ttf"
    OUTPUT_SUBDIRECTORY fonts
)

target_compile_features(NAMix PUBLIC cxx_std_17)
target_compile_options(NAMix PRIVATE -ffast-math -Wall)

# AudioDSPTools' ResamplingContainer.h has an injected-class-name alias
# ("using LanczosResampler = LanczosResampler<...>") that GCC rejects outright
# while Clang accepts it. It reaches only this translation unit, via
# resamplingnam.h, so the relaxation is scoped to that file rather than to the
# whole target.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${NAMIX_ROOT}/src/namprocessor.cpp
        PROPERTIES COMPILE_OPTIONS -fpermissive)
endif()
target_include_directories(NAMix PRIVATE ${NAMIX_ROOT}/src)

target_link_libraries(NAMix
    PRIVATE
        sdk
        NamGfx
        PkgConfig::CAIROX11
        -Wl,--whole-archive NamDsp -Wl,--no-whole-archive
)

# ---------------------------------------------------------------------------
# panelrender — offline render of the editor to a PNG. No X11, no host, no
# VST3 objects: the graphics stack's test rig, and the reference shot used to
# compare the panel against the original plug-in.
# ---------------------------------------------------------------------------
add_executable(panelrender ${NAMIX_ROOT}/tools/panelrender.cpp)
target_link_libraries(panelrender PRIVATE NamGfx sdk)
target_compile_options(panelrender PRIVATE -Wall)

# ---------------------------------------------------------------------------
# namix-standalone — run the plug-in without a DAW: one X window, the plug-in's
# own editor embedded in it, a Linux::IRunLoop the editor registers with, and a
# JACK client driving the processor. The bundle is loaded as a module (not
# linked in) so the editor's dladdr-based resource lookup behaves exactly as it
# does inside a host.
# ---------------------------------------------------------------------------
pkg_check_modules(JACK IMPORTED_TARGET jack)
if(JACK_FOUND)
    # The module loader, the plug-in provider and the host-side interface IIDs
    # are compiled straight in, exactly as the SDK's own hosting samples do.
    add_executable(namix-standalone
        ${NAMIX_ROOT}/standalone/main.cpp
        ${NAMIX_ROOT}/standalone/runloop.h
        ${NAMIX_ROOT}/standalone/runloop.cpp
        ${NAMIX_ROOT}/standalone/jackclient.h
        ${NAMIX_ROOT}/standalone/jackclient.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    )
    target_include_directories(namix-standalone PRIVATE ${NAMIX_ROOT}/src)
    target_link_libraries(namix-standalone PRIVATE sdk_hosting PkgConfig::JACK
                                                   PkgConfig::CAIROX11)
    target_compile_features(namix-standalone PRIVATE cxx_std_17)
    target_compile_options(namix-standalone PRIVATE -Wall)
    add_dependencies(namix-standalone NAMix)
else()
    message(STATUS "[NAMix] JACK not found - skipping the standalone build")
endif()
