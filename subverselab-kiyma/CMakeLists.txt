cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Kiyma VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF: the build never writes into ~/Library/Audio/Plug-Ins. Pass
# -DSVL_COPY_PLUGINS=ON to let JUCE copy the built VST3/AU there after each build.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE is fetched by CMake. To build offline against a checkout already on
# disk, pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_KIYMA_FORMATS Standalone VST3 AU)
else()
    set(SVL_KIYMA_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_KIYMA_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_KIYMA_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Kiyma
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Kyma
    PLUGIN_NAME "KIYMA"
    PRODUCT_NAME "KIYMA"
    BUNDLE_ID com.subverselab.kiyma
    VERSION "1.0.0"
    FORMATS ${SVL_KIYMA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_KIYMA_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect
    # The standalone app reads the audio input: macOS asks for microphone
    # access with this text, and the hardened runtime needs the entitlement.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Kiyma)

# The Artist Edition face: backdrop, grinder body, crank, cast-iron plinth and knob (all at 2x) and the two typefaces
# (Shrikhand and DejaVu Sans Mono Bold, licences next to them), so nothing depends on the machine.
juce_add_binary_data(KiymaAssets SOURCES
    Assets/backdrop@2x.jpg Assets/body@2x.png Assets/crank@2x.png
    Assets/plinth@2x.png Assets/knob@2x.png Assets/seal@2x.png Assets/babasultan_B@2x.png
    Assets/Fonts/Shrikhand-Regular.ttf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(KiymaAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KIYMA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_sources(Kiyma PRIVATE ${KIYMA_SOURCES})

set(KIYMA_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Kiyma PUBLIC ${KIYMA_DEFS})

target_link_libraries(Kiyma
    PRIVATE
        KiymaAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plugin code built into the console tools below, with the macros the
# plugin wrapper would otherwise define.
set(KIYMA_TOOL_DEFS ${KIYMA_DEFS}
    JucePlugin_Name="KIYMA"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
set(KIYMA_TOOL_LIBS KiymaAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_dsp juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# render: --selftest (null, finite, latency, steps, level, events, undo) and WAV renders;
# --controlproof runs the shared control harness through the real processor.
juce_add_console_app(render PRODUCT_NAME "render")
juce_generate_juce_header(render)
target_sources(render PRIVATE tests/render.cpp ${KIYMA_SOURCES})
target_compile_definitions(render PRIVATE ${KIYMA_TOOL_DEFS})
target_link_libraries(render PRIVATE ${KIYMA_TOOL_LIBS})

# ui_snapshot: draws the editor to a PNG without a screen.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${KIYMA_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${KIYMA_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE ${KIYMA_TOOL_LIBS})
