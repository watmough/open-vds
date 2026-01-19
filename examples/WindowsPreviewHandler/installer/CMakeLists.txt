# VDS Shell Extension MSI Installer
# Requires WiX Toolset 5.x to be installed

if(NOT WIN32)
  message(STATUS "VDS Shell Extension Installer is only available on Windows")
  return()
endif()

# Find WiX Toolset 5.x (uses 'wix' command instead of candle/light)
find_program(WIX_EXECUTABLE wix
  HINTS
    "$ENV{WIX}/bin"
    "C:/Program Files/WiX Toolset v5.0/bin"
    "C:/Program Files (x86)/WiX Toolset v5.0/bin"
)

if(NOT WIX_EXECUTABLE)
  message(STATUS "WiX Toolset 5 not found - VdsShellExtensionInstaller target will not be available")
  message(STATUS "  Install WiX 5 from https://wixtoolset.org/")
  return()
endif()

message(STATUS "Found WiX: ${WIX_EXECUTABLE}")

# Staging directory for DLLs
set(STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/staging")
file(MAKE_DIRECTORY ${STAGING_DIR})

# WiX source file
set(WXS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/VdsShellExtension.wxs")
set(MSI_FILE "${CMAKE_CURRENT_BINARY_DIR}/VdsShellExtension.msi")

# Custom command to stage DLLs and EXEs
add_custom_command(
  OUTPUT ${STAGING_DIR}/openvds.dll ${STAGING_DIR}/VdsShellExtension.dll ${STAGING_DIR}/vds-viewer.exe
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:openvds> ${STAGING_DIR}/openvds.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:VdsShellExtension> ${STAGING_DIR}/VdsShellExtension.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:VdsViewer> ${STAGING_DIR}/vds-viewer.exe
  DEPENDS openvds VdsShellExtension VdsViewer
  COMMENT "Staging files for MSI installer"
)

# Custom command to build MSI with WiX 5
# WiX 5 uses: wix build <source.wxs> -o <output.msi> -d <variable>=<value>
add_custom_command(
  OUTPUT ${MSI_FILE}
  COMMAND ${WIX_EXECUTABLE} build
    ${WXS_SOURCE}
    -arch x64
    -d StagingDir=${STAGING_DIR}
    -ext WixToolset.UI.wixext
    -o ${MSI_FILE}
  DEPENDS ${WXS_SOURCE} ${STAGING_DIR}/openvds.dll ${STAGING_DIR}/VdsShellExtension.dll ${STAGING_DIR}/vds-viewer.exe
  COMMENT "Building MSI installer with WiX 5"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Custom target for building the MSI
add_custom_target(VdsShellExtensionInstaller
  DEPENDS ${MSI_FILE}
  COMMENT "Building VDS Shell Extension MSI Installer"
)

# Set folder in IDE
set_target_properties(VdsShellExtensionInstaller PROPERTIES FOLDER "examples")

message(STATUS "VdsShellExtensionInstaller target available - build with: cmake --build . --target VdsShellExtensionInstaller")
