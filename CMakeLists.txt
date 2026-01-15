cmake_minimum_required(VERSION 3.18.0)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

file(READ "${CMAKE_SOURCE_DIR}/VERSION" OpenVDSVersion)
string(STRIP ${OpenVDSVersion} OpenVDSVersion)

project(OpenVDS VERSION ${OpenVDSVersion})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  "${CMAKE_CURRENT_SOURCE_DIR}/CMake")


if(WIN32)
  if(CMAKE_VS_PLATFORM_TOOLSET)
    string(REGEX REPLACE "^v" "" MSVC_TOOLSET_VERSION_LOCAL "${CMAKE_VS_PLATFORM_TOOLSET}")
  else()
    set(MSVC_TOOLSET_VERSION_LOCAL ${MSVC_TOOLSET_VERSION})
  endif()
endif()

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${PROJECT_SOURCE_DIR}/Dist/OpenVDS CACHE STRING "" FORCE)
endif(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)

include(GNUInstallDirs)

set (TEST_DATA_PATH "" CACHE PATH "Test data path")
set (TEST_SEGY_FILE "" CACHE FILEPATH "SEGY test file")
set (TEST_SEGY_HEADER_FILE "" CACHE FILEPATH "Json file describing the header data of SEGY test file")
set (TEST_URL "" CACHE STRING "Test url for OpenOptions")
set (TEST_CONNECTION "" CACHE STRING "Test connection string for OpenOptions")

set (OPENVDS_LIBDIR_SUFFIX "" CACHE STRING "Suffix to add to lib directories on linux")
set (OPENVDS_PYTHON_VERSION "" CACHE STRING "Exact Python 3 version to discover")
set (OPENVDS_JAVA_VERSION "" CACHE STRING "Exact Java version to discover")
set (OPENVDS_3RD_PARTY_DIR "" CACHE PATH "Path to put 3rd party source code, should be absolute path")

option(FORCE_NO_LIBDIR_SUFFIX "Force there to be no suffix on lib directories linux")

get_property(LIB64 GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS)
if ("${LIB64}" STREQUAL "TRUE" AND NOT FORCE_NO_LIBDIR_SUFFIX)
  set(LIBSUFFIX 64)
else()
  set(LIBSUFFIX "")
endif()

if (EXISTS /etc/alpine-release) #the Alpine folks desided to patch cmake but forgot to patch the FIND_LIBRARY_USE_LIB64_PATHS
  if (NOT ${CMAKE_INSTALL_LIBDIR} STREQUAL "lib64")
    set(LIBSUFFIX "")
  endif()
endif()

if (NOT EMSCRIPTEN)
  set(BUILD_PYTHON_DEFAULT OFF)
  if (Python_ROOT_DIR)
    set(PYTHON_REQUIRED "REQUIRED")
  endif()

  if (OPENVDS_PYTHON_VERSION)
    find_package(Python3 ${OPENVDS_PYTHON_VERSION} EXACT REQUIRED COMPONENTS Interpreter Development.Module)
  else()
    find_package(Python3 ${PYTHON_REQUIRED} COMPONENTS Interpreter Development.Module)
  endif()

  if (Python3_FOUND)
    set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
    set(PYTHONINTERP_FOUND ON)
    if (WIN32)
      get_target_property(Python3_IMPLIB_DEBUG Python3::Module IMPORTED_IMPLIB_DEBUG)
      get_target_property(Python3_IMPLIB_RELEASE Python3::Module IMPORTED_IMPLIB_RELEASE)
      if (Python3_IMPLIB_DEBUG)
        set_target_properties(Python3::Module PROPERTIES IMPORTED_IMPLIB_DEBUG ${Python3_IMPLIB_RELEASE})
      endif()
    endif()
  else()
    message("Failed to find Python3 interpreter. Disabling Python build")
    set(BUILD_PYTHON_DEFAULT OFF)
  endif()

  if (OPENVDS_JAVA_VERSION)
    find_package(Java ${OPENVDS_JAVA_VERSION} COMPONENTS Development)
    find_package(JNI)
  else()
    find_package(Java 1.8 COMPONENTS Development)
    find_package(JNI)
  endif()

  if (Java_FOUND AND JNI_FOUND)
    set(BUILD_JAVA_DEFAULT ON)
  else()
    message("Failed to find Java compiler. Disabling Java build")
    set(BUILD_JAVA_DEFAULT OFF)
  endif()
