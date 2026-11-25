cmake_minimum_required(VERSION 3.24)
project(hertzyevs LANGUAGES C CXX)
# HertzyEVS: event-camera frequency + RPM workbench for the LUCID Triton2 EVS (IMX636), HvA Maintenance Lab.
#   cmake -S . -B build -G Ninja            (dev edition: simulator, tests, scripted screenshots)
#   cmake -S . -B build-full -DHZ_EDITION=full     (camera release, no simulator)
#   cmake -S . -B build-viewer -DHZ_EDITION=viewer (Library + Analyse only, no Arena SDK needed)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/O2 /fp:fast /bigobj /utf-8 /permissive- /W3 /wd4244 /wd4267 /wd4305)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---- third-party headers, downloaded into the build folder -----------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(pocketfft GIT_REPOSITORY https://github.com/mreineck/pocketfft.git GIT_TAG cpp SOURCE_SUBDIR none)
FetchContent_MakeAvailable(pocketfft)
add_library(thirdparty_headers INTERFACE)
target_include_directories(thirdparty_headers SYSTEM INTERFACE ${pocketfft_SOURCE_DIR})
target_compile_definitions(thirdparty_headers INTERFACE POCKETFFT_NO_MULTITHREADING)

# ---- Qt 6.8 (e.g. python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O %USERPROFILE%/Qt) ---------------
if(WIN32 AND NOT CMAKE_PREFIX_PATH AND EXISTS "$ENV{USERPROFILE}/Qt/6.8.3/msvc2022_64")
  list(APPEND CMAKE_PREFIX_PATH "$ENV{USERPROFILE}/Qt/6.8.3/msvc2022_64")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

# ---- LUCID Arena SDK (camera) ------------------------------------------------------------------------------------------
if(WIN32)
  set(ARENA_SDK_DIR "C:/Program Files/Lucid Vision Labs/Arena SDK" CACHE PATH "LUCID Arena SDK folder (with include/ and lib64/)")
else()
  set(ARENA_SDK_DIR "$ENV{HOME}/ArenaSDK_Linux_ARM64" CACHE PATH "LUCID Arena SDK folder (with include/ and lib/)")
endif()
set(HZ_EDITION "dev" CACHE STRING "edition: dev | full | viewer")
set_property(CACHE HZ_EDITION PROPERTY STRINGS dev full viewer)
if(NOT HZ_EDITION MATCHES "^(dev|full|viewer)$")
  message(FATAL_ERROR "HZ_EDITION must be dev, full or viewer (got ${HZ_EDITION})")
endif()
message(STATUS "HertzyEVS: edition ${HZ_EDITION}")
if(HZ_EDITION STREQUAL "viewer")
  set(ARENA_SDK_DIR "" CACHE PATH "" FORCE)
endif()

# ---- core: codec, engine, sources, clips, ROIs, measurements, RPM, library (no QML) ------------------------------------
add_library(hz_core STATIC
  src/evt3.cpp src/engine.cpp src/sim_source.cpp src/arena_source.cpp src/roi.cpp src/clip.cpp src/measure.cpp src/rpm.cpp
  src/detect.cpp src/library.cpp common/dsp.cpp common/signal.cpp)
