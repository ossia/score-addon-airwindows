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
  
  ${airwindows_srcs}

  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_airwindows.cpp"
)

include(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/cmake/CmakeRC.cmake)
cmrc_add_resource_library(awdoc_resources ALIAS awdoc::rc NAMESPACE awdoc_resources)
file(GLOB DOC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/*.txt")
cmrc_add_resources(awdoc_resources
    WHENCE "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack"
    ${DOC_FILES}
)

add_library(${PROJECT_NAME} ${SRCS} ${HDRS})
set_target_properties(${PROJECT_NAME} 
  PROPERTIES 
    UNITY_BUILD OFF
)

if(NOT MSVC)
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-unused)
if(NOT CMAKE_CROSSCOMPILING)
  if(CMAKE_BUILD_TYPE MATCHES ".*Debug.*")
    set_target_properties(${PROJECT_NAME} PROPERTIES SCORE_CUSTOM_PCH 1)
    target_precompile_headers(${PROJECT_NAME} PRIVATE
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

    target_compile_options(${PROJECT_NAME} PRIVATE -O3 -march=native)
  endif()
  target_compile_options(${PROJECT_NAME} PRIVATE -w)
endif()

endif()

target_include_directories(${PROJECT_NAME} 
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/airwin2rack/src/autogen_airwin"
)

score_generate_command_list_file(${PROJECT_NAME} "${HDRS}")

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC
    score_lib_base score_plugin_engine score_plugin_media

  PRIVATE
    awdoc_resources
)

setup_score_plugin(${PROJECT_NAME})
