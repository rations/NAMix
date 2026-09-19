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
    VERSION 0.8.0
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

# Editor art and fonts. The ten icons ship as the original SVGs (rasterised at
# runtime); the seven raster layers are PNG32 imports of the upstream masters.
# Both sets come from gui/import_assets.sh.
#
# Held as paths RELATIVE to resources/, because two bundle formats lay them out
# differently and only one list should decide what ships: a VST3 bundle keeps
# them under Contents/Resources/{img,fonts}, and an LV2 bundle keeps them beside
# its binaries. One list, two layouts.
file(GLOB NAMIX_IMG_ABS CONFIGURE_DEPENDS
    "${NAMIX_ROOT}/resources/img/*.png"
    "${NAMIX_ROOT}/resources/img/*.svg")
set(NAMIX_IMG_FILES "")
foreach(_img IN LISTS NAMIX_IMG_ABS)
    file(RELATIVE_PATH _rel "${NAMIX_ROOT}/resources" "${_img}")
    list(APPEND NAMIX_IMG_FILES "${_rel}")
endforeach()

# Title (Michroma) and body (Roboto), loaded straight out of the bundle by
# FreeType — no system font installation needed.
set(NAMIX_FONT_FILES
    fonts/Michroma-Regular.ttf
    fonts/Roboto-Regular.ttf)

smtg_target_add_plugin_resources(NAMix
    RESOURCES ${NAMIX_IMG_ABS}
    OUTPUT_SUBDIRECTORY img
)
set(NAMIX_FONT_ABS "")
foreach(_font IN LISTS NAMIX_FONT_FILES)
    list(APPEND NAMIX_FONT_ABS "${NAMIX_ROOT}/resources/${_font}")