target_include_directories(hz_core PUBLIC src common)
target_link_libraries(hz_core PUBLIC thirdparty_headers Qt6::Core Qt6::Gui)
set_target_properties(hz_core PROPERTIES AUTOMOC OFF)
if(EXISTS "${ARENA_SDK_DIR}/include/Arena/ArenaApi.h")
  message(STATUS "HertzyEVS: Arena SDK found in ${ARENA_SDK_DIR}")
  target_compile_definitions(hz_core PRIVATE HZ_HAVE_ARENA)
  target_include_directories(hz_core SYSTEM PRIVATE "${ARENA_SDK_DIR}/include/Arena" "${ARENA_SDK_DIR}/GenICam/library/CPP/include"
                             "${ARENA_SDK_DIR}/include/Save" "${ARENA_SDK_DIR}/include/GenTL")
  if(WIN32)
    set(_gc "${ARENA_SDK_DIR}/GenICam/library/CPP/lib/Win64_x64")
    target_link_libraries(hz_core PRIVATE
      "${ARENA_SDK_DIR}/lib64/Arena/Arena_v140.lib" "${ARENA_SDK_DIR}/lib64/Arena/GenTL_LUCID_v140.lib"
      "${ARENA_SDK_DIR}/lib64/Arena/lucidlog_v140.lib"
      "${_gc}/GCBase_MD_VC140_v3_3_LUCID.lib" "${_gc}/GenApi_MD_VC140_v3_3_LUCID.lib")
  else()
    file(GLOB _arena_libs "${ARENA_SDK_DIR}/lib/libarena.so" "${ARENA_SDK_DIR}/lib/libgentl.so" "${ARENA_SDK_DIR}/lib/liblucidlog.so")
    file(GLOB _gc_libs "${ARENA_SDK_DIR}/GenICam/library/lib/Linux64_*/libGCBase_*.so"
                       "${ARENA_SDK_DIR}/GenICam/library/lib/Linux64_*/libGenApi_*.so"
                       "${ARENA_SDK_DIR}/GenICam/library/lib/Linux64_*/libMathParser_*.so"
                       "${ARENA_SDK_DIR}/GenICam/library/lib/Linux64_*/liblog4cpp_*.so"
                       "${ARENA_SDK_DIR}/GenICam/library/lib/Linux64_*/libLog_*.so")
    target_link_libraries(hz_core PRIVATE ${_arena_libs} ${_gc_libs} pthread)
    list(GET _gc_libs 0 _gc0)
    get_filename_component(_gcdir "${_gc0}" DIRECTORY)
    set(CMAKE_BUILD_RPATH "${ARENA_SDK_DIR}/lib;${_gcdir};${ARENA_SDK_DIR}/ffmpeg")
  endif()
else()
  message(STATUS "HertzyEVS: no Arena SDK (${ARENA_SDK_DIR}) - no camera support")
endif()

add_executable(hz_cli src/cli_main.cpp)
target_link_libraries(hz_cli PRIVATE hz_core)
add_executable(hz_tests src/tests.cpp)
target_link_libraries(hz_tests PRIVATE hz_core)

# ---- the app -------------------------------------------------------------------------------------------------------------
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
file(GLOB HZ_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
foreach(f ${HZ_QML})
  get_filename_component(n ${f} NAME)
  set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${n})
endforeach()
if(WIN32)
  qt_add_executable(hertzyevs WIN32 src/main.cpp icons/hertzyevs.rc)
else()
  qt_add_executable(hertzyevs src/main.cpp)
endif()
qt_add_qml_module(hertzyevs
  URI HertzyEvs
  VERSION 1.0
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/HertzyEvs
  QML_FILES ${HZ_QML}
  SOURCES src/controller.hpp src/controller.cpp src/liveview.hpp src/liveview.cpp src/roictl.hpp src/roictl.cpp
          src/webview.hpp src/webview.cpp src/player.hpp src/player.cpp src/analyse.hpp src/analyse.cpp
          src/libctl.hpp src/libctl.cpp
          common/plotitem.hpp common/plotitem.cpp common/tablemodel.hpp common/tablemodel.cpp)
target_include_directories(hertzyevs PRIVATE src common)
if(MSVC)
  target_link_options(hertzyevs PRIVATE /MAP)   # crash offsets -> function names
endif()
if(NOT HZ_EDITION STREQUAL "dev")
  target_compile_definitions(hertzyevs PRIVATE EVS_NO_SIM)
endif()
if(HZ_EDITION STREQUAL "viewer")
  target_compile_definitions(hertzyevs PRIVATE EVS_VIEWER)
endif()
target_compile_definitions(hertzyevs PRIVATE EVS_EDITION="${HZ_EDITION}")
target_link_libraries(hertzyevs PRIVATE hz_core Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2)

if(WIN32)   # HertzyEVS.exe: one-icon launcher for the portable package (tools/ship_hertzy.ps1)
  add_executable(hertzyevs_launcher WIN32 src/launcher.cpp icons/hertzyevs.rc)
  set_target_properties(hertzyevs_launcher PROPERTIES AUTOMOC OFF)
  find_program(HZ_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
  if(HZ_WINDEPLOYQT)
    add_custom_command(TARGET hertzyevs POST_BUILD
      COMMAND "${HZ_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw --verbose 0
              --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml" "$<TARGET_FILE:hertzyevs>"
      VERBATIM)
  endif()
endif()
