cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

project(score_addon_airwindows LANGUAGES CXX)

if(NOT TARGET score_plugin_engine)
  return()
endif()

score_common_setup()

file(GLOB airwindows_srcs  
  "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/autogen_airwin/*.cpp"
)
# Files & main target
set(HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Executor/Component.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Library.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/ProcessFactory.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/ProcessMetadata.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/ProcessModel.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_airwindows.hpp"
)
set(SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/ProcessModel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/ProcessModelSerialization.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Executor/Component.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_airwindows.cpp"
)

# The per-effect documentation, embedded as a resource: AirwinRegistry.cpp reads it.
include(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/cmake/CmakeRC.cmake)
cmrc_add_resource_library(awdoc_resources ALIAS awdoc::rc NAMESPACE awdoc_resources)
file(GLOB DOC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/*.txt")
cmrc_add_resources(awdoc_resources
    WHENCE "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack"
    ${DOC_FILES}
)

# The airwindows effects themselves: 1459 files imported verbatim. Their own target
# so that the warning suppression and the DSP optimisation flags they need stay off
# every line of code we actually wrote.
add_library(airwindows_dsp STATIC ${airwindows_srcs})

target_include_directories(airwindows_dsp
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src"
  PRIVATE
    # 486 headers named Point.h, curve.h, Chorus.h... - kept off every other target
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/autogen_airwin"
)

target_link_libraries(airwindows_dsp PRIVATE awdoc_resources)

set_target_properties(airwindows_dsp
  PROPERTIES
    UNITY_BUILD OFF
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_definitions(airwindows_dsp PRIVATE _USE_MATH_DEFINES=1)
else()
  target_compile_options(airwindows_dsp PRIVATE -w)

  if(NOT CMAKE_CROSSCOMPILING AND CMAKE_BUILD_TYPE MATCHES ".*Debug.*")
    # Optimize the DSP even in Debug, which is what score_optimize_in_debug_mode()
    # is for - it cannot be used here because it bails once SCORE_CUSTOM_PCH is set.
    # Deliberately -O3 and not its -Ofast: -ffast-math lets the compiler assume no
    # NaN or infinity exists, and this code is full of both denormal and infinity
    # handling. (Our own code uses ossia::safe_isnan/safe_isinf, which survive it,
    # but there is no reason to change the FP semantics of imported DSP.)
    target_compile_options(airwindows_dsp PRIVATE -O3 ${SCORE_NATIVE_ARCH_FLAG})
    set_target_properties(airwindows_dsp PROPERTIES SCORE_CUSTOM_PCH 1)
    target_precompile_headers(airwindows_dsp PRIVATE
      <cstdint>
      <cassert>
      <cstring>
      <cstdio>
      <cmath>
      <vector>
      <map>
      <set>
      <algorithm>
      <memory>
      <optional>
      <string>
      <array>
      <atomic>
      <mutex>
    )
  endif()
endif()

# The parameter metadata derived from those effects. A plain static library so that
# tests can probe them without having to link (and boot) the score plug-in.
add_library(score_addon_airwindows_core STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Metadata.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Metadata.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Airwindows/Registry.cpp"
)

target_include_directories(score_addon_airwindows_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(score_addon_airwindows_core
  PUBLIC ${QT_PREFIX}::Core ossia airwindows_dsp)

set_target_properties(score_addon_airwindows_core
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${PROJECT_NAME} ${SRCS} ${HDRS})
set_target_properties(${PROJECT_NAME} 
  PROPERTIES 
    UNITY_BUILD OFF
)

if(MSVC)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _USE_MATH_DEFINES=1)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wno-unused)
endif()

score_generate_command_list_file(${PROJECT_NAME} "${HDRS}")

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC
    score_lib_base score_plugin_engine score_plugin_media
    score_addon_airwindows_core
)

setup_score_plugin(${PROJECT_NAME})

if(SCORE_TESTING AND COMMAND score_add_test)
  add_subdirectory(Tests)
endif()
