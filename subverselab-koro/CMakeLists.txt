cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Koro VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# KORO is a paid product, but the shell builds with the licence gate OFF,
# exactly like KUBBE 1.0.1. The seam stays: -DSVL_LICENSING=ON compiles the
# Lemon Squeezy / SVL gate from subverselab-plugin-common/Licensing back in.
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

# The build never installs anything. tools/validate_mac.sh --copy puts ONLY the
# AU into ~/Library/Audio/Plug-Ins/Components for auval (and --remove takes it
# out again); -DSVL_COPY_PLUGINS=ON restores JUCE's copy-after-build.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)
if(APPLE)
    set(SVL_KORO_FORMATS Standalone VST3 AU)
else()
    set(SVL_KORO_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_KORO_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_KORO_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Koro
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Koro
    PLUGIN_NAME "KORO"
    PRODUCT_NAME "KORO"
    BUNDLE_ID com.subverselab.koro
    VERSION "1.0.1"
    FORMATS ${SVL_KORO_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_KORO_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx "Pitch Shift"
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Koro)

# The SubverseLab mark and wordmark, cream on transparent (the KUBBE files).
# The panel typefaces (TeX Gyre Heros, DejaVu Sans Mono) are compiled in too, so no text
# on the panel depends on a font installed on the machine (Windows has no Helvetica Neue / Menlo).
juce_add_binary_data(KoroAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(KoroAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KORO_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_sources(Koro PRIVATE ${KORO_SOURCES})

set(KORO_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SVL_LICENSING=$<BOOL:${SVL_LICENSING}>)
target_compile_definitions(Koro PUBLIC ${KORO_DEFS})

target_link_libraries(Koro
    PRIVATE
        KoroAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

set(KORO_TOOL_DEFS ${KORO_DEFS}
    JucePlugin_Name="KORO"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(KORO_TOOL_LIBS KoroAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_dsp juce::juce_gui_basics juce::juce_graphics juce::juce_core)

# render --selftest: the engine's own selftest (tools/engine_render.cpp, verbatim),
# then the plugin checks. render in.wav out.wav [--mood NAME] [--param id=value]...
juce_add_console_app(render PRODUCT_NAME "render")
juce_generate_juce_header(render)
target_sources(render PRIVATE tests/render.cpp ${KORO_SOURCES})
target_compile_definitions(render PRIVATE ${KORO_TOOL_DEFS})
target_link_libraries(render PRIVATE ${KORO_TOOL_LIBS})

# Draws the editor to PNGs without a screen: docs/ui/front.png, front_locked.png, rear.png.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${KORO_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${KORO_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE ${KORO_TOOL_LIBS})
