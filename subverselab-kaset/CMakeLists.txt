cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Kaset VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is fetched by CMake. To build offline against a checkout already on
# disk, pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

# AU is macOS-only; Windows builds VST3 + Standalone. COPY_PLUGIN_AFTER_BUILD
# is also macOS-only so CI on Windows doesn't try to copy into system plugin
# folders it doesn't have permission for.
if(APPLE)
    set(SVL_KASET_FORMATS Standalone VST3 AU)
    set(SVL_KASET_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_KASET_FORMATS Standalone VST3)
    set(SVL_KASET_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Kaset
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Kast
    PLUGIN_NAME "SubverseLab Kaset"
    PRODUCT_NAME "SubverseLab Kaset"
    BUNDLE_ID com.subverselab.kaset
    VERSION "1.0.0"
    FORMATS ${SVL_KASET_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_KASET_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect
)

juce_generate_juce_header(Kaset)

# The SubverseLab mark and wordmark, cream on transparent, from Assets/Brand/logo.
juce_add_binary_data(KasetAssets SOURCES Assets/mark_cream.png Assets/wordmark_cream.png)
set_target_properties(KasetAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KASET_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_sources(Kaset PRIVATE ${KASET_SOURCES})

set(KASET_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Kaset PUBLIC ${KASET_DEFS})

target_link_libraries(Kaset
    PRIVATE
        KasetAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plugin code built into the console tools below, with the macros the
# plugin wrapper would otherwise define.
set(KASET_TOOL_DEFS ${KASET_DEFS}
    JucePlugin_Name="SubverseLab Kaset"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(KASET_TOOL_LIBS KasetAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_dsp juce::juce_gui_basics juce::juce_graphics juce::juce_core)

# Latency, generation, auto-gain, determinism and level checks (--selftest), and WAV renders.
juce_add_console_app(render PRODUCT_NAME "render")
juce_generate_juce_header(render)
target_sources(render PRIVATE tests/render.cpp ${KASET_SOURCES})
target_compile_definitions(render PRIVATE ${KASET_TOOL_DEFS})
target_link_libraries(render PRIVATE ${KASET_TOOL_LIBS})

# Draws the editor to a PNG.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${KASET_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${KASET_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE ${KASET_TOOL_LIBS})
