cmake_minimum_required(VERSION 3.22)

# SVL milestone 1: the plain C++ engine and its raw render / selftest tool. No JUCE yet (milestone 5 adds the plug-in target).
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

project(SVL VERSION 1.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# svl: render a preset + note list to a 24-bit WAV, or run --selftest.
add_executable(svl tools/svl.cpp)
target_include_directories(svl PRIVATE Source/engine)
if(MSVC)
    target_compile_options(svl PRIVATE /W4)
else()
    target_compile_options(svl PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- milestone 5: the JING FANG plug-in (Standalone + VST3 + AU)
# Built against JUCE 9.0.1. Offline: -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE (the sahne build keeps a copy).
# Configure with -DSVL_BUILD_PLUGIN=OFF to build only the raw tool.
option(SVL_BUILD_PLUGIN "Build the JING FANG plug-in and its offline checks" ON)
# OFF: the build never writes into ~/Library/Audio/Plug-Ins.
option(SVL_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" OFF)

if(SVL_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.1)
    FetchContent_MakeAvailable(JUCE)

    if(APPLE)
        set(JF_FORMATS Standalone VST3 AU)
    else()
        set(JF_FORMATS Standalone VST3)
    endif()
    if(APPLE AND SVL_COPY_PLUGINS)
        set(JF_COPY TRUE)
    else()
        set(JF_COPY FALSE)
    endif()

    juce_add_plugin(JingFang
        COMPANY_NAME "SubverseLab"
        PLUGIN_MANUFACTURER_CODE Svlb
        PLUGIN_CODE Jfsy
        PLUGIN_NAME "JING FANG"
        PRODUCT_NAME "JING FANG"
        BUNDLE_ID com.subverselab.jingfang
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Icon/jingfang_icon_1024.png"    # the dock icon (tools/make_icon.py)
        ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Icon/jingfang_icon_1024.png"
        VERSION "${PROJECT_VERSION}"
        FORMATS ${JF_FORMATS}
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE   # a host keeps its keys; the keyboard asks for focus when it is clicked, the standalone at once
        COPY_PLUGIN_AFTER_BUILD ${JF_COPY}
        VST3_CATEGORIES Instrument Synth
        AU_MAIN_TYPE kAudioUnitType_MusicDevice)
    juce_generate_juce_header(JingFang)

    # Panel typefaces and the SubverseLab marks, compiled in.
    juce_add_binary_data(JingFangAssets HEADER_NAME BinaryData.h NAMESPACE BinaryData SOURCES
        Assets/mark_cream.png Assets/wordmark_cream.png
        Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf Assets/Fonts/DejaVuSansMono-Bold.ttf
        Assets/Panel/panel_v3_2x.png Assets/Panel/knob_green.png Assets/Panel/layout_v3.json Assets/Panel/dragon_overlay.png
        Assets/Panel/doors.json Assets/Panel/door_patch_2x.png Assets/Panel/door_seq_2x.png Assets/Panel/door_lab_2x.png Assets/Panel/door_drum_2x.png
        Assets/Panel/seq/switch_up.png Assets/Panel/seq/switch_mid.png Assets/Panel/seq/switch_down.png
        Assets/Panel/seq/led_off.png Assets/Panel/seq/led_on.png Assets/Panel/seq/chicken.png
        Assets/Panel/seq/jack.png Assets/Panel/seq/step_off.png Assets/Panel/seq/step_on.png Assets/Panel/seq/step_acc.png)
    set_target_properties(JingFangAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Factory presets: the same XML files `svl render` reads (presets/<family>/*.xml), compiled in.
    file(GLOB_RECURSE JF_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/*.xml")
    juce_add_binary_data(JingFangPresets HEADER_NAME FactoryPresets.h NAMESPACE FactoryPresets SOURCES ${JF_PRESET_FILES})
    set_target_properties(JingFangPresets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set(JF_SOURCES Source/plugin/PluginProcessor.cpp Source/plugin/PluginEditor.cpp Source/plugin/Ui.cpp Source/plugin/Keyboard.cpp
        Source/plugin/Dragon.cpp Source/plugin/PatchBay.cpp Source/plugin/Scopes.cpp Source/plugin/AnalogDoor.cpp Source/plugin/Analog.cpp Source/plugin/MidiClip.cpp
    Source/plugin/LabDoor.cpp Source/plugin/FxDoor.cpp)
    target_sources(JingFang PRIVATE ${JF_SOURCES})
    target_include_directories(JingFang PRIVATE Source/engine Source/plugin)
    set(JF_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_compile_definitions(JingFang PUBLIC ${JF_DEFS})
    target_link_libraries(JingFang
        PRIVATE JingFangAssets JingFangPresets juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
    if(APPLE)
        target_link_libraries(JingFang PRIVATE "-framework Carbon")   # HIToolbox: the keyboard layout (Keyboard.cpp)
    endif()

    # Offline checks built against the same sources (no host, no screen).
    set(JF_TOOL_DEFS ${JF_DEFS} JucePlugin_Name="JING FANG" JucePlugin_VersionString="${PROJECT_VERSION}"
        JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
    function(jf_tool name)
        juce_add_console_app(${name} PRODUCT_NAME "${name}")
        juce_generate_juce_header(${name})
        target_sources(${name} PRIVATE tests/${name}.cpp ${JF_SOURCES})
        target_include_directories(${name} PRIVATE Source/engine Source/plugin)
        target_compile_definitions(${name} PRIVATE ${JF_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
        target_link_libraries(${name} PRIVATE JingFangAssets JingFangPresets
            juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
        if(APPLE)
            target_link_libraries(${name} PRIVATE "-framework Carbon")
        endif()
    endfunction()
    # plugin_check: the processor against the raw engine path (`svl render`), state, presets, keyswitch write-back.
    jf_tool(plugin_check)
    # ui_snapshot: draws the panel to PNGs without a screen and runs the panel checks (glide, double-click, help, lockup, keyboard).
    jf_tool(ui_snapshot)
    # demo_render: the ads, rendered from the plug-in itself (frames + audio, no screen capture)
    jf_tool(demo_render)
    # preset_storm: bug 1 of 20 Oct (fast preset / set / slot switching while the host plays left the channel silent)
    jf_tool(preset_storm)
    # rack_capture / rack_render (work order 1.x item 6 preparation): host Reason Rack Plugin as a local measurement reference
    set(JF_RACK_DEFS JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="rack" JUCE_APPLICATION_VERSION_STRING="1.0.0")
    foreach(name rack_capture rack_render)
        if(name STREQUAL rack_capture)
            juce_add_gui_app(${name} PRODUCT_NAME "${name}")
        else()
            juce_add_console_app(${name} PRODUCT_NAME "${name}")
        endif()
        juce_generate_juce_header(${name})
        target_sources(${name} PRIVATE tests/${name}.cpp)
        target_compile_definitions(${name} PRIVATE ${JF_RACK_DEFS})
        target_link_libraries(${name} PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
            juce::juce_recommended_config_flags)
    endforeach()
endif()
