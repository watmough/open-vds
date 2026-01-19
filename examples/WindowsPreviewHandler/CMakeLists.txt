if(WIN32)
  # VDS Shell Extension - Preview and Thumbnail Handlers
  add_library(VdsShellExtension SHARED
    VdsShellExtension.cpp
    VdsRenderer.cpp
    VdsRenderer.h
    VdsShellExtension.def
    version.h
  )

  target_link_libraries(VdsShellExtension
    PRIVATE
      openvds::openvds
      shlwapi
      propsys
  )

  # Single-threaded mode for COM/IStream compatibility in prevhost.exe
  if (OPENVDS_SINGLE_THREADED)
    target_compile_definitions(VdsShellExtension PRIVATE OPENVDS_SINGLE_THREADED)
  endif()

  set_target_properties(VdsShellExtension PROPERTIES
    FOLDER examples
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "VdsShellExtension"
  )

  # Copy the DLL to a known location for registration
  add_custom_command(TARGET VdsShellExtension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:VdsShellExtension>
      ${CMAKE_CURRENT_BINARY_DIR}/VdsShellExtension.dll
    COMMENT "Copying VdsShellExtension.dll for registration"
  )

  # Generate registration script with correct path
  set(DLL_PATH "$<TARGET_FILE:VdsShellExtension>")
  string(REPLACE "/" "\\\\" DLL_PATH_ESCAPED "${DLL_PATH}")

  add_custom_command(TARGET VdsShellExtension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "DLL built at: ${DLL_PATH}"
    COMMENT "VDS Shell Extension build complete"
  )

  setWarningFlagsForTarget(VdsShellExtension)
  copyDllForTarget(VdsShellExtension)

  # Install
  install(TARGETS VdsShellExtension
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
  )

  # VDS Render Test Harness - standalone command-line tool for debugging
  add_executable(VdsRenderTest
    VdsRenderTest.cpp
  )

  target_link_libraries(VdsRenderTest
    PRIVATE
      openvds::openvds
      shlwapi
  )

  set_target_properties(VdsRenderTest PROPERTIES
    FOLDER examples
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )

  setWarningFlagsForTarget(VdsRenderTest)
  copyDllForTarget(VdsRenderTest)

  install(TARGETS VdsRenderTest
    RUNTIME DESTINATION bin
  )

  # VDS Viewer - Standalone VDS viewer application
  add_executable(VdsViewer WIN32
    VdsViewer.cpp
    VdsRenderer.cpp
    VdsRenderer.h
    VdsViewer.rc
  )

  target_link_libraries(VdsViewer
    PRIVATE
      openvds::openvds
      shlwapi
      comdlg32
      shell32
  )

  set_target_properties(VdsViewer PROPERTIES
    FOLDER examples
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "vds-viewer"
  )

  setWarningFlagsForTarget(VdsViewer)
  copyDllForTarget(VdsViewer)

  install(TARGETS VdsViewer
    RUNTIME DESTINATION bin
  )

  # MSI Installer (requires WiX Toolset)
  add_subdirectory(installer)

else()
  message(STATUS "VdsShellExtension is only available on Windows")
endif()
