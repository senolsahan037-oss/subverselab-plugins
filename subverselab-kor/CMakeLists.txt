cmake_minimum_required(VERSION 3.22)

# DEV build: native arm64 for speed. Pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal release.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Kor VERSION 1.0.1 LANGUAGES C CXX)

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
    set(SVL_KOR_FORMATS VST3 AU)
else()
    set(SVL_KOR_FORMATS VST3)
endif()
if(APPLE AND SVL_COPY_PLUGINS)
    set(SVL_KOR_COPY_AFTER_BUILD TRUE)
else()
    set(SVL_KOR_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Kor
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Alev
    PLUGIN_NAME "ALEV"
    PRODUCT_NAME "ALEV"
    BUNDLE_ID com.subverselab.alev
    VERSION "1.0.1"
    FORMATS ${SVL_KOR_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SVL_KOR_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect
)

juce_generate_juce_header(Kor)

# The SubverseLab wordmark (cream on transparent) and the panel typefaces, compiled in so no text on the panel
# depends on a font installed on the machine.
juce_add_binary_data(KorAssets SOURCES
    Assets/mark_cream.png Assets/wordmark_cream.png
    Assets/Fonts/texgyreheros-bold.otf Assets/Fonts/texgyreheros-regular.otf
    Assets/Fonts/DejaVuSansMono-Bold.ttf)
set_target_properties(KorAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KOR_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_sources(Kor PRIVATE ${KOR_SOURCES})

set(KOR_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(Kor PUBLIC ${KOR_DEFS})

target_link_libraries(Kor
    PRIVATE
        KorAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ui_snapshot: draws the panel to PNGs without a screen (docs/ui/kor_*.png) after real programme has gone through
# the real processor, and checks the controls' hit areas, resets, size and colours.
set(KOR_TOOL_DEFS ${KOR_DEFS}
    JucePlugin_Name="ALEV"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${KOR_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${KOR_TOOL_DEFS})
target_link_libraries(ui_snapshot PRIVATE
    KorAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ui_video: the demo video frames (real audio, real panel, scripted ring moves). Not part of the plug-in.
juce_add_console_app(ui_video PRODUCT_NAME "ui_video")
juce_generate_juce_header(ui_video)
target_sources(ui_video PRIVATE tests/ui_video.cpp ${KOR_SOURCES})
target_compile_definitions(ui_video PRIVATE ${KOR_TOOL_DEFS})
target_link_libraries(ui_video PRIVATE
    KorAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags)

# param_text_test: type-in text of every float parameter (tests/SvlParamTextTests.h) plus literal cases. Exit code = failures.
juce_add_console_app(param_text_test PRODUCT_NAME "param_text_test")
juce_generate_juce_header(param_text_test)
target_sources(param_text_test PRIVATE tests/param_text_test.cpp ${KOR_SOURCES})
target_compile_definitions(param_text_test PRIVATE ${KOR_TOOL_DEFS})
target_link_libraries(param_text_test PRIVATE
    KorAssets
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
    juce::juce_gui_basics juce::juce_graphics juce::juce_core
    juce::juce_recommended_config_flags)
