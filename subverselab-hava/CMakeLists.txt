cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Hava VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF, as KUBBE 1.0.1 ships: no activation gate, no licence checks, network
# calls or licence files. The licensing code still compiles behind this switch
# for a future paid phase: build with -DSVL_LICENSING=ON to bring it back.
option(SVL_LICENSING "Build the Lemon Squeezy / SVL licence-activation gate into the plugin" OFF)

# JUCE is fetched by CMake. To build offline against a checkout already on
# disk, pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

# Default ON (unchanged dev behaviour): JUCE copies the built plug-ins into ~/Library/Audio/Plug-Ins after each build.
# Release builds pass -DSVL_COPY_PLUGINS=OFF so nothing is installed.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" ON)

# AU is macOS-only; Windows builds VST3 + Standalone.
if(APPLE)
    set(SVL_HAVA_FORMATS Standalone VST3 AU)
    if(SVL_COPY_PLUGINS)
        set(SVL_HAVA_COPY_AFTER_BUILD TRUE)
    else()
        set(SVL_HAVA_COPY_AFTER_BUILD FALSE)
    endif()
else()
    set(SVL_HAVA_FORMATS Standalone VST3)
    set(SVL_HAVA_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Hava
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Hava
    PLUGIN_NAME "HAVA"
    PRODUCT_NAME "HAVA"
    BUNDLE_ID com.subverselab.hava
    VERSION "1.0.0"
    FORMATS ${SVL_HAVA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_HAVA_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Hava)

# The SubverseLab mark and wordmark, cream on transparent (the same files KUBBE ships).
# The panel typefaces (TeX Gyre Heros, DejaVu Sans Mono) are compiled in too, so no text
# on the panel depends on a font installed on the machine (Windows has no Helvetica Neue / Menlo).
juce_add_binary_data(HavaAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(HavaAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(HAVA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_sources(Hava PRIVATE ${HAVA_SOURCES})

set(HAVA_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SVL_LICENSING=$<BOOL:${SVL_LICENSING}>)
target_compile_definitions(Hava PUBLIC ${HAVA_DEFS})

target_link_libraries(Hava
    PRIVATE
        HavaAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plugin code built into the console tools below, with the macros the
# plugin wrapper would otherwise define.
set(HAVA_TOOL_DEFS ${HAVA_DEFS}
    JucePlugin_Name="HAVA"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(HAVA_TOOL_LIBS HavaAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_dsp juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# The engine's own selftest and WAV renderer, unchanged from the prototype
# (tools/render.cpp, tools/presets.h, tools/wav.h, tools/pluck.h are verbatim
# copies; they include ../Source/YayEngine.h, the plugin's copy of the engine).
add_executable(render tools/render.cpp)
target_compile_options(render PRIVATE -O2 -Wall -Wextra)

# Processor checks through the real plugin code path: bit-exact bypass,
# latency reporting, MASTER SAFE crossfade, mood recall, state round trip.
juce_add_console_app(plugin_check PRODUCT_NAME "plugin_check")
juce_generate_juce_header(plugin_check)
target_sources(plugin_check PRIVATE tests/plugin_check.cpp ${HAVA_SOURCES})
target_compile_definitions(plugin_check PRIVATE ${HAVA_TOOL_DEFS})
target_link_libraries(plugin_check PRIVATE ${HAVA_TOOL_LIBS})

# Draws the editor to PNGs without a screen.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${HAVA_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${HAVA_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE ${HAVA_TOOL_LIBS})
