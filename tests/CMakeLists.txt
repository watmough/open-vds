include(GoogleTest)
set(BUILD_GMOCK OFF CACHE BOOL "We dont need mock" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Dont install" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${gtest_SOURCE_DIR} ${PROJECT_BINARY_DIR}/gtest)
set_target_properties(gtest PROPERTIES
  FOLDER ExternalProjectTargets/GoogleTest)
set_target_properties(gtest_main PROPERTIES
  FOLDER ExternalProjectTargets/GoogleTest)

string(REPLACE " " ";" DISABLE_WARNING_FLAG ${DISABLE_WARNING_FLAG})
openvds_target_compile_options(gtest PRIVATE ${DISABLE_WARNING_FLAG})
openvds_target_compile_options(gtest_main PRIVATE ${DISABLE_WARNING_FLAG})
setCompilerFlagsForTarget(gtest)
setCompilerFlagsForTarget(gtest_main)

add_executable(TestRootTarget RootTestExecutable.cpp)
set_target_properties(TestRootTarget PROPERTIES FOLDER tests)
copyDllForTarget(TestRootTarget)

include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)

get_property(include_3rdparty      GLOBAL PROPERTY OPENVDS_INCLUDE_LIBRARIES)

string(REPLACE ";" "\\\\;" TEST_CONNECTION_ESCAPED "${TEST_CONNECTION}")

function(add_test_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE openvds_objects ods_objects gtest gtest_main)
  addSystemHeadersToTarget(${name} "${include_3rdparty}")
  target_compile_definitions(${name} PRIVATE openvds_EXPORTS)
  add_dependencies(${name} TestRootTarget)
  gtest_discover_tests(${name}
    DISCOVERY_TIMEOUT 25
    XML_OUTPUT_DIR ${PROJECT_BINARY_DIR}/test_results
  )
  setCommonTestTargetProperties(${name})
  set_target_properties(${name} PROPERTIES FOLDER tests)
  get_target_property(json_INCLUDE jsoncpp_lib_static INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(${name} SYSTEM PRIVATE ${json_INCLUDE})
  addSystemHeadersToTarget(${name} "${json_INCLUDE}")
  
  target_link_libraries(${name} PRIVATE fmt::fmt)

  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src/OpenVDS ${COMMON_DIR})
  target_compile_definitions(${name} PRIVATE -DTEST_DATA_PATH="${PROJECT_SOURCE_DIR}/tests/VDS")
  target_compile_definitions(${name} PRIVATE -DTEST_URL="${TEST_URL}")
  target_compile_definitions(${name} PRIVATE -DTEST_CONNECTION="${TEST_CONNECTION_ESCAPED}")

  get_property(link_3rdparty GLOBAL PROPERTY OPENVDS_LINK_LIBRARIES)

  if (OpenMP_CXX_FOUND)
      target_link_libraries(${name} PRIVATE OpenMP::OpenMP_CXX)
  endif()
  if (DISABLE_AWS_IOMANAGER)
    target_compile_definitions(${name} PRIVATE OPENVDS_NO_AWS_IOMANAGER)
  endif()
  if (DISABLE_AZURESDKFORCPP_IOMANAGER)
    target_compile_definitions(${name} PRIVATE OPENVDS_NO_AZURE_SDK_FOR_CPP_IOMANAGER)
  endif()
endfunction()