endif()

if(SKBUILD)
  message(STATUS "The project is built using scikit-build")

  if(WIN32)
    set(CMAKE_INSTALL_BINDIR "Scripts")
  endif()

  set(BUILD_JAVA_DEFAULT OFF)
  set(ATTRIBUTION_INSTALL_DIR python/openvds)
else()
  set(ATTRIBUTION_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/)
endif()

if (WIN32)
  set(DEFAULT_USE_LIBRESSL ON)
  set(DEFAULT_BUILD_OPENSSL OFF)
else()
  set(DEFAULT_USE_LIBRESSL OFF)
  set(DEFAULT_BUILD_OPENSSL ON)
endif()

if (APPLE)
  set(CMAKE_BUILD_RPATH "@executable_path")
endif()

if (EMSCRIPTEN)
  option(ENABLE_OPENMP "Enable the OpenMP codepath" OFF) # In case OpenMP is supported in the future.
  option(DISABLE_STRICT_WARNINGS "Disable strict warnings settings. This includes making warnings to errors." OFF)
  option(BUILD_ZLIB   "Build zlib" OFF)
  set(CMAKE_CXX_STANDARD 11)
else()
  option(BUILD_PYTHON "Build python bindings" ${BUILD_PYTHON_DEFAULT})
  option(BUILD_JAVA "Build java bindings" ${BUILD_JAVA_DEFAULT})
  option(ENABLE_OPENMP "Enable the OpenMP codepath" OFF)
  option(ENABLE_MSVC_TOOLSET_DIR "Enable adding a subdir for the MSVC toolset" ON)
  option(BUILD_SHARED_LIBS "Build OpenVDS as a shared library" ON)
  option(BUILD_EXAMPLES "Build OpenVDS example projects" ON)
  option(BUILD_TESTS "Build OpenVDS tests" OFF)
  option(BUILD_DOCS  "Build OpenVDS docs" OFF)
  option(ENABLE_RUNPATH_ORIGIN "On Linux make the install have ORIGIN as RUNPATH" ON)
  option(DISABLE_STRICT_WARNINGS "Disable strict warnings settings. This includes making warnings to errors." ON)
  option(DISABLE_AWS_IOMANAGER "Disable compiling the Aws-cpp-sdk IOManager backend." ON)
  option(DISABLE_AZURE_IOMANAGER "Disable compiling Azure-storage-cpp and cpprestapi. This also disables Azure support for DMS." ON)
  option(DISABLE_AZURESDKFORCPP_IOMANAGER "Disable compiling AzureSdkForCpp. This also disables Azure support for DMS." ON)
  option(DISABLE_AZURE_PRESIGNED_IOMANAGER "Disable compiling Azure presigned IOManager backend." ON)
  option(DISABLE_GCP_IOMANAGER "Disable compiling GCP IOManager backend." ON)
  option(DISABLE_DMS_IOMANAGER "Disable compiling DMS IOManager backend." ON)
  option(BUILD_ZLIB   "Build zlib" ON)
  option(BUILD_CURL   "Build libcurl as part of the openVDS build" ON)
  option(BUILD_UV     "Build libuv as part of the openVDS build" ON)
  option(BUILD_OPENSSL "Build OpenSSL as part of the openVDS build" ${DEFAULT_BUILD_OPENSSL})
  option(USE_LIBRESSL "Build libressl as part of the openVDS build and use it instead of OpenSSL" ${DEFAULT_USE_LIBRESSL})
  option(AUTODETECT_CA_CERTIFICATES "Autodetect location of ca certificates" ON)
  option(AUTO_ADJUST_UUID "When building on MacOS atumatically auto adjust finding UUID" ON)
  option(ENABLE_ASAN "Build this OpenVDS with AddressSanitizer" OFF)
  option(DISABLE_LTO "Disable LTO for release builds" OFF)
  option(ENABLE_GEN_PYTEST_INI "Enable generating pytest.ini file in project root" ON)


  include(CMakeDependentOption)

  # cmake_dependent_option(
  #     DISABLE_CURL_IOMANAGER 
  #     "Disable CURL backend and CURL-based HTTP IOManager."
  #     ON 	# Disable CURL, only if the following is true
  #     "DISABLE_AWS_IOMANAGER;AND;DISABLE_AZURE_PRESIGNED_IOMANAGER;AND;DISABLE_GCP_IOMANAGER;AND;DISABLE_DMS_IOMANAGER"
  #     OFF)	# Otherwise set Disable Curl option to OFF
  # mark_as_advanced(DISABLE_CURL_IOMANAGER)

  if ((${CMAKE_MAJOR_VERSION} EQUAL 3) AND (${CMAKE_MINOR_VERSION} EQUAL 19) AND (${CMAKE_PATCH_VERSION} LESS 3 OR ${CMAKE_PATCH_VERSION} EQUAL 20122902) AND (NOT ${DISABLE_AWS_IOMANAGER}))
    message(FATAL_ERROR "A bug in CMake 3.19 series makes it is makes it not possible to build the AWS backend code.\n Please upgrade og downgrade. Its possible to use custom version of cmake in Visual Studio under CMake Settings.\n Disable AWS IO Manager by defining -DDISABLE_AWS_IOMANAGER=ON as a cmake parameter")
  endif()

  if (DISABLE_AZURESDKFORCPP_IOMANAGER)
    set(CMAKE_CXX_STANDARD 11)
  else()
    set(CMAKE_CXX_STANDARD 14)
  endif()
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (MSVC)
  set(DISABLE_WARNING_FLAG "/W0 /WX-")
