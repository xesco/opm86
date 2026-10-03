cmake_minimum_required(VERSION 3.22)

project(OPM86 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

option(OPM86_BUILD_PLUGIN "Build the VST3/AU/Standalone plugin" ON)

include(FetchContent)

FetchContent_Declare(ymfm
    GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
    GIT_TAG 81aec25ccbb98f4873a255f7551ac4dadac59b4a)
FetchContent_MakeAvailable(ymfm) # no CMakeLists upstream; this only downloads

find_package(ZLIB REQUIRED)

# Chip emulation and instrument logic, free of any plugin framework.
add_library(opm86_core STATIC
    src/core/Arrangement.cpp
    src/core/Engine.cpp
    src/core/FmSection.cpp
    src/core/MidiExport.cpp
    src/core/PcmBank.cpp
    src/core/Presets.cpp
    src/core/SegaPcm.cpp
    src/core/SongLog.cpp
    src/core/SongPlayer.cpp
    src/core/SongTrack.cpp
    src/core/Tune.cpp
    src/core/TuneRebuilder.cpp
    src/core/VgmImport.cpp
    src/core/VgmPlayer.cpp
    src/core/VgmSong.cpp
    ${ymfm_SOURCE_DIR}/src/ymfm_opm.cpp)
target_include_directories(opm86_core PUBLIC src/core)
target_include_directories(opm86_core SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(opm86_core PUBLIC ZLIB::ZLIB Threads::Threads)
set_target_properties(opm86_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE opm86_core)
add_test(NAME core_test COMMAND core_test)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# The command-line player and renderer.
juce_add_console_app(opm86_cli PRODUCT_NAME "opm86")
target_sources(opm86_cli PRIVATE src/cli/Main.cpp)
set_target_properties(opm86_cli PROPERTIES OUTPUT_NAME opm86)
target_include_directories(opm86_cli PRIVATE src/common)
target_compile_definitions(opm86_cli PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(opm86_cli
    PRIVATE
        opm86_core
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
add_test(NAME tuning COMMAND opm86_cli tune)

# The tracker: a standalone Mac app that edits a VGM rip and plays it exactly.
juce_add_binary_data(opm86_fonts SOURCES
    resources/fonts/PressStart2P-Regular.ttf
    resources/fonts/JetBrainsMono-Regular.ttf
    resources/fonts/JetBrainsMono-Bold.ttf)
juce_add_gui_app(opm86_tracker
    PRODUCT_NAME "OPM86 Tracker"
    COMPANY_NAME "xesco"
    BUNDLE_ID "com.xesco.opm86tracker"
    DOCUMENT_EXTENSIONS vgz vgm)
target_sources(opm86_tracker PRIVATE
    src/app/AppModel.cpp
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/Theme.cpp
    src/app/TrackerView.cpp)
target_compile_definitions(opm86_tracker PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_APPLICATION_NAME_STRING="OPM86 Tracker"
    JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(opm86_tracker
    PRIVATE
        opm86_core
        opm86_fonts
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Checks a VGM converted to MIDI against the VGM played straight into the chips.
juce_add_console_app(export_test)
target_sources(export_test PRIVATE tests/export_test.cpp)
target_compile_definitions(export_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(export_test PRIVATE opm86_core juce::juce_audio_basics)
add_test(NAME export_test COMMAND export_test)

# Checks song mode: song tracks played straight and rearranged.
juce_add_console_app(song_test)
target_sources(song_test PRIVATE tests/song_test.cpp)
target_compile_definitions(song_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(song_test PRIVATE opm86_core juce::juce_audio_basics)
add_test(NAME song_test COMMAND song_test)

if(OPM86_BUILD_PLUGIN)
    juce_add_plugin(OPM86
        COMPANY_NAME "xesco"
        BUNDLE_ID "com.xesco.opm86"
        PLUGIN_MANUFACTURER_CODE Xsco
        PLUGIN_CODE Op86
        PRODUCT_NAME "OPM86"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        VST3_CATEGORIES Instrument Synth
        FORMATS AU VST3 Standalone)

    target_sources(OPM86 PRIVATE
        src/plugin/Parameters.cpp
        src/plugin/PluginEditor.cpp
        src/plugin/PluginProcessor.cpp
        src/plugin/SongCache.cpp)
    target_include_directories(OPM86 PRIVATE src/common)

    target_compile_definitions(OPM86 PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(OPM86
        PRIVATE
            opm86_core
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # Development aid: draws the editor to a PNG. Not built by default.
    juce_add_console_app(editor_snapshot)
    set_target_properties(editor_snapshot PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(editor_snapshot PRIVATE
        tests/editor_snapshot.cpp
        src/plugin/Parameters.cpp
        src/plugin/PluginEditor.cpp
        src/plugin/PluginProcessor.cpp
        src/plugin/SongCache.cpp)
    target_include_directories(editor_snapshot PRIVATE src/plugin src/common)
    target_compile_definitions(editor_snapshot PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(editor_snapshot PRIVATE opm86_core juce::juce_audio_utils)

    # Processor checks, run by ctest.
    juce_add_console_app(plugin_test)
    target_sources(plugin_test PRIVATE
        tests/plugin_test.cpp
        src/plugin/Parameters.cpp
        src/plugin/PluginEditor.cpp
        src/plugin/PluginProcessor.cpp
        src/plugin/SongCache.cpp)
    target_include_directories(plugin_test PRIVATE src/plugin src/common)
    target_compile_definitions(plugin_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(plugin_test PRIVATE opm86_core juce::juce_audio_utils)
    add_test(NAME plugin_test COMMAND plugin_test)
endif()
