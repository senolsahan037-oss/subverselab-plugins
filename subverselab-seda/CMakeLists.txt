cmake_minimum_required(VERSION 3.22)

# DEV shell: native arm64 for speed. Pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

project(Seda VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF in P3 (the licensing seam is Source/LicenseConfig.h with TODO ids, like DYNEQ / YANKI): build with -DSVL_LICENSING=ON to compile the
# Lemon Squeezy / SVL licence gate from ../subverselab-plugin-common/Licensing in. Not exercised in P3.
option(SVL_LICENSING "Build the Lemon Squeezy / SVL licence-activation gate into the plugin" OFF)

# OFF: the build never writes into ~/Library/Audio/Plug-Ins.
# Plug-in version (numeric, AU rule). Default unchanged; the tester build passes -DSVL_PLUGIN_VERSION=0.9.0 ("test1" lives only in the file names).
set(SVL_PLUGIN_VERSION "1.0.0" CACHE STRING "Numeric plug-in version")   # 1.0.0: the release (owner 19 Oct "sedayı da yayınlayalım")

option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE is fetched by CMake. To build offline against a checkout already on disk, pass
# -DFETCHCONTENT_SOURCE_DIR_JUCE=../subverselab-splitter-desktop/build/_deps/juce-src
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_SEDA_FORMATS Standalone VST3 AU)
else()
    set(SVL_SEDA_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_SEDA_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_SEDA_COPY_AFTER_BUILD FALSE)
endif()

# SEDA is a working name until the owner names the product (the code Seda and the bundle id follow the name later).
juce_add_plugin(Seda
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Seda
    PLUGIN_NAME "SEDA"
    PRODUCT_NAME "SEDA"
    BUNDLE_ID com.subverselab.seda
    VERSION "${SVL_PLUGIN_VERSION}"
    FORMATS ${SVL_SEDA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_SEDA_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Delay
    AU_MAIN_TYPE kAudioUnitType_Effect
    # The standalone app reads the audio input: macOS asks for microphone access with this text.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Seda)

# The SubverseLab mark and wordmark (cream on transparent) and the typefaces (TeX Gyre Heros, DejaVu Sans Mono), compiled in.
juce_add_binary_data(SedaAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(SedaAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(SEDA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_sources(Seda PRIVATE ${SEDA_SOURCES})

set(SEDA_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SVL_LICENSING=$<BOOL:${SVL_LICENSING}>)
target_compile_definitions(Seda PUBLIC ${SEDA_DEFS})

target_link_libraries(Seda
    PRIVATE
        SedaAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plug-in code built into the console tools below, with the macros the plugin wrapper would otherwise define.
set(SEDA_TOOL_DEFS ${SEDA_DEFS}
    JucePlugin_Name="SEDA"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(SEDA_TOOL_LIBS SedaAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# plugin_check: the real processor against the raw engine, state, bypass, block sizes, sample rates, tail, undo, control proof (tests/plugin_check.cpp).
juce_add_console_app(plugin_check PRODUCT_NAME "plugin_check")
juce_generate_juce_header(plugin_check)
target_sources(plugin_check PRIVATE tests/plugin_check.cpp ${SEDA_SOURCES})
target_compile_definitions(plugin_check PRIVATE ${SEDA_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(plugin_check PRIVATE ${SEDA_TOOL_LIBS})

# ui_snapshot: draws the editor to PNGs without a screen, after real programme has gone through the processor (the P4 harness).
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${SEDA_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${SEDA_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_snapshot PRIVATE ${SEDA_TOOL_LIBS})

# variation_rig: the variation test rig (tests/variation_rig.cpp): sweeps programs x LISTEN x SYNC x AUTO x NOTE x DUCK x THROW x STEREO x AGE x FEEDBACK through the real
# processor and editor, measures notes, delay time, duck depth, clicks, latency, panel cost, state round trips; prints a table, exit code 1 on any failure.
juce_add_console_app(variation_rig PRODUCT_NAME "variation_rig")
juce_generate_juce_header(variation_rig)
target_sources(variation_rig PRIVATE tests/variation_rig.cpp ${SEDA_SOURCES})
target_compile_definitions(variation_rig PRIVATE ${SEDA_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 SEDA_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(variation_rig PRIVATE ${SEDA_TOOL_LIBS})

# wrapper_check: loads the built VST3 / AU bundles like a host (JUCE plug-in hosting) and measures dry path, latency and default MIX through the real wrappers (tests/wrapper_check.cpp).
juce_add_console_app(wrapper_check PRODUCT_NAME "wrapper_check")
juce_generate_juce_header(wrapper_check)
target_sources(wrapper_check PRIVATE tests/wrapper_check.cpp)
target_compile_definitions(wrapper_check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(wrapper_check PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics juce::juce_core juce::juce_recommended_config_flags)

# sedaraw: offline renderer around Source/SedaEngine.h (no JUCE: the engine and the WAV reader are plain C++).
add_executable(sedaraw tools/sedaraw.cpp)
target_compile_options(sedaraw PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O3 -Wall -Wextra>)

# SourceSense v0 (plugin-common) command-line runner, used by tools/sourcesense_eval.py
add_executable(sensecli tools/sensecli.cpp)
target_compile_options(sensecli PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O3 -Wall -Wextra>)