else()
  set(DISABLE_WARNING_FLAG "-w")
endif()

define_property(GLOBAL PROPERTY OPENVDS_RUNTIME_LIBS BRIEF_DOCS "Runtime libraries" FULL_DOCS "Files that will be copied to test folders")
define_property(GLOBAL PROPERTY OPENVDS_LINK_LIBRARIES BRIEF_DOCS "OpenVDS link libraries" FULL_DOCS "Files OpenVDS will link with")
define_property(GLOBAL PROPERTY OPENVDS_INCLUDE_LIBRARIES BRIEF_DOCS "OpenVDS include libraries" FULL_DOCS "Include paths for 3rdparty")
define_property(GLOBAL PROPERTY OPENVDS_DEPENDENCY_TARGETS BRIEF_DOCS "Dependency targets for OpenVDS" FULL_DOCS "Targets that OpenVDS needs to build first")

if (ENABLE_MSVC_TOOLSET_DIR AND "${MSVC_TOOLSET_VERSION_LOCAL}")
  set(LIB_TOOLSET_DIR "/msvc_${MSVC_TOOLSET_VERSION_LOCAL}")
endif()

set(LIB_TOOLSET_DIR "${LIB_TOOLSET_DIR}$<$<NOT:$<CONFIG:Release>>:/$<CONFIG>>")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
find_package(Threads)
if (ENABLE_OPENMP)
  find_package(OpenMP)
endif()
if (OpenMP_CXX_FOUND)
  set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
endif()

include(CheckCXXCompilerFlag)
if (EMSCRIPTEN)
  check_cxx_compiler_flag("-msimd128 -msse4.1" ENABLE_SSE4_1) # -msse4.1 will not work without -msimd128.
else()
  check_cxx_compiler_flag("-msse4.1" ENABLE_SSE4_1)
endif()

include(CMake/SetCompilerFlagsForTarget.cmake)
include(CMake/SetWarnings.cmake)
include(CMake/SetVersionForTarget.cmake)
include(CMake/SetLTOForTarget.cmake)
include(CMake/SetCommonTargetProperties.cmake)
include(CMake/AddSystemHeadersToTarget.cmake)
include(CMake/CopyDllForTarget.cmake)

