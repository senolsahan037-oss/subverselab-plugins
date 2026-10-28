cmake_minimum_required(VERSION 3.22)

# TORNA: the family's graphic equaliser (Kenwood GE-1100 model). Native arm64 for the dev build; pass
# -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for the universal release build.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

project(Torna VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SVL_PLUGIN_VERSION "1.0.0" CACHE STRING "Numeric plug-in version")
# OFF: the build never writes into ~/Library/Audio/Plug-Ins.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE 9.0.1. Offline: -DFETCHCONTENT_SOURCE_DIR_JUCE=../subverselab-seda/build-tester/_deps/juce-src
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.1)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_TORNA_FORMATS Standalone VST3 AU)
else()
    set(SVL_TORNA_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_TORNA_COPY TRUE)
else()
    set(SVL_TORNA_COPY FALSE)
endif()

juce_add_plugin(Torna
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Torn
    PLUGIN_NAME "TORNA"
    PRODUCT_NAME "TORNA"
    BUNDLE_ID com.subverselab.torna
    VERSION "${SVL_PLUGIN_VERSION}"
    FORMATS ${SVL_TORNA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_TORNA_COPY}
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)
juce_generate_juce_header(Torna)

juce_add_binary_data(TornaAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(TornaAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(TORNA_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_sources(Torna PRIVATE ${TORNA_SOURCES})
set(TORNA_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Torna PUBLIC ${TORNA_DEFS})
target_link_libraries(Torna
    PRIVATE TornaAssets juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

set(TORNA_TOOL_DEFS ${TORNA_DEFS} JucePlugin_Name="TORNA" JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(TORNA_TOOL_LIBS TornaAssets juce::juce_dsp juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# plugin_check: the real processor and editor against the engine: parameters, latency, state, switches, LINK, memories,
# glide, double-click target, bus gates through the processor (tests/plugin_check.cpp).
juce_add_console_app(plugin_check PRODUCT_NAME "plugin_check")
juce_generate_juce_header(plugin_check)
target_sources(plugin_check PRIVATE tests/plugin_check.cpp ${TORNA_SOURCES})
target_compile_definitions(plugin_check PRIVATE ${TORNA_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(plugin_check PRIVATE ${TORNA_TOOL_LIBS})

# ui_snapshot: draws the editor to PNGs without a screen, after real audio has gone through the processor.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${TORNA_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${TORNA_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 TORNA_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ui_snapshot PRIVATE ${TORNA_TOOL_LIBS})
