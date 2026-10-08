cmake_minimum_required(VERSION 3.22)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Taban VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SVL_COPY_PLUGINS "Copy built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_TABAN_FORMATS VST3 AU)
else()
    set(SVL_TABAN_FORMATS VST3)
endif()

if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_TABAN_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_TABAN_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Taban
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Tabn
    PLUGIN_NAME "TABAN"
    PRODUCT_NAME "TABAN"
    BUNDLE_ID com.subverselab.taban
    VERSION "1.0.0"
    FORMATS ${SVL_TABAN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_TABAN_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
)

juce_generate_juce_header(Taban)

juce_add_binary_data(TabanAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(TabanAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(TABAN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_sources(Taban PRIVATE ${TABAN_SOURCES})

set(TABAN_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Taban PUBLIC ${TABAN_DEFS})

target_link_libraries(Taban
    PRIVATE
        TabanAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Tools: the engine self-test (plain C++) and a headless snapshot of the editor.
set(TABAN_TOOL_DEFS ${TABAN_DEFS}
    JucePlugin_Name="TABAN"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

add_executable(test_taban_engine tests/test_taban_engine.cpp)

juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${TABAN_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${TABAN_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE TabanAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
