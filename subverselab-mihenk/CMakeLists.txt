cmake_minimum_required(VERSION 3.22)

# Release builds run natively on Apple Silicon and Intel Macs.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# Oldest macOS the release supports (Big Sur, the first with Apple Silicon).
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(Mihenk VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link-time optimisation for the app and plug-ins (release). OFF makes development builds much faster.
option(MIHENK_LTO "Link-time optimisation for the app and the plug-ins" ON)

# JUCE is fetched by CMake. To build offline against a checkout already on
# disk, pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
)
FetchContent_MakeAvailable(JUCE)

# The engine: measurement, the offline chain, the real-time chain, export. No UI, no audio device.
set(MIHENK_ENGINE_SOURCES
    Source/dsp/Loudness.cpp
    Source/dsp/Spectrum.cpp
    Source/dsp/Equalizer.cpp
    Source/dsp/Dynamics.cpp
    Source/engine/MasterChain.cpp
    Source/engine/AudioIO.cpp
    Source/engine/Preset.cpp
    Source/engine/Diagnosis.cpp
    Source/engine/RtChain.cpp
    Source/engine/Profile.cpp
    Source/engine/Batch.cpp
    Source/engine/Proxy.cpp
    Source/engine/Plan.cpp
)

# The panel, the session (background renders) and the standalone player.
set(MIHENK_UI_SOURCES
    Source/app/Widgets.cpp
    Source/app/ProfilePanel.cpp
    Source/app/Panels.cpp
    Source/app/BatchPanel.cpp
    Source/app/LiveAnalysis.cpp
    Source/app/Player.cpp
    Source/app/Session.cpp
    Source/app/MainComponent.cpp
)

# The AudioProcessor (one product: standalone app, VST3, AU), its parameters, state and editor.
set(MIHENK_PLUGIN_SOURCES
    Source/plugin/Params.cpp
    Source/plugin/StateIO.cpp
    Source/plugin/PluginProcessor.cpp
    Source/plugin/PluginEditor.cpp
)

