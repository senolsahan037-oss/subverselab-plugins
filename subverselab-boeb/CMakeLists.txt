cmake_minimum_required(VERSION 3.22)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Boeb VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The engine's raw render tool (plain C++, no JUCE): tools/measure.py runs it.
add_executable(boebraw tools/boebraw.cpp)
target_compile_options(boebraw PRIVATE -Wall -Wextra -Wshadow -O2)

# P3: the plug-in. Off by default so the engine build stays JUCE-free; build it in its own folder:
#   cmake -S . -B build-plugin -DBOEB_PLUGIN=ON -DFETCHCONTENT_SOURCE_DIR_JUCE=<JUCE 9.0.1 checkout>
option(BOEB_PLUGIN "Build the JUCE plug-in and its snapshot tool" OFF)
option(SVL_COPY_PLUGINS "Copy built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

if(BOEB_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.1)
    FetchContent_MakeAvailable(JUCE)

    if(APPLE)
        set(BOEB_FORMATS VST3 AU)
    else()
        set(BOEB_FORMATS VST3)
    endif()
    if(APPLE AND SVL_COPY_PLUGINS)
        set(BOEB_COPY TRUE)
    else()
        set(BOEB_COPY FALSE)
    endif()

    juce_add_plugin(Boeb
        COMPANY_NAME "SubverseLab"
        PLUGIN_MANUFACTURER_CODE Svlb
        PLUGIN_CODE Boeb
        PLUGIN_NAME "BOE-B"
        PRODUCT_NAME "BOE-B"
        BUNDLE_ID com.subverselab.boeb
        VERSION "1.0.0"
        FORMATS ${BOEB_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD ${BOEB_COPY}
        VST3_CATEGORIES Fx Dynamics
        AU_MAIN_TYPE kAudioUnitType_Effect)

    juce_generate_juce_header(Boeb)

    juce_add_binary_data(BoebAssets SOURCES
        Assets/Images/portrait_ink.png Assets/Images/mark_ink.png Assets/Images/wordmark_ink.png
        Assets/Fonts/Oswald-Bold.ttf Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
        Assets/Fonts/DejaVuSansMono-Bold.ttf)
    set_target_properties(BoebAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set(BOEB_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_sources(Boeb PRIVATE ${BOEB_SOURCES})
    set(BOEB_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_compile_definitions(Boeb PUBLIC ${BOEB_DEFS})
    target_link_libraries(Boeb
        PRIVATE BoebAssets juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

    # Headless snapshot of the editor (and a host-like run of a take through the processor).
    juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
    juce_generate_juce_header(ui_snapshot)
    target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${BOEB_SOURCES})
    target_compile_definitions(ui_snapshot PRIVATE ${BOEB_DEFS}
        JucePlugin_Name="BOE-B" JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ui_snapshot PRIVATE BoebAssets
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics juce::juce_graphics juce::juce_core
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
