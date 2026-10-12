cmake_minimum_required(VERSION 3.22)

project(SubverseSplitter VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch JUCE SDK
message(STATUS "Fetching JUCE Framework...")
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG develop
)
FetchContent_MakeAvailable(JUCE)

# In-app licence gate: OFF. The family protects 1.0 with the download gate (buy on subverselab.com, download from /account);
# in-plugin keys come with 1.1 (owner, 12 Oct: "bizim pluginlerimizde lisans aktivasyon alanı yok"). The code stays for then.
option(SVL_LICENSING "Build the licence-activation gate into ELEK (1.1)" OFF)

# Create the plugin target
juce_add_plugin(SubverseSplitter
    COMPANY_NAME "SubverseLab"
    PLUGIN_NAME "ELEK"
    PRODUCT_NAME "ELEK"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Elek
    BUNDLE_ID com.subverselab.elek
    VERSION "1.0.0"
    FORMATS Standalone VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE   # ELEK: no auto-install into ~/Library/Audio/Plug-Ins; installs are reported by hand
    # the SubverseLab app-icon standard (plugin-common docs/APP_ICON_STANDARD.md, tools/app_icon/make_app_icon.py --name ELEK)
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon/AppIcon_1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon/AppIcon_1024.png"
    COMPANY_WEBSITE "https://subverselab.com"
    COMPANY_COPYRIGHT "Copyright 2026 SubverseLab"
    VST3_CATEGORIES Fx Tools
    MICROPHONE_PERMISSION_ENABLED FALSE                                    # the app plays files; it never opens a microphone
    DOCUMENT_EXTENSIONS wav aif aiff mp3 flac                             # Finder "Open With" and drops on the Dock icon
)

juce_generate_juce_header(SubverseSplitter)


# Link ONNX Runtime directory
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps/onnxruntime")

if(WIN32)
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
    set(ONNXRUNTIME_DLL "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll")
else()
    set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/libonnxruntime.dylib")
endif()

# Binary Data
# Only the header mark. `background.png` and `loading_workers.png` were also
# compiled in and loaded into an Image at startup, and neither was ever drawn —
# 1.1 MB of AI-generated cartoon artwork carried by every build for nothing.
# The background is painted from Theme.h and the loading state is animated in
# code, so there was never anything for them to do.
juce_add_binary_data(SubverseSplitter_Assets
    SOURCES
        Resources/mark_cream.png
        Resources/wordmark_cream.png
        Resources/Fonts/texgyreheros-bold.otf
        Resources/Fonts/texgyreheros-regular.otf
)

# Add source files
target_sources(SubverseSplitter PRIVATE
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/Analysis.h
    Source/Analysis.cpp
    Source/Theme.h
    Source/ElekLibrary.h
    Source/StandaloneApp.cpp
    Source/LicenseConfig.h
)

target_link_libraries(SubverseSplitter PRIVATE SubverseSplitter_Assets)

target_include_directories(SubverseSplitter PRIVATE
    ${ONNXRUNTIME_DIR}/include
)

# Link JUCE core modules and ONNX Runtime
target_link_libraries(SubverseSplitter PRIVATE
    # juce_dsp carries the FFT and the windowing functions that Analysis.cpp
    # uses for the chroma and the onset envelope. It was not linked before
    # because nothing in the plugin analysed audio — only separated it.
    juce::juce_dsp
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_events
    juce::juce_core
    ${ONNXRUNTIME_LIB}
)

if(WIN32)
    # onnxruntime.dll is delay-loaded and then loaded by full path from ELEK's own folder (PluginProcessor.cpp, elekLoadOrt): a DAW
    # never searches a VST3's folder for its DLLs, and System32 may hold Windows ML's older onnxruntime.dll. ORT_API_MANUAL_INIT
    # keeps the ORT C++ header from calling into the DLL at static-initialisation time, before it is loaded.
    target_compile_definitions(SubverseSplitter PUBLIC ORT_API_MANUAL_INIT)
    target_link_options(SubverseSplitter INTERFACE "/DELAYLOAD:onnxruntime.dll")
    target_link_libraries(SubverseSplitter INTERFACE delayimp)

    # Copy ONNX Runtime DLL and model to executable directory
    add_custom_command(TARGET SubverseSplitter_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:SubverseSplitter_Standalone>"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Resources/htdemucs.onnx" "$<TARGET_FILE_DIR:SubverseSplitter_Standalone>"
    )
    add_custom_command(TARGET SubverseSplitter_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:SubverseSplitter_VST3>"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Resources/htdemucs.onnx" "$<TARGET_FILE_DIR:SubverseSplitter_VST3>"
    )
endif()

# Copy the huge ONNX model next to the built binary to avoid 10-minute compilation times
# The model has to land inside each format's bundle, because that is where the
# plugin looks for it at runtime.
#
# This used to copy it next to the shared-code library instead — a directory no
# format ever searches — so every search path missed and separation failed for
# everyone. It went unnoticed because loadModel() ended with an absolute path
# into one developer's home directory, and on that one machine the fallback
# always hit. Nowhere else could it have worked.
#
# Three copies of a 302 MB file per build is not cheap. Copying it to fewer
# would mean shipping a format that cannot separate, which is the bug this
# replaces.
foreach(format Standalone VST3 AU)
    if(TARGET SubverseSplitter_${format})
        add_custom_command(TARGET SubverseSplitter_${format} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:SubverseSplitter_${format}>/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/htdemucs.onnx"
                "$<TARGET_BUNDLE_CONTENT_DIR:SubverseSplitter_${format}>/Resources/htdemucs.onnx"
            COMMENT "Copying htdemucs.onnx into the ${format} bundle"
        )
    endif()
