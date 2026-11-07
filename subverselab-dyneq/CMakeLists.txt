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

project(Dyneq VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF in P3 (the licensing seam is Source/LicenseConfig.h with TODO ids, like YANKI): build with -DSVL_LICENSING=ON to compile the
# Lemon Squeezy / SVL licence gate from ../subverselab-plugin-common/Licensing in. Not exercised in P3.
option(SVL_LICENSING "Build the Lemon Squeezy / SVL licence-activation gate into the plugin" OFF)

# OFF: the build never writes into ~/Library/Audio/Plug-Ins.
# Plug-in version (numeric, AU rule). Default unchanged; the tester build passes -DSVL_PLUGIN_VERSION=0.9.0 ("test1" lives only in the file names).
set(SVL_PLUGIN_VERSION "1.0.0" CACHE STRING "Numeric plug-in version")   # 1.0.0: the release (owner 8 Nov "15 dolar yazalım imzala")

option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

# JUCE is fetched by CMake. To build offline against a checkout already on disk, pass
# -DFETCHCONTENT_SOURCE_DIR_JUCE=/Users/senolsahan/Desktop/SubverseLab-Launchpad/01_AI_Tools/subverselab-splitter-desktop/build/_deps/juce-src
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(SVL_DYNEQ_FORMATS Standalone VST3 AU)
else()
    set(SVL_DYNEQ_FORMATS Standalone VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_DYNEQ_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_DYNEQ_COPY_AFTER_BUILD FALSE)
endif()

# The owner named it ADALET (10 Oct). 11 Oct: the name a host shows is ADALET; the plug-in code (Dyeq / Svlb) and the bundle id stay, so a host keeps
# every saved instance (AU and VST3 identify a plug-in by its codes, not its name). The CMake target and the state tag keep the working name DYNEQ.
juce_add_plugin(Dyneq
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Dyeq
    PLUGIN_NAME "ADALET"
    PRODUCT_NAME "ADALET"
    BUNDLE_ID com.subverselab.dyneq
    VERSION "${SVL_PLUGIN_VERSION}"
    FORMATS ${SVL_DYNEQ_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_DYNEQ_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    # The standalone app reads the audio input: macOS asks for microphone access with this text.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "The standalone app processes the audio coming into your input device."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
)

juce_generate_juce_header(Dyneq)

# The SubverseLab mark (icon only, cream on transparent, from the brand kit) and the typefaces (TeX Gyre Heros, DejaVu Sans Mono), compiled in.
juce_add_binary_data(DyneqAssets SOURCES
    Assets/mark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/texgyreheros-italic.otf Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(DyneqAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(DYNEQ_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_sources(Dyneq PRIVATE ${DYNEQ_SOURCES})

set(DYNEQ_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SVL_LICENSING=$<BOOL:${SVL_LICENSING}>)
target_compile_definitions(Dyneq PUBLIC ${DYNEQ_DEFS})

target_link_libraries(Dyneq
    PRIVATE
        DyneqAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Plug-in code built into the console tools below, with the macros the plugin wrapper would otherwise define.
set(DYNEQ_TOOL_DEFS ${DYNEQ_DEFS}
    JucePlugin_Name="ADALET"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(DYNEQ_TOOL_LIBS DyneqAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# plugin_check: the real processor against the raw engine, BAKE, undo, control proof (tests/plugin_check.cpp).
juce_add_console_app(plugin_check PRODUCT_NAME "plugin_check")
juce_generate_juce_header(plugin_check)
target_sources(plugin_check PRIVATE tests/plugin_check.cpp ${DYNEQ_SOURCES})
target_compile_definitions(plugin_check PRIVATE ${DYNEQ_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(plugin_check PRIVATE ${DYNEQ_TOOL_LIBS})

# ui_snapshot: draws the editor to PNGs without a screen, after real programme has gone through the processor (the P4 harness).
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${DYNEQ_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${DYNEQ_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_snapshot PRIVATE ${DYNEQ_TOOL_LIBS})

# dyneqraw: offline renderer around Source/DyneqEngine.h (no JUCE: the engine and the WAV reader are plain C++).
add_executable(dyneqraw tools/dyneqraw.cpp)
target_compile_options(dyneqraw PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O3 -Wall -Wextra>)
