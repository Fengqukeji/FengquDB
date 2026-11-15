cmake_minimum_required(VERSION 3.24)
project(FengquDBJavaAdapter LANGUAGES CXX)

# Standalone adapter build for a client SDK; no database engine sources needed.
get_filename_component(_fqdb_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
list(APPEND CMAKE_PREFIX_PATH "${_fqdb_sdk}")
find_package(FengquDB CONFIG REQUIRED)
find_package(JNI REQUIRED)
add_library(fengqudb_jni SHARED store_jni.cpp)
target_compile_features(fengqudb_jni PRIVATE cxx_std_17)
target_include_directories(fengqudb_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(fengqudb_jni PRIVATE FengquDB::Core)
set_target_properties(fengqudb_jni PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(fengqudb_jni PRIVATE /utf-8)
endif()
