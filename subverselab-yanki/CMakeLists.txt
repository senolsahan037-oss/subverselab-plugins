cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Yanki VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF like KUBBE 1.0.1 (free launch, no activation gate). The licensing seam
# stays: build with -DSVL_LICENSING=ON to compile the Lemon Squeezy / SVL
# licence gate from ../subverselab-plugin-common/Licensing back in.
option(SVL_LICENSING "Build the Lemon Squeezy / SVL licence-activation gate into the plugin" OFF)

# OFF: the build never writes into ~/Library/Audio/Plug-Ins. Pass
# -DSVL_COPY_PLUGINS=ON (the documented "--copy") to let JUCE copy the built
# VST3/AU there after each build. tools/validate_mac.sh --copy copies only the
# AU (auval can only find registered components).
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
    set(SVL_YANKI_FORMATS Standalone VST3 AU)
else()
    set(SVL_YANKI_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_YANKI_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_YANKI_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Yanki
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Ynki
    PLUGIN_NAME "YANKI"
    PRODUCT_NAME "YANKI"
    BUNDLE_ID com.subverselab.yanki
    VERSION "1.0.1"
    FORMATS ${SVL_YANKI_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_YANKI_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Delay
    AU_MAIN_TYPE kAudioUnitType_Effect
    # The standalone app reads the audio input: macOS asks for microphone
    # access with this text, and the hardened runtime needs the entitlement.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Yanki)

# The SubverseLab mark and wordmark, cream on transparent, from
# Assets/Brand/logo (mark-only-cream.png, wordmark-only-cream.png: the files
# the approved concept generator draws).
# The panel typefaces (TeX Gyre Heros, DejaVu Sans Mono) are compiled in too, so no text
# on the panel depends on a font installed on the machine (Windows has no Helvetica Neue / Menlo).
juce_add_binary_data(YankiAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(YankiAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(YANKI_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PanelKit.cpp
)

target_sources(Yanki PRIVATE ${YANKI_SOURCES})

set(YANKI_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    SVL_LICENSING=$<BOOL:${SVL_LICENSING}>)
target_compile_definitions(Yanki PUBLIC ${YANKI_DEFS})

target_link_libraries(Yanki
    PRIVATE
        YankiAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plugin code built into the console tools below, with the macros the
# plugin wrapper would otherwise define.
set(YANKI_TOOL_DEFS ${YANKI_DEFS}
    JucePlugin_Name="YANKI"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(YANKI_TOOL_LIBS YankiAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_dsp juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# render: --selftest runs the engine's own selftest (the prototype harness,
# copied verbatim into tests/, compiled against Source/YankiEngine.h) and the
# plugin-layer checks; otherwise renders a WAV through the real processor.
juce_add_console_app(render PRODUCT_NAME "render")
juce_generate_juce_header(render)
target_sources(render PRIVATE tests/render.cpp tests/engine_render.cpp ${YANKI_SOURCES})
# The prototype harness is not plugin code: its own build is -Wall -Wextra
# clean; here its main() is renamed and JUCE's stricter set is not applied.
set_source_files_properties(tests/engine_render.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=yanki_engine_main"
    COMPILE_OPTIONS "-w")
target_compile_definitions(render PRIVATE ${YANKI_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(render PRIVATE ${YANKI_TOOL_LIBS})

# ui_snapshot: draws the editor (front and rear) to PNG without a screen.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${YANKI_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${YANKI_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE ${YANKI_TOOL_LIBS})

# hostsim: a host-like harness (message thread + audio thread + automation
# thread) for the "plugin ignores every change after a MIX move" bug.
juce_add_console_app(hostsim PRODUCT_NAME "hostsim")
juce_generate_juce_header(hostsim)
target_sources(hostsim PRIVATE tests/host_sim_main.cpp ${YANKI_SOURCES})
target_compile_definitions(hostsim PRIVATE ${YANKI_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(hostsim PRIVATE ${YANKI_TOOL_LIBS})

# hostsim_hosted: the same harness against the installed AU / VST3 through its wrapper.
juce_add_console_app(hostsim_hosted PRODUCT_NAME "hostsim_hosted")
juce_generate_juce_header(hostsim_hosted)
target_sources(hostsim_hosted PRIVATE tests/host_sim_hosted.cpp)
target_compile_definitions(hostsim_hosted PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_AU=1 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(hostsim_hosted PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_core juce::juce_recommended_config_flags)