endforeach()

# ONNX Runtime ships INSIDE every macOS bundle (Contents/Frameworks), found through @loader_path. Before 12 Oct the binaries
# carried an absolute rpath into this project's _deps folder and no copy of the library: on any other Mac ELEK would not start
# ("Library not loaded: @rpath/libonnxruntime.1.18.0.dylib"). Bundles are re-signed ad hoc after the copy (the release script
# signs them again with Developer ID).
if(APPLE)
    file(GLOB ORT_REAL_DYLIB "${ONNXRUNTIME_DIR}/lib/libonnxruntime.*.*.*.dylib")
    foreach(format Standalone VST3 AU)
        if(TARGET SubverseSplitter_${format})
            set_target_properties(SubverseSplitter_${format} PROPERTIES
                SKIP_BUILD_RPATH OFF
                BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "@loader_path/../Frameworks")
            add_custom_command(TARGET SubverseSplitter_${format} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SubverseSplitter_${format}>/Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_REAL_DYLIB}" "$<TARGET_BUNDLE_CONTENT_DIR:SubverseSplitter_${format}>/Frameworks/"
                COMMAND codesign --force -s - "$<TARGET_BUNDLE_CONTENT_DIR:SubverseSplitter_${format}>/Frameworks/libonnxruntime.1.18.0.dylib"
                COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:SubverseSplitter_${format}>"
                COMMENT "ONNX Runtime -> ${format} bundle Frameworks"
                VERBATIM)
        endif()
    endforeach()
endif()

# The Dock icon macOS 26+ draws without the grey plate: Assets.car + CFBundleIconName, copied in after each Standalone build
include("${CMAKE_CURRENT_SOURCE_DIR}/../subverselab-plugin-common/cmake/SvlAppIcon.cmake")
svl_app_icon(SubverseSplitter "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon")


# Optimization definitions
# ELEK's own desktop window (Source/StandaloneApp.cpp), as MIHENK: native title bar, output only, files from the Finder
target_compile_definitions(SubverseSplitter PUBLIC JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
target_compile_definitions(SubverseSplitter PRIVATE
    SVL_LICENSING=$<BOOL:${SVL_LICENSING}>
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# Test target to verify WAV writing functionality
add_executable(TestWavWriter test_wav_writer.cpp)
target_link_libraries(TestWavWriter PRIVATE
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_events
    juce::juce_graphics
)

# Test target to verify MP3 reading functionality
add_executable(TestModel test_model.cpp)
target_include_directories(TestModel PRIVATE ${ONNXRUNTIME_DIR}/include)
target_link_libraries(TestModel PRIVATE
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_events
    ${ONNXRUNTIME_LIB}
)


# A console check for the tempo and key estimator, kept out of the plugin
# target. Verified against the web Splitter's reading of the same file rather
# than against its own output.
juce_add_console_app(test_analysis PRODUCT_NAME "test_analysis")
juce_generate_juce_header(test_analysis)
target_sources(test_analysis PRIVATE test_analysis.cpp Source/Analysis.cpp)
target_compile_definitions(test_analysis PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(test_analysis PRIVATE
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_core
)


# The window plan, checked without the model. Header-only, so this needs no
# JUCE and no ONNX Runtime.
add_executable(test_overlap test_overlap.cpp)
target_include_directories(test_overlap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})


# Renders the editor offscreen so its layout can be looked at without a screen
# and without Screen Recording permission. Links the plugin's shared code.
juce_add_console_app(test_ui_snapshot PRODUCT_NAME "test_ui_snapshot")
juce_generate_juce_header(test_ui_snapshot)
target_sources(test_ui_snapshot PRIVATE
    test_ui_snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Analysis.cpp
)
target_include_directories(test_ui_snapshot PRIVATE ${ONNXRUNTIME_DIR}/include)
if(WIN32)   # it compiles Source/PluginProcessor.cpp itself: same ORT loading as the plug-in
    target_compile_definitions(test_ui_snapshot PRIVATE ORT_API_MANUAL_INIT)
    target_link_options(test_ui_snapshot PRIVATE "/DELAYLOAD:onnxruntime.dll")
    target_link_libraries(test_ui_snapshot PRIVATE delayimp)
endif()
target_compile_definitions(test_ui_snapshot PRIVATE
    SVL_LICENSING=$<BOOL:${SVL_LICENSING}>
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="ELEK"
    JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0 JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
)
target_link_libraries(test_ui_snapshot PRIVATE
    SubverseSplitter_Assets
    juce::juce_dsp juce::juce_audio_utils juce::juce_audio_processors
    juce::juce_gui_extra juce::juce_gui_basics juce::juce_graphics
    juce::juce_events juce::juce_core
    ${ONNXRUNTIME_LIB}
)

# elek_host_test: loads a built ELEK.vst3 as a DAW does, runs one separation through SPLITTER_AUTOTEST and checks the stems (2026-10-12,
# first Windows build: proves the delay-loaded onnxruntime.dll path inside a host). Not part of the product.
juce_add_console_app(elek_host_test PRODUCT_NAME "elek_host_test")
juce_generate_juce_header(elek_host_test)
target_sources(elek_host_test PRIVATE elek_host_test.cpp)
target_compile_definitions(elek_host_test PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(elek_host_test PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics juce::juce_core)
