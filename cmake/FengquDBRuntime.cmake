# Attach runtime deployment to a consumer executable, keeping SDK tools out of its output.
function(fengqudb_link target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "fengqudb_link requires an existing executable target")
  endif()
  target_link_libraries(${target} PRIVATE FengquDB::Core)
  get_target_property(_fqdb_standard ${target} CXX_STANDARD)
  if(NOT _fqdb_standard OR _fqdb_standard LESS 17)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
  endif()
  if(FengquDB_SHARED)
    if(WIN32)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:FengquDB::Core>" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    elseif(UNIX AND NOT APPLE)
      # Distributed ELF libraries carry an ABI SONAME; keep that filename beside the app.
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:FengquDB::Core>" "$<TARGET_FILE_DIR:${target}>/$<TARGET_SONAME_FILE_NAME:FengquDB::Core>"
        VERBATIM)
      set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
      set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    elseif(APPLE)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:FengquDB::Core>" "$<TARGET_FILE_DIR:${target}>/libfengqudb.2.dylib"
        VERBATIM)
      set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@loader_path")
      set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path")
    endif()
  endif()
endfunction()
