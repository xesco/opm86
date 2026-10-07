cmake_minimum_required(VERSION 3.22)

project(OPM86 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

include(FetchContent)

FetchContent_Declare(ymfm
    GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
    GIT_TAG 81aec25ccbb98f4873a255f7551ac4dadac59b4a)
FetchContent_MakeAvailable(ymfm) # no CMakeLists upstream; this only downloads

# zlib reads .vgz files: the system's where there is one (macOS, Linux), else
# built from source (Windows).
find_package(ZLIB)
if(NOT ZLIB_FOUND)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# Chip emulation and song logic, free of any app framework.
add_library(opm86_core STATIC
    src/core/SegaPcm.cpp
    src/core/SongLog.cpp
    src/core/SongPlayer.cpp
    src/core/Tune.cpp
    src/core/TuneRebuilder.cpp
    src/core/VgmImport.cpp
    src/core/VgmSong.cpp
    src/core/YmfmOpm.cpp
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

# The player: a standalone Mac app that plays a VGM rip exactly, track by track.
juce_add_binary_data(opm86_fonts SOURCES
    resources/fonts/PressStart2P-Regular.ttf
    resources/fonts/JetBrainsMono-Regular.ttf
    resources/fonts/JetBrainsMono-Bold.ttf)
juce_add_gui_app(opm86_player
    PRODUCT_NAME "OPM86 Player"
    COMPANY_NAME "xesco"
    BUNDLE_ID "com.xesco.opm86player"
    DOCUMENT_EXTENSIONS vgz vgm)
target_sources(opm86_player PRIVATE
    src/app/AppModel.cpp
    src/app/ChipView.cpp
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/Theme.cpp
    src/app/TrackerView.cpp)
target_compile_definitions(opm86_player PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_APPLICATION_NAME_STRING="OPM86 Player"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(opm86_player
    PRIVATE
        opm86_core
        opm86_fonts
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