include(CMake/Fetch3rdPartyInBuild.cmake)
Fetch3rdParty()


if (APPLE)
  find_program(BREW_EXEC brew)
  if (BREW_EXEC)
    execute_process(COMMAND ${BREW_EXEC} --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if (NOT OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR "${BREW_PREFIX}/opt/openssl@3")
  endif()
endif()

if (NOT WIN32 AND NOT BUILD_OPENSSL AND NOT EMSCRIPTEN)
  find_package(OpenSSL REQUIRED)
endif()

include(CMake/Build3rdParty.cmake)
build3rdparty()

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

#this is disabled in Java and Python since they match dlopen with a string
set(CMAKE_DEBUG_POSTFIX d)

add_subdirectory(src)

if (NOT EMSCRIPTEN)
  add_subdirectory(docs)

  add_subdirectory(tools)

  if (BUILD_PYTHON)
    add_subdirectory(python)
  endif ()

  if (BUILD_JAVA)
    add_subdirectory(java)
  endif()


  if (BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()

  if (BUILD_EXAMPLES)
    add_subdirectory(examples)
  endif()

  file(RELATIVE_PATH INSTALLED_LIB_RELATIVE_TO_EXAMPLES "${PROJECT_SOURCE_DIR}/${CMAKE_INSTALL_DATAROOTDIR}/openvds/examples" "${PROJECT_SOURCE_DIR}/${CMAKE_INSTALL_DATAROOTDIR}")
  configure_file(CMake/InstalledPaths.cmake.in ${CMAKE_BINARY_DIR}/CMake/InstalledPaths.cmake)
  install(FILES CMake/CopyDllForTarget.cmake CMake/SetWarnings.cmake CMake/SetCompilerFlagsForTarget.cmake ${CMAKE_BINARY_DIR}/CMake/InstalledPaths.cmake
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/openvds/examples/modules)
  install(DIRECTORY examples DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/openvds)
  install(DIRECTORY common/Noise DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/openvds/examples/common)

  configure_file(docker/build_scripts/README.txt.in ${CMAKE_BINARY_DIR}/README.txt)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/Attributions.md ${CMAKE_BINARY_DIR}/README.txt ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${ATTRIBUTION_INSTALL_DIR})

configure_file(
  "${PROJECT_SOURCE_DIR}/CMake/openvds.pc.in"
  "lib/pkgconfig/openvds.pc"
  @ONLY
  )
install(DIRECTORY "${CMAKE_BINARY_DIR}/lib/pkgconfig"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )


if (NOT DEFINED CPACK_GENERATOR)
  if (UNIX)
    set (CPACK_GENERATOR "TGZ")
  elseif(WIN32)
    set (CPACK_GENERATOR "ZIP")
  endif()
endif()

if (UNIX)
    find_program(LSB_RELEASE_EXEC lsb_release)
    execute_process(COMMAND ${LSB_RELEASE_EXEC} -irs
        OUTPUT_VARIABLE OS_NAME
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # OS_NAME is not empty, check for "CentOS" and replace it with "redhat-"
    if (NOT "${OS_NAME}" STREQUAL "")
        STRING(REPLACE "CentOS " "redhat-" OS_NAME ${OS_NAME})
        STRING(REGEX MATCH "(redhat-)[1-9]" OS_NAME ${OS_NAME})
    endif ()

    # if OS_NAME is still empty, set it to Linux
    if ("${OS_NAME}" STREQUAL "")
        set(OS_NAME "Linux")
    endif()
else ()
    # Always set OS_NAME to "win" for windows
    set(OS_NAME "win-${MSVC_TOOLSET_VERSION_LOCAL}")
endif ()

if (NOT DEFINED PACKAGE_RELEASE)
   set(PACKAGE_RELEASE "1")
endif()

set (CPACK_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")
set (CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package-install")
set (CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION}-${PACKAGE_RELEASE}-${OS_NAME}")

include(CPack)

