include(${PROJECT_SOURCE_DIR}/CMake/CompileInTarget.cmake)

set(WAVELET_SOURCES
      VDS/WaveletAdaptiveLLDecompress.cpp
      VDS/WaveletDecompress.cpp
      VDS/WaveletInverseTransform.cpp
      VDS/WaveletTypes.cpp
      VDS/FSE/entropy_common.cpp
      VDS/FSE/fse_decompress.cpp
      VDS/Rle.cpp
   )
set(WAVELET_HEADERS
      VDS/WaveletAdaptiveLLDecompress.h
      VDS/WaveletAdaptiveLLDecompress_DecodeAllBits.h
      VDS/WaveletAdaptiveLLDecompress_DecodeTreeStructure.h
      VDS/WaveletDecompress.h
      VDS/WaveletInverseTransform.h
      VDS/WaveletInverseTransformSSE.h
      VDS/WaveletTypes.h
      VDS/FSE/bitstream.h
      VDS/FSE/error_private.h
      VDS/FSE/error_public.h
      VDS/FSE/mem.h
      VDS/FSE/fse.h
      VDS/Rle.h
   )

if (EMSCRIPTEN)
  set(SOURCE_FILES
    VDS/VolumeDataStore.cpp
    VDS/DataBlock.cpp
    ${WAVELET_SOURCES})

  set (PRIVATE_HEADER_FILES
    VDS/DataBlock.h
    ${WAVELET_HEADERS}
    VDS/Logging.h)
else()

  set(SOURCE_FILES
    OpenVDS.cpp
    IO/File.cpp
    IO/Linux_File.cpp
    IO/Win_File.cpp
    IO/IOManager.cpp
    IO/IOManagerAWSCurl.cpp
    IO/IOManagerAzureSdkForCpp.cpp
    IO/IOManagerInMemory.cpp
    IO/IOManagerCurl.cpp
    IO/IOManagerAzurePresigned.cpp
    IO/IOManagerGoogle.cpp
    IO/IOManagerHttp.cpp
    IO/IOManagerDmsFactory.cpp
    IO/IORefreshToken.cpp
    IO/IOManagerDmsProxy.cpp
    IO/MsiTokenSource.cpp
    IO/DmsIoFactories/AzureDmsIoManagerFactory.cpp
    IO/DmsIoFactories/AwsDmsIoManagerFactory.cpp
    IO/DmsIoFactories/DmsIoManagerFactory.cpp
    IO/DmsIoFactories/AnthosDmsIoManagerFactory.cpp
    IO/DmsIoFactories/IbmDmsIoManagerFactory.cpp
    IO/DmsIoFactories/GcpDmsIoManagerFactory.cpp
    IO/SslVerifyPeerEnv.cpp
    IO/SDPath.cpp
    VDS/VolumeDataPartition.cpp
    VDS/VolumeDataChannelMapping.cpp
    VDS/VolumeDataLayer.cpp
    VDS/VolumeDataLayoutImpl.cpp
    VDS/VolumeDataChunk.cpp
    VDS/VolumeDataRegion.cpp
    VDS/VolumeDataHash.cpp
    VDS/VolumeDataPageAccessorImpl.cpp
    VDS/VolumeDataAccessManagerImpl.cpp
    VDS/VolumeDataPageImpl.cpp
    VDS/VolumeDataAccessor.cpp
    VDS/DimensionGroup.cpp
    VDS/ParseVDSJson.cpp
    VDS/MetadataManager.cpp
    VDS/VolumeDataStore.cpp
    VDS/VolumeDataStoreIOManager.cpp
    VDS/VolumeDataStoreVDSFile.cpp
    VDS/DataBlock.cpp
    VDS/VolumeDataRequestProcessor.cpp
    VDS/VolumeIndexer.cpp
    VDS/Env.cpp
    VDS/StringToDouble.cpp
    VDS/GlobalStateImpl.cpp
    ${WAVELET_SOURCES}
    ${COMMON_DIR}/Base64/Base64.cpp
    )

  set (PRIVATE_HEADER_FILES
    IO/File.h
    IO/IOManager.h
    IO/IOManagerAWSCurl.h
    IO/IOManagerAzure.h
    IO/IOManagerAzureSdkForCpp.h
    IO/IOManagerInMemory.h
    IO/IOManagerCurl.h
    IO/IOManagerAzurePresigned.h
    IO/IOManagerGoogle.h
    IO/IOManagerHttp.h
    IO/IOManagerDmsFactory.h
    IO/IOManagerRequestImpl.h
    IO/IOManagerTransformer.h
    IO/IORefreshToken.h
    IO/SslVerifyPeerEnv.h
    IO/SDPath.h
    IO/ErrorRequest.h
    IO/MsiTokenSource.h
    IO/IOManagerDmsProxy.h
    IO/DmsIoFactories/AzureDmsIoManagerFactory.h
    IO/DmsIoFactories/AwsDmsIoManagerFactory.h
    IO/DmsIoFactories/DmsIoManagerFactory.h
    IO/DmsIoFactories/AnthosDmsIoManagerFactory.h
    IO/DmsIoFactories/IbmDmsIoManagerFactory.h
    IO/DmsIoFactories/GcpDmsIoManagerFactory.h
    VDS/VDS.h
    VDS/VolumeDataPartition.h
    VDS/VolumeDataChannelMapping.h
    VDS/VolumeDataLayer.h
    VDS/VolumeDataLayoutImpl.h
    VDS/VolumeDataChunk.h
    VDS/VolumeDataRegion.h
    VDS/VolumeDataHash.h
    VDS/VolumeDataPageAccessorImpl.h
    VDS/VolumeDataAccessManagerImpl.h
    VDS/VolumeDataAccessor.h
    VDS/VolumeDataPageImpl.h
    VDS/DimensionGroup.h
    VDS/Hash.h
    VDS/Bitmask.h
    VDS/ParseVDSJson.h
    VDS/MetadataManager.h
    VDS/IntrusiveList.h
    VDS/VolumeDataStore.h
    VDS/VolumeDataStoreIOManager.h
    VDS/VolumeDataStoreVDSFile.h
    VDS/DataBlock.h
    VDS/VolumeDataRequestProcessor.h
    VDS/Env.h
    VDS/ConnectionStringParser.h
    VDS/GlobalStateImpl.h
    VDS/StringToDouble.h
    VDS/LayerMetadataContainer.h
    VDS/Logging.h
    VDS/Url.h
    ${WAVELET_HEADERS}
    ${COMMON_DIR}/Base64/Base64.h
    ${COMMON_DIR}/ThreadPool/ThreadPool.h
    json_cpp_include.h
    )
