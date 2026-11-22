cmake_minimum_required(VERSION 3.22)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(HighStereo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SVL_COPY_PLUGINS "Copy built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE 9.0.1. Pass -DFETCHCONTENT_SOURCE_DIR_JUCE=<local juce-src> to build offline.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_HS_FORMATS VST3 AU Standalone)
else()
    set(SVL_HS_FORMATS VST3)
endif()

if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_HS_COPY TRUE)
else()
    set(SVL_HS_COPY FALSE)
endif()

juce_add_plugin(HighStereo
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE HiSt
    PLUGIN_NAME "HIGH STEREO"
    PRODUCT_NAME "HIGH STEREO"
    BUNDLE_ID com.subverselab.highstereo
    VERSION "1.0.0"
    FORMATS ${SVL_HS_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_HS_COPY}
    VST3_CATEGORIES Fx Spatial
    AU_MAIN_TYPE kAudioUnitType_Effect
)

juce_generate_juce_header(HighStereo)

juce_add_binary_data(HighStereoAssets SOURCES
    Assets/mark_cream.png Assets/wordmark.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(HighStereoAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(HS_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_sources(HighStereo PRIVATE ${HS_SOURCES})

set(HS_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(HighStereo PUBLIC ${HS_DEFS})

target_link_libraries(HighStereo
    PRIVATE
        HighStereoAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Engine self-test (plain C++).
add_executable(test_highstereo_engine tests/test_highstereo_engine.cpp)

# Preset checks against the real processor (headless console app).
set(HS_TOOL_DEFS ${HS_DEFS}
    JucePlugin_Name="HIGH STEREO"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
juce_add_console_app(test_presets PRODUCT_NAME "test_presets")
juce_generate_juce_header(test_presets)
target_sources(test_presets PRIVATE tests/test_presets.cpp ${HS_SOURCES})
target_compile_definitions(test_presets PRIVATE ${HS_TOOL_DEFS})
target_link_libraries(test_presets PRIVATE HighStereoAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_graphics juce::juce_core juce::juce_dsp
    juce::juce_recommended_config_flags)

# Panel snapshot, headless (the product's own snapshot tool; no screen capture).
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tools/ui_snapshot.cpp ${HS_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${HS_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE HighStereoAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_graphics juce::juce_core juce::juce_dsp
    juce::juce_recommended_config_flags)

# Panel mouse checks, headless (synthetic events into the real editor; no window).
juce_add_console_app(test_ui_mouse PRODUCT_NAME "test_ui_mouse")
juce_generate_juce_header(test_ui_mouse)
target_sources(test_ui_mouse PRIVATE tests/test_ui_mouse.cpp ${HS_SOURCES})
target_compile_definitions(test_ui_mouse PRIVATE ${HS_TOOL_DEFS})
target_link_libraries(test_ui_mouse PRIVATE HighStereoAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_graphics juce::juce_core juce::juce_dsp
    juce::juce_recommended_config_flags)
