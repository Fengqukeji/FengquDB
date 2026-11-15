cmake_minimum_required(VERSION 3.15)
project(fengqudb_node LANGUAGES CXX)

# N-API addon for FengquDB. Only node_api.h and the FengquDB C headers are needed:
# no node-addon-api, no node-gyp download.
#
#   cmake -S . -B build -DFQDB_INCLUDE_DIR=<dir with fengqudb_store.h> \
#         -DFQDB_LIB_DIR=<dir with libfengqudb> -DNODE_INCLUDE_DIR=<dir with node_api.h>
#   cmake --build build
#
# Output: build/fengqudb_native.node (looked up by dist/src/native.js).
# Windows release SDKs ship a prebuilt N-API module; the source build uses MSVC.
# It additionally needs NODE_LIB (node.lib from the Node.js headers/libs package).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FQDB_INCLUDE_DIR "" CACHE PATH "Directory containing fengqudb_store.h")
set(FQDB_LIB_DIR "" CACHE PATH "Directory containing the FengquDB shared library")
set(NODE_INCLUDE_DIR "/usr/local/include/node" CACHE PATH "Directory containing node_api.h")
set(NODE_LIB "" CACHE FILEPATH "Windows only: node.lib to link the addon against")

if(NOT FQDB_INCLUDE_DIR)
  message(FATAL_ERROR "set -DFQDB_INCLUDE_DIR=<dir with fengqudb_store.h>")
endif()

find_library(FQDB_LIBRARY NAMES fengqudb libfengqudb HINTS "${FQDB_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT FQDB_LIBRARY)
  find_library(FQDB_LIBRARY NAMES fengqudb libfengqudb)
endif()
if(NOT FQDB_LIBRARY)
  message(FATAL_ERROR "FengquDB library not found; set -DFQDB_LIB_DIR=...")
endif()

add_library(fengqudb_native MODULE
  native/module.cpp
  native/store.cpp
  native/shared.cpp)
set_target_properties(fengqudb_native PROPERTIES
  PREFIX ""
  OUTPUT_NAME "fengqudb_native"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  SUFFIX ".node"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(fengqudb_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}")
endforeach()

target_include_directories(fengqudb_native PRIVATE "${FQDB_INCLUDE_DIR}" "${NODE_INCLUDE_DIR}")
target_compile_definitions(fengqudb_native PRIVATE NAPI_VERSION=8 NAPI_DISABLE_CPP_EXCEPTIONS)
target_link_libraries(fengqudb_native PRIVATE "${FQDB_LIBRARY}")

if(MSVC)
  target_compile_options(fengqudb_native PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(fengqudb_native PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(NODE_LIB)
    target_link_libraries(fengqudb_native PRIVATE "${NODE_LIB}")
  endif()
else()
  target_compile_options(fengqudb_native PRIVATE -Wall -Wextra -O2)
  if(APPLE)
    target_link_options(fengqudb_native PRIVATE -undefined dynamic_lookup)
    set_target_properties(fengqudb_native PROPERTIES INSTALL_RPATH "@loader_path")
  else()
    set_target_properties(fengqudb_native PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  if(FQDB_LIB_DIR)
    # Optional convenience: find libfengqudb next to the build without LD_LIBRARY_PATH.
    set_target_properties(fengqudb_native PROPERTIES BUILD_RPATH "${FQDB_LIB_DIR}")
  endif()
endif()