endif()

file(GLOB EXPORTED_HEADER_FILES OpenVDS/*.h)

set_source_files_properties(VDS/FSE/entropy_common.cpp PROPERTIES COMPILE_FLAGS ${DISABLE_WARNING_FLAG})
set_source_files_properties(VDS/FSE/fse_decompress.cpp PROPERTIES COMPILE_FLAGS ${DISABLE_WARNING_FLAG})

if (NOT EMSCRIPTEN)
  # OpenDataStore replaces HueBulkDataStore for main library
  add_library(ods_objects STATIC
    OpenDataStore/OpenDataStore.cpp
    BulkDataStore/VDSObjectParser.cpp
    BulkDataStore/ExtentAllocator.cpp )
  get_target_property(json_cpp_include_dir jsoncpp_lib_static INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(ods_objects PRIVATE ${json_cpp_include_dir})
  target_compile_definitions(ods_objects PRIVATE -DHUEBDS_EXPORTS= -DUSE_OPEN_DATA_STORE=1)
  target_compile_definitions(ods_objects PRIVATE -D_CRT_SECURE_NO_WARNINGS=1)
  setCommonTargetProperties(ods_objects)
  target_link_libraries(ods_objects PRIVATE fmt::fmt)
  target_include_directories(ods_objects
      PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
          $<BUILD_INTERFACE:${COMMON_DIR}>
  )

  # Keep hue_bds_objects for tools/HueBDS standalone tool
  add_library(hue_bds_objects STATIC
    BulkDataStore/HueBulkDataStore.cpp
    BulkDataStore/ExtentAllocator.cpp
    BulkDataStore/VDSObjectParser.cpp )
  target_include_directories(hue_bds_objects PRIVATE ${json_cpp_include_dir})
  target_compile_definitions(hue_bds_objects PRIVATE -DHUEBDS_EXPORTS= )
  target_compile_definitions(hue_bds_objects PRIVATE -D_CRT_SECURE_NO_WARNINGS=1)
  setCommonTargetProperties(hue_bds_objects)
  target_link_libraries(hue_bds_objects PRIVATE fmt::fmt)
  target_include_directories(hue_bds_objects
      PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
          $<BUILD_INTERFACE:${COMMON_DIR}>
  )
endif()


add_library(openvds_objects OBJECT
  ${PRIVATE_HEADER_FILES}
  ${EXPORTED_HEADER_FILES}
  ${SOURCE_FILES} )

target_include_directories(openvds_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${COMMON_DIR}>
        $<INSTALL_INTERFACE:include>
)

if (NOT EMSCRIPTEN)
  compileInTarget(openvds_objects jsoncpp_lib_static)
endif()

if(MSVC_VERSION)
  set_source_files_properties(VDS/VolumeDataAccessor.cpp PROPERTIES COMPILE_FLAGS /bigobj)
endif()

get_property(link_3rdparty GLOBAL PROPERTY OPENVDS_LINK_LIBRARIES)
get_property(runtime_3rdparty GLOBAL PROPERTY OPENVDS_RUNTIME_LIBS)
get_property(include_3rdparty      GLOBAL PROPERTY OPENVDS_INCLUDE_LIBRARIES)
get_property(dependency_targets_3rdparty      GLOBAL PROPERTY OPENVDS_DEPENDENCY_TARGETS)

foreach(DEP ${dependency_targets_3rdparty})
  add_dependencies(openvds_objects ${DEP})
endforeach()

if (BUILD_OPENSSL)
  target_link_libraries(openvds_objects PRIVATE dl)
  if (NOT WIN32)
    target_link_libraries(openvds_objects PRIVATE -pthread)
  endif()
endif()
target_compile_definitions(openvds_objects PRIVATE openvds_EXPORTS)

if (NOT WIN32 AND AUTODETECT_CA_CERTIFICATES)
  target_compile_definitions(openvds_objects PRIVATE AUTODETECT_CA_CERTIFICATES)
endif()

set(OPENVDS_REVISION "")
find_package(Git)
if(Git_FOUND)
  execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      OUTPUT_VARIABLE OPENVDS_REVISION
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()
configure_file(VDS/OpenVDSVersion.h.in OpenVDS/OpenVDSVersion.h)
list(APPEND EXPORTED_HEADER_FILES ${CMAKE_CURRENT_BINARY_DIR}/OpenVDS/OpenVDSVersion.h)

#needed for ENABLE_SSE_TRANSFORM
target_include_directories(openvds_objects PRIVATE ${sse2neon_SOURCE_DIR})
if (ENABLE_SSE4_1 AND NOT (APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64"))
  openvds_target_compile_options(openvds_objects PRIVATE -msse4.1)
endif()

addSystemHeadersToTarget(openvds_objects "${include_3rdparty}")

if (WIN32)
  target_compile_definitions(openvds_objects PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(openvds_objects PRIVATE "iphlpapi")
  target_link_libraries(openvds_objects PRIVATE "userenv")
endif()

if (NOT EMSCRIPTEN)
  if (DISABLE_AWS_IOMANAGER)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_AWS_IOMANAGER)
    set_source_files_properties(IO/IOManagerAWSCurl.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AwsDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/IbmDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AnthosDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  else()
    if (aws-cpp-sdk_EXTERNAL_LIBS)
      target_link_libraries(openvds_objects PRIVATE ${aws-cpp-sdk_EXTERNAL_LIBS})
    endif()
  endif()

  if (BUILD_CURL)
    target_compile_definitions(openvds_objects PRIVATE CURL_STATICLIB=1)
    if(APPLE)
      find_library(COREFOUNDATION_LIBRARY CoreFoundation)
      find_library(SYSTEMCONFIGURATION_LIBRARY SystemConfiguration)
      target_link_libraries(openvds_objects PRIVATE ${COREFOUNDATION_LIBRARY} ${SYSTEMCONFIGURATION_LIBRARY})
    endif()
  endif()

  if ((NOT BUILD_CURL) AND ((NOT DISABLE_AZURESDKFORCPP_IOMANAGER) OR (NOT DISABLE_CURL_IOMANAGER) OR (NOT DISABLE_AZURE_PRESIGNED_IOMANAGER) OR (NOT DISABLE_DMS_IOMANAGER)))
    find_package(CURL REQUIRED)
    target_link_libraries(openvds_objects PRIVATE CURL::libcurl)
  endif()

  if (DISABLE_AZURESDKFORCPP_IOMANAGER)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_AZURE_SDK_FOR_CPP_IOMANAGER)
    set_source_files_properties(IO/IOManagerAzureSdkForCpp.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  else()
    target_link_libraries(openvds_objects PRIVATE azure-core)
  endif()

  if (DISABLE_CURL_IOMANAGER AND DISABLE_AWS_IOMANAGER AND DISABLE_AZURE_PRESIGNED_IOMANAGER AND DISABLE_GCP_IOMANAGER AND DISABLE_DMS_IOMANAGER)
    set_source_files_properties(IO/IOManagerCurl.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  else()
    if (NOT BUILD_UV)
      find_package(LibUV REQUIRED)
      target_link_libraries(openvds_objects PRIVATE LibUV::LibUV)
    endif()
  endif()

  if (DISABLE_CURL_IOMANAGER)
    set_source_files_properties(IO/IOManagerHttp.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_HTTP_IOMANAGER)
  endif()

  if (DISABLE_AZURE_PRESIGNED_IOMANAGER)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER)
    set_source_files_properties(IO/IOManagerAzurePresigned.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AzureDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()

  if (DISABLE_GCP_IOMANAGER)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_GCP_IOMANAGER)
    set_source_files_properties(IO/IOManagerGoogle.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/GcpDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  else()
    add_dependencies(openvds_objects google_cloud_ovds)
    target_include_directories(openvds_objects PRIVATE "${google_nlohmann_SOURCE_DIR}")
  endif()

  if (DISABLE_DMS_IOMANAGER)
    target_compile_definitions(openvds_objects PRIVATE OPENVDS_NO_DMS_IOMANAGER)
    set_source_files_properties(IO/IOManagerDmsFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/IORefreshToken.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/IOManagerDmsProxy.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AzureDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AwsDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/DmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/AnthosDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/IbmDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(IO/DmsIoFactories/GcpDmsIoManagerFactory.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()

  target_link_libraries(openvds_objects PRIVATE Threads::Threads fmt::fmt)
endif()

if (OpenMP_CXX_FOUND)
    target_link_libraries(openvds_objects PRIVATE OpenMP::OpenMP_CXX)
endif()

if (BUILD_ZLIB)
  add_dependencies(openvds_objects zlib)
elseif (EMSCRIPTEN)
  openvds_target_compile_options(openvds_objects PRIVATE -sUSE_ZLIB=1)
else()
  find_package(ZLIB REQUIRED)
  target_link_libraries(openvds_objects PRIVATE ZLIB::ZLIB)
endif()

if (NOT EMSCRIPTEN)
  if (WIN32)
    set_source_files_properties(IO/Linux_File.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
  elseif (UNIX)
    set_source_files_properties(IO/Win_File.cpp PROPERTIES HEADER_FILE_ONLY TRUE)

    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(syncfs "unistd.h" HAVE_SYNCFS)
    set(CMAKE_REQUIRED_DEFINITIONS "")

    if (HAVE_SYNCFS)
      set_source_files_properties(IO/Linux_File.cpp
        PROPERTIES COMPILE_DEFINITIONS
        HAVE_SYNCFS
      )
    endif()
  endif()

  target_link_libraries(openvds_objects PRIVATE ods_objects)
endif()

target_compile_definitions(openvds_objects PRIVATE -DHUEBDS_EXPORTS= -DUSE_OPEN_DATA_STORE=1) # We're not building HueBulkDataStore as a shared library

foreach(item ${link_3rdparty})
  target_link_libraries(openvds_objects PRIVATE ${item})
endforeach()

if (EMSCRIPTEN)
  add_executable(openvds)
  add_executable(openvds::openvds ALIAS openvds)
  openvds_target_compile_options(openvds_objects PRIVATE -msimd128)
  target_link_options(openvds PRIVATE --bind -lz -sMODULARIZE=1 -sSINGLE_FILE=1 -sENVIRONMENT='web,worker' -sALLOW_MEMORY_GROWTH=1)
else()
  add_library(openvds)

  if (WIN32)
    target_link_libraries(openvds_objects PRIVATE crypt32 wsock32 ws2_32)
  elseif (NOT BUILD_OPENSSL)
    target_link_libraries(openvds_objects PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()

  add_library(openvds::openvds ALIAS openvds)
endif()

target_link_libraries(openvds PRIVATE openvds_objects)

setCommonTargetProperties(openvds_objects)
setCommonTargetProperties(openvds)
setExportedHeadersForTarget(openvds ${EXPORTED_HEADER_FILES})


if (ENABLE_RUNPATH_ORIGIN)
set_target_properties(openvds
  PROPERTIES
  INSTALL_RPATH  "$ORIGIN"
)
endif()

target_include_directories(openvds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

install(TARGETS openvds
  EXPORT openvds-export
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}${LIB_TOOLSET_DIR}
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}${LIB_TOOLSET_DIR}
  RUNTIME
    DESTINATION ${CMAKE_INSTALL_BINDIR}${LIB_TOOLSET_DIR}
  FRAMEWORK
    DESTINATION framework
  PUBLIC_HEADER
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/OpenVDS
  PRIVATE_HEADER
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/OpenVDS/private)

install(
    EXPORT openvds-export
    NAMESPACE openvds::
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/cmake/openvds
    FILE openvds-config.cmake
)

export(
    TARGETS
        openvds
    NAMESPACE
        openvds::
    FILE
        openvds-config.cmake
)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR}${LIB_TOOLSET_DIR})
include (InstallRequiredSystemLibraries)

set(OPENVDS_LIB_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR} CACHE PATH "")