endforeach()
smtg_target_add_plugin_resources(NAMix
    RESOURCES ${NAMIX_FONT_ABS}
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
# The LV2 build: NAMix.lv2, a second FORMAT rather than a second plug-in.
#
# Both shared objects host the very same C++ objects a VST3 DAW loads — one a
# NamProcessor driven through IAudioProcessor, the other a NamController and a
# NamEditorView driven through IPlugView — so there is one DSP path, one editor,
# and one state format, and everything this project has measured about them is a
# statement about this build too. src/lv2/namixlv2.h carries the reasoning;
# src/lv2/namix_lv2_ui.cpp carries the run-loop half of it.
#
# Linux only, because the editor half is X11: ui:X11UI is the only UI type
# declared, and src/platform/x11plugview.cpp is the window underneath it. That
# costs nothing here, since the VST3 build is Linux-only too.
# ---------------------------------------------------------------------------
pkg_check_modules(NAMIX_LV2 QUIET lv2)
# lilv is the reference discovery library and is what namix_lv2check asks. It is
# a TEST dependency only: nothing in the bundle links it.
pkg_check_modules(NAMIX_LILV QUIET lilv-0)

if(NAMIX_LV2_FOUND)
    # Reported rather than assumed: any version-sensitive fact has to record the
    # version it was checked against.
    message(STATUS "[NAMix] LV2 ${NAMIX_LV2_VERSION} found - building NAMix.lv2")

    set(NAMIX_LV2_BUNDLE "${CMAKE_BINARY_DIR}/lv2/NAMix.lv2")

    # --- the TTL generator -------------------------------------------------
    # Not a hand-written pair of .ttl files: an LV2 bundle states every
    # parameter's range, default, step count and name, and those already exist
    # once in the controller. This links the real controller, asks it, and
    # refuses to write anything when the port table disagrees with it.
    add_executable(namix_ttlgen
        ${NAMIX_ROOT}/tools/namix_ttlgen.cpp
        ${NAMIX_ROOT}/src/namcontroller.cpp
        ${NAMIX_ROOT}/src/namview.cpp
        ${NAMIX_ROOT}/src/filebrowser.cpp
        ${NAMIX_ROOT}/src/platform/x11plugview.cpp
    )
    target_include_directories(namix_ttlgen
        PRIVATE ${NAMIX_ROOT}/src ${NAMIX_LV2_INCLUDE_DIRS})
    target_link_libraries(namix_ttlgen
        PRIVATE sdk NamGfx PkgConfig::CAIROX11
                -Wl,--whole-archive NamDsp -Wl,--no-whole-archive)
    target_compile_features(namix_ttlgen PRIVATE cxx_std_17)
    target_compile_options(namix_ttlgen PRIVATE -Wall)

    # --- the DSP half ------------------------------------------------------
    # The same sources the VST3 bundle compiles, minus the editor and minus the
    # factory: an LV2 plug-in has one entry point and it is not GetPluginFactory.
    add_library(namix_lv2 MODULE
        ${NAMIX_ROOT}/src/lv2/namix_lv2.cpp
        ${NAMIX_ROOT}/src/lv2/lv2message.cpp
        ${NAMIX_ROOT}/src/lv2/lv2message.h
        ${NAMIX_ROOT}/src/lv2/namixlv2.h
        ${NAMIX_ROOT}/src/namprocessor.cpp
        ${NAMIX_ROOT}/src/platform/respath.cpp
        # The two SDK helpers a HOST needs and a plug-in does not, which is why
        # the `sdk` target does not carry them: the pre-allocated parameter
        # queues run() fills, and the memory stream getState/setState are handed.
        # The JACK standalone gets them from sdk_hosting; this takes the files,
        # because sdk_hosting also brings a module loader and a plug-in provider
        # that an LV2 bundle has no use for. There is no eventlist.cpp here: the
        # processor declares no event bus, so mData.inputEvents stays null.
        ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    )
    target_include_directories(namix_lv2
        PRIVATE ${NAMIX_ROOT}/src ${NAMIX_ROOT}/src/lv2 ${NAMIX_LV2_INCLUDE_DIRS})
    target_link_libraries(namix_lv2
        PRIVATE sdk -Wl,--whole-archive NamDsp -Wl,--no-whole-archive)
    target_compile_features(namix_lv2 PRIVATE cxx_std_17)
    # -ffast-math to match the VST3 bundle exactly. A second build of the same
    # DSP at a different floating-point contract would be a second set of
    # numbers. See mLastControl in namix_lv2.cpp for what that costs if it is
    # forgotten.
    target_compile_options(namix_lv2 PRIVATE -ffast-math -Wall)
    # The same GCC relaxation the VST3 target needs, and for the same file: see
    # the note on ResamplingContainer.h above.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(${NAMIX_ROOT}/src/namprocessor.cpp
            PROPERTIES COMPILE_OPTIONS -fpermissive)
    endif()

    # --- the UI half -------------------------------------------------------
    add_library(namix_lv2_ui MODULE
        ${NAMIX_ROOT}/src/lv2/namix_lv2_ui.cpp
        ${NAMIX_ROOT}/src/lv2/lv2message.cpp
        ${NAMIX_ROOT}/src/namcontroller.cpp
        ${NAMIX_ROOT}/src/namview.cpp
        ${NAMIX_ROOT}/src/filebrowser.cpp
        ${NAMIX_ROOT}/src/platform/x11plugview.cpp
        # setComponentState takes an IBStream, and the state blob arrives from the
        # DSP half as bytes; MemoryStream is what turns one into the other.
        ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    )
    target_include_directories(namix_lv2_ui
        PRIVATE ${NAMIX_ROOT}/src ${NAMIX_ROOT}/src/lv2 ${NAMIX_LV2_INCLUDE_DIRS})
    target_link_libraries(namix_lv2_ui PRIVATE sdk NamGfx PkgConfig::CAIROX11)
    target_compile_features(namix_lv2_ui PRIVATE cxx_std_17)
    target_compile_options(namix_lv2_ui PRIVATE -ffast-math -Wall)

    # The bundle's file names are fixed by the TTL the generator writes, so the
    # usual lib<name>.so would leave a manifest naming a binary that is not
    # there.
    foreach(_lv2_target namix_lv2 namix_lv2_ui)
        string(REPLACE "namix_lv2_ui" "NAMix_ui" _lv2_name "${_lv2_target}")
        string(REPLACE "namix_lv2" "NAMix" _lv2_name "${_lv2_name}")
        set_target_properties(${_lv2_target} PROPERTIES
            PREFIX ""
            OUTPUT_NAME "${_lv2_name}"
            LIBRARY_OUTPUT_DIRECTORY "${NAMIX_LV2_BUNDLE}"
            POSITION_INDEPENDENT_CODE ON
            # Two plug-ins in one host process that both export the same inline
            # or template symbol can be merged by the dynamic linker through
            # STB_GNU_UNIQUE, leaving one of them running the other's code. This
            # tree ships the VST3 and the LV2 of the SAME plug-in, so a host with
            # both installed is the collision path -- and the NAM architecture
            # registry is a file-scope static initialiser, which is exactly the
            # kind of symbol involved. Hidden by default, and the archives bound
            # locally below, leaves lv2_descriptor and lv2ui_descriptor (which
            # carry LV2_SYMBOL_EXPORT) as the entire public surface.
            # scripts/lv2-gate.sh measures that rather than trusting it.
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
        target_link_options(${_lv2_target} PRIVATE -Wl,--exclude-libs,ALL)
    endforeach()

    # --- the bundle --------------------------------------------------------
    # Art and fonts go in beside the binaries, which is the LV2 layout, and the
    # UI is handed that directory as instantiate()'s bundle_path. A VST3 bundle
    # keeps them two levels up under Contents/Resources instead, which is why
    # respath.cpp grew an explicit override rather than a second layout rule.
    set(NAMIX_LV2_STAMPS "")
    foreach(_res IN LISTS NAMIX_FONT_FILES NAMIX_IMG_FILES)
        set(_out "${NAMIX_LV2_BUNDLE}/${_res}")
        add_custom_command(OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NAMIX_ROOT}/resources/${_res}" "${_out}"
            DEPENDS "${NAMIX_ROOT}/resources/${_res}"
            COMMENT "lv2 bundle: ${_res}")
        list(APPEND NAMIX_LV2_STAMPS "${_out}")
    endforeach()

    add_custom_command(
        OUTPUT "${NAMIX_LV2_BUNDLE}/manifest.ttl" "${NAMIX_LV2_BUNDLE}/NAMix.ttl"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAMIX_LV2_BUNDLE}"
        COMMAND namix_ttlgen "${NAMIX_LV2_BUNDLE}"
        DEPENDS namix_ttlgen
        COMMENT "lv2 bundle: manifest.ttl and NAMix.ttl (checked against the controller)")

    # --- the bundle's own gate ---------------------------------------------
    # What a host actually sees, asked of lilv rather than of the source tree.
    # namix_ttlgen checks the code against the controller; this checks the
    # BUNDLE against the code, loads the binary, runs it and round-trips its
    # state — none of which the ordinary build can say anything about, and all
    # of which fail silently at runtime rather than at build time.
    if(NAMIX_LILV_FOUND)
        # lv2message.cpp comes along rather than being restated: the check
        # writes a message with the plug-in's OWN codec, so what it exercises is
        # the wire format the two halves use and not a second description of it.
        # vstinitiids.cpp is where the VST3 interface IIDs are DEFINED rather
        # than declared; a host-side translation unit that uses IMessage and
        # IAttributeList needs it, and the SDK expects each host to name it.
        add_executable(namix_lv2check
            ${NAMIX_ROOT}/tools/namix_lv2check.cpp
            ${NAMIX_ROOT}/src/lv2/lv2message.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp
            ${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp
            ${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp)
        target_include_directories(namix_lv2check
            PRIVATE ${NAMIX_ROOT}/src ${VST3_SDK_DIR}
                    ${NAMIX_LV2_INCLUDE_DIRS} ${NAMIX_LILV_INCLUDE_DIRS})
        target_link_libraries(namix_lv2check
            PRIVATE base ${NAMIX_LILV_LIBRARIES} ${CMAKE_DL_LIBS})
        target_link_directories(namix_lv2check PRIVATE ${NAMIX_LILV_LIBRARY_DIRS})
        target_compile_features(namix_lv2check PRIVATE cxx_std_17)
        target_compile_options(namix_lv2check PRIVATE -Wall)
    else()
        message(STATUS "[NAMix] lilv not found - skipping namix_lv2check")
    endif()

    add_custom_target(namix_lv2_bundle ALL
        DEPENDS "${NAMIX_LV2_BUNDLE}/manifest.ttl" "${NAMIX_LV2_BUNDLE}/NAMix.ttl"
                ${NAMIX_LV2_STAMPS} namix_lv2 namix_lv2_ui)
else()
    message(STATUS "[NAMix] LV2 headers not found (lv2.pc) - skipping NAMix.lv2")
endif()

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