if (NOT ENABLE_ASAN)
  add_test_executable(io_tests
    io/filetest.cpp
    io/InMemoryIo.cpp
    io/IoManagerBasic.cpp
    io/sdpath.cpp
    )

  add_test_executable(io_performance_test
    io/IoPerformance.cpp
    )

  add_test_executable(io_vds_roundtrip_test
    io/IoVDSRoundtrip.cpp
    )

  add_test_executable(io_error_handling_tests
    io/IoErrorHandling.cpp
    io/IoManagerProtocol.cpp
    )

  add_test_executable(jsoncpp_tests
    jsoncpp/jsoncpp_parse.cpp
    jsoncpp/jsoncpp_serialize.cpp)

  add_test_executable(fmt_tests
    fmt/basic_fmt.cpp)

  add_test_executable(segy_tests
    SEG-Y/SEGYScanTest.cpp
    ../src/SEGYUtils/SEGY.cpp
    ../src/SEGYUtils/SEGYFileInfo.cpp
    ../src/SEGYUtils/SEGYUtils/SEGYFileInfo.h)
  target_include_directories(segy_tests PRIVATE ../src/SEGYUtils)

  add_test_executable(deserialize VDS/DeserializeVolumeDataTest.cpp)

  add_test_executable(vds_integration_tests
    VDS/ParseVDSJsonTest.cpp
    VDS/RequestVolumeCleanupThread.cpp
    VDS/ParseConnectionStringTest.cpp
    VDS/LoggingTests.cpp
    )

  add_test_executable(openvds_integration_tests
    OpenVDS/OpenVDSIntegrationTest.cpp
    OpenVDS/DataAccess.cpp
    OpenVDS/RequestVolumeSamples.cpp
    OpenVDS/RequestVolumeTraces.cpp
    OpenVDS/RequestVolumeSubsetFormat.cpp
    OpenVDS/DataAccessorClone.cpp
    OpenVDS/WaitForCompletion.cpp
    OpenVDS/RequestCancellation.cpp
    OpenVDS/VolumeIndexerSymbols.cpp
    OpenVDS/RequestVolumeError.cpp
    OpenVDS/IJKCoordinateTransformerTest.cpp
    OpenVDS/MultiComponent.cpp
    OpenVDS/ReadWritePage.cpp
    OpenVDS/VolumeDataPageAccessorGetChunkVolumeDataHash.cpp
    OpenVDS/ReadOnly.cpp
    OpenVDS/MetadataEnableWriting.cpp
    )

  add_test_executable(multithreaded_requests
    OpenVDS/MultiThreadedRequests.cpp
    )

  add_test_executable(global_state
    OpenVDS/GlobalStateTest.cpp
    )


  add_test_executable(openvds_mixed_request
    OpenVDS/RequestVolumeSubsetAndPageAccessor.cpp
    )

  add_test_executable(tools
    tools/SplitUrlTest.cpp
    )
endif()
if (TEST_SEGY_FILE AND TEST_URL)
  add_test(NAME "tools.SegyRoundtrip"
    COMMAND ${CMAKE_COMMAND}
                -DSEGYImport=$<TARGET_FILE:SEGYImport>
                -DSEGYExport=$<TARGET_FILE:SEGYExport>
                -DTEST_URL=${TEST_URL}
                -DTEST_CONNECTION=${TEST_CONNECTION_ESCAPED}
                -DTEST_SEGY_FILE=${TEST_SEGY_FILE}
                -DTEST_SEGY_HEADER_FILE=${TEST_SEGY_HEADER_FILE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunRoundtrip.cmake
    )
endif()

if (BUILD_PYTHON)
  add_subdirectory(python)
endif()

if (BUILD_JAVA)
  add_custom_command(TARGET TestRootTarget
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:openvds-javacpp>;${testng_SOURCE_FILE};${jcommander_SOURCE_FILE}" $<TARGET_FILE_DIR:TestRootTarget>
    COMMAND_EXPAND_LISTS
    DEPENDS openvds-javacpp
    )

  get_filename_component(testng_file_name ${testng_SOURCE_FILE} NAME)
  get_filename_component(jcommander_file_name ${jcommander_SOURCE_FILE} NAME)

  set(copied_testng "$<TARGET_FILE_DIR:TestRootTarget>/${testng_file_name}")
  set(copied_jcommander "$<TARGET_FILE_DIR:TestRootTarget>/${jcommander_file_name}")
  add_subdirectory(java)
endif()

if (NOT ENABLE_ASAN)
  add_subdirectory(benchmark)
endif()
