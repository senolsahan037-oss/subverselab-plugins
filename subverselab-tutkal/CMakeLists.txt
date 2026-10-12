cmake_minimum_required(VERSION 3.22)

# DEV shell (phase 3): native arm64 for speed. Pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Tutkal VERSION 1.0.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF: the build never writes into ~/Library/Audio/Plug-Ins.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE is fetched by CMake. To build offline against a checkout already on disk,
# pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_TUTKAL_FORMATS VST3 AU)
else()
    set(SVL_TUTKAL_FORMATS VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_TUTKAL_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_TUTKAL_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Tutkal
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Ttkl
    PLUGIN_NAME "TUTKAL"
    PRODUCT_NAME "TUTKAL"
    BUNDLE_ID com.subverselab.tutkal
    VERSION "1.0.3"
    FORMATS ${SVL_TUTKAL_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_TUTKAL_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
)

juce_generate_juce_header(Tutkal)

# The SubverseLab mark and wordmark (cream on transparent) and the panel typefaces (TeX Gyre Heros, DejaVu Sans Mono),
# compiled in so no text on the panel depends on a font installed on the machine.
juce_add_binary_data(TutkalAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(TutkalAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(TUTKAL_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_sources(Tutkal PRIVATE ${TUTKAL_SOURCES})

set(TUTKAL_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Tutkal PUBLIC ${TUTKAL_DEFS})

target_link_libraries(Tutkal
    PRIVATE
        TutkalAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# plugin_check: offline test of the real processor against the raw engine.
set(TUTKAL_TOOL_DEFS ${TUTKAL_DEFS}
    JucePlugin_Name="TUTKAL" JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
juce_add_console_app(plugin_check PRODUCT_NAME "plugin_check")
juce_generate_juce_header(plugin_check)
target_sources(plugin_check PRIVATE tests/plugin_check.cpp ${TUTKAL_SOURCES})
target_compile_definitions(plugin_check PRIVATE ${TUTKAL_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(plugin_check PRIVATE
    TutkalAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ui_snapshot: draws the editor to PNGs without a screen (docs/ui/front_glue.png, front_la.png, front_pump.png, rear.png)
# after real programme has gone through the processor.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${TUTKAL_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${TUTKAL_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE
    TutkalAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# host_check: loads the INSTALLED bundles (what a buyer has) as a DAW does and turns the knobs (2026-10-12). Read-only.
juce_add_console_app(host_check PRODUCT_NAME "host_check")
juce_generate_juce_header(host_check)
target_sources(host_check PRIVATE tests/host_check.cpp)
target_compile_definitions(host_check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(host_check PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_core
    juce::juce_recommended_config_flags)
