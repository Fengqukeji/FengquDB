cmake_minimum_required(VERSION 3.24)
project(FengquDBBinarySdkVersion LANGUAGES CXX)
find_package(FengquDB CONFIG REQUIRED)
add_executable(fqdb_binary_sdk_version main.cpp)
target_link_libraries(fqdb_binary_sdk_version PRIVATE FengquDB::Index)
if(MSVC)
    target_compile_options(fqdb_binary_sdk_version PRIVATE /utf-8 /EHsc)
endif()
if(WIN32)
    add_custom_command(TARGET fqdb_binary_sdk_version POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:FengquDB::Core> $<TARGET_FILE:FengquDB::Index>
            $<TARGET_FILE_DIR:fqdb_binary_sdk_version>)
endif()
enable_testing()
add_test(NAME binary_sdk_version COMMAND fqdb_binary_sdk_version)