set(MIHENK_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
set(MIHENK_ENGINE_LIBS
    juce::juce_audio_formats juce::juce_audio_basics juce::juce_dsp juce::juce_core)

# The SubverseLab mark and wordmark, cream on transparent, from Assets/Brand/logo.
juce_add_binary_data(MihenkAssets SOURCES Assets/mark_cream.png Assets/wordmark_cream.png)
set_target_properties(MihenkAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

# One product from one target: the desktop app, the VST3 and (on macOS) the AU. Same manufacturer code as the
# SubverseLab siblings (KASET, KUBBE). AU is macOS-only; Windows builds VST3 + Standalone.
if(APPLE)
    set(MIHENK_FORMATS Standalone VST3 AU)
    option(MIHENK_COPY_PLUGINS "Copy the built plug-ins into ~/Library/Audio/Plug-Ins after each build" ON)
    set(MIHENK_COPY_AFTER_BUILD ${MIHENK_COPY_PLUGINS})
else()
    set(MIHENK_FORMATS Standalone VST3)
    set(MIHENK_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(Mihenk
    COMPANY_NAME "SubverseLab"
    PLUGIN_MANUFACTURER_CODE Svlb
    PLUGIN_CODE Mhnk
    PLUGIN_NAME "MIHENK"
    PRODUCT_NAME "MIHENK"
    BUNDLE_ID com.subverselab.mihenk
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/AppIcon_1024.png"
    VERSION "1.0.1"
    FORMATS ${MIHENK_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${MIHENK_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx Mastering Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    DOCUMENT_EXTENSIONS wav aif aiff
    # The app plays files and never opens an input, so it needs no microphone permission.
    MICROPHONE_PERMISSION_ENABLED FALSE
    HARDENED_RUNTIME_ENABLED TRUE
)
juce_generate_juce_header(Mihenk)
target_sources(Mihenk PRIVATE ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES} Source/plugin/StandaloneApp.cpp)
target_compile_definitions(Mihenk PUBLIC ${MIHENK_DEFS} JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
target_link_libraries(Mihenk
    PRIVATE
        MihenkAssets
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_gui_extra
        ${MIHENK_ENGINE_LIBS}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
if(MIHENK_LTO)
    target_link_libraries(Mihenk PUBLIC juce::juce_recommended_lto_flags)
endif()

# Plug-in code built into the console tools below, with the macros the plug-in wrapper would otherwise define.
set(MIHENK_TOOL_DEFS ${MIHENK_DEFS}
    JucePlugin_Name="MIHENK" JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
set(MIHENK_TOOL_LIBS MihenkAssets
    juce::juce_audio_utils juce::juce_audio_devices juce::juce_audio_processors ${MIHENK_ENGINE_LIBS}
    juce::juce_gui_extra juce::juce_recommended_config_flags)

# Measurement, chain, real-time chain and export checks (--selftest), --measure, --diagnose, batch --master.
juce_add_console_app(render PRODUCT_NAME "render")
juce_generate_juce_header(render)
target_sources(render PRIVATE tests/render.cpp ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(render PRIVATE ${MIHENK_DEFS})
target_link_libraries(render PRIVATE ${MIHENK_ENGINE_LIBS} juce::juce_gui_basics juce::juce_recommended_config_flags)

# The processor as a host drives it: LEARN through processBlock, offline vs real-time, state round trip, CPU.
juce_add_console_app(rt_check PRODUCT_NAME "rt_check")
juce_generate_juce_header(rt_check)
target_sources(rt_check PRIVATE tests/rt_check.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(rt_check PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(rt_check PRIVATE ${MIHENK_TOOL_LIBS})

# Personal profile (SPEC_V2 §7): learning on synthetic sessions, the store, locks in the app and the plug-in, real time.
juce_add_console_app(profile_check PRODUCT_NAME "profile_check")
juce_generate_juce_header(profile_check)
target_sources(profile_check PRIVATE tests/profile_check.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(profile_check PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(profile_check PRIVATE ${MIHENK_TOOL_LIBS})

# v2 usability (faces, AUTO knobs, reference, level-matched A/B, KEEP INPUT, batch queue) through the panel and the processor.
juce_add_console_app(ui_check PRODUCT_NAME "ui_check")
juce_generate_juce_header(ui_check)
target_sources(ui_check PRIVATE tests/ui_check.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(ui_check PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_check PRIVATE ${MIHENK_TOOL_LIBS})

# Draws the panel to a PNG, with synthetic audio loaded (and live meters simulated).
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
juce_generate_juce_header(ui_snapshot)
target_sources(ui_snapshot PRIVATE tests/ui_snapshot.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(ui_snapshot PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_snapshot PRIVATE ${MIHENK_TOOL_LIBS})

# Virtual user: the plug-in driven headless the way a producer uses it (bypass, activation level, finished material,
# automation, state recovery), with measured signals. Writes tests/behavior_report.txt and .json. Finds and reports; fixes nothing.
juce_add_console_app(behavior_check PRODUCT_NAME "behavior_check")
juce_generate_juce_header(behavior_check)
target_sources(behavior_check PRIVATE tests/behavior_check.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(behavior_check PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(behavior_check PRIVATE ${MIHENK_TOOL_LIBS})

# Loads the built VST3 / AU binaries through JUCE's plug-in hosting (a host-side smoke test; no GUI host needed).
juce_add_console_app(host_check PRODUCT_NAME "host_check")
juce_generate_juce_header(host_check)
target_sources(host_check PRIVATE tests/host_check.cpp)
target_compile_definitions(host_check PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(host_check PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_recommended_config_flags)

juce_add_console_app(plan_check PRODUCT_NAME "plan_check")
juce_generate_juce_header(plan_check)
target_sources(plan_check PRIVATE tests/plan_check.cpp ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(plan_check PRIVATE ${MIHENK_DEFS})
target_link_libraries(plan_check PRIVATE ${MIHENK_ENGINE_LIBS} juce::juce_gui_basics juce::juce_recommended_config_flags)

juce_add_console_app(plan_audit PRODUCT_NAME "plan_audit")
juce_generate_juce_header(plan_audit)
target_sources(plan_audit PRIVATE tests/plan_audit.cpp ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(plan_audit PRIVATE ${MIHENK_DEFS})
target_link_libraries(plan_audit PRIVATE ${MIHENK_ENGINE_LIBS} juce::juce_gui_basics juce::juce_recommended_config_flags)

# Auditor's driver (tests/audit_*.cpp): drives the real Session / MihenkProcessor / MainComponent headless. Finds and reports; edits nothing.
juce_add_console_app(audit PRODUCT_NAME "audit")
juce_generate_juce_header(audit)
target_sources(audit PRIVATE tests/audit_main.cpp tests/audit_modes2.cpp tests/audit_formats.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(audit PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(audit PRIVATE ${MIHENK_TOOL_LIBS})

# v3 stage 2: the real Session and the real processor on every corpus file x target x character, random click orders,
# ten bypass cycles, the plug-in path (LEARN, the plan in the real-time chain, processBlockBypassed). Needs tests/corpus.
juce_add_console_app(v3_session_check PRODUCT_NAME "v3_session_check")
juce_generate_juce_header(v3_session_check)
target_sources(v3_session_check PRIVATE tests/v3_session_check.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(v3_session_check PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
target_link_libraries(v3_session_check PRIVATE ${MIHENK_TOOL_LIBS})

# Control audit (2026-10-04): every knob and switch of the panel, SIMPLE and ADVANCED, turned through the real panel -> Session -> plan -> render
# path in a context where it must work; the two masters must differ. Writes build-v3/control_audit_out/control_audit.tsv.
juce_add_console_app(control_audit PRODUCT_NAME "control_audit")
juce_generate_juce_header(control_audit)
target_sources(control_audit PRIVATE tests/control_audit.cpp ${MIHENK_PLUGIN_SOURCES} ${MIHENK_UI_SOURCES} ${MIHENK_ENGINE_SOURCES})
target_compile_definitions(control_audit PRIVATE ${MIHENK_TOOL_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(control_audit PRIVATE ${MIHENK_TOOL_LIBS})
