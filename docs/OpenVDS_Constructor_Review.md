# OpenVDS Constructor Code Review

This document summarizes the code flow for opening VDS files, handling OpenOptions, and initializing HueBulkDataStore.

## 1. Entry Points - Open() Functions

**File**: `src/OpenVDS/OpenVDS.h` (lines 623-747)

The API provides multiple overloaded `Open()` functions:

```cpp
// Open with URL and connection string
VDSHandle Open(std::string url, std::string connectionString, Error& error)

// Open with adaptive compression tolerance
VDSHandle OpenWithAdaptiveCompressionTolerance(std::string url, std::string connectionString,
                                                float waveletAdaptiveTolerance, Error& error)

// Open with adaptive compression ratio
VDSHandle OpenWithAdaptiveCompressionRatio(std::string url, std::string connectionString,
                                            float waveletAdaptiveRatio, Error& error)

// Open with OpenOptions object
VDSHandle Open(const OpenOptions& options, Error& error)

// Open with IOManager directly
VDSHandle Open(IOManager* ioManager, Error& error)
```

All wrapper functions delegate to `GetOpenVDSInterface(OPENVDS_VERSION)` which returns the singleton `OpenVDSInterfaceImpl` instance.

---

## 2. OpenOptions Structure

**File**: `src/OpenVDS/OpenVDS.h` (lines 79-589)

### Base Class

```cpp
struct OpenOptions
{
  enum ConnectionType
  {
    AWS,
    Azure,
    AzureSdkForCpp,
    AzurePresigned,
    GoogleStorage,
    DMS,
    Http,
    VDSFile,        // Local file format
    InMemory,
    Other,
    ConnectionTypeCount
  };

  ConnectionType connectionType;
  WaveletAdaptiveMode waveletAdaptiveMode;
  float waveletAdaptiveTolerance;
  float waveletAdaptiveRatio;
  int requestThreadCount;
  LogLevel logLevel;
};
```

### Specialized Subclasses

| Class | Purpose |
|-------|---------|
| `AWSOpenOptions` | S3 bucket/key + authentication |
| `AzureOpenOptions` | Azure Blob Storage |
| `AzurePresignedOpenOptions` | Azure with presigned URLs |
| `GoogleOpenOptions` | Google Cloud Storage |
| `DMSOpenOptions` | OSDU Data Management Service |
| `HttpOpenOptions` | HTTP URL-based access |
| `VDSFileOpenOptions` | Local VDS file (HueBulkDataStore) |
| `InMemoryOpenOptions` | In-memory storage |

---

## 3. URL to OpenOptions Parsing

**File**: `src/OpenVDS/OpenVDS.cpp` (lines 464-637)

### Protocol Mapping

```cpp
static const std::vector<UrlToOpenOptions> urlToOpenOptions = {
  {"s3://",        &removeProtocol, &createS3OpenOptions },
  {"az://",        &removeProtocol, &createAzureOpenOptions },
  {"azure://",     &removeProtocol, &createAzureOpenOptions },
  {"azuresas://",  &removeProtocol, &createAzureSASOpenOptions },
  {"gs://",        &removeProtocol, &createGoogleOpenOptions },
  {"sd://",        nullptr,         &createDMSOpenOptions },
  {"http://",      nullptr,         &createHttpOpenOptions },
  {"https://",     nullptr,         &createHttpOpenOptions },
  {"file://",      &removeProtocol, &createVDSFileOpenOptions },
  {"inmemory://",  &removeProtocol, &createInMemoryOpenOptions}
};
```

### CreateOpenOptions() Flow

1. Parse wavelet adaptive mode/tolerance/ratio from connection string
2. Parse log level from connection string
3. Parse request thread count from connection string
4. Match URL protocol to creator function
5. If no protocol matched, default to `VDSFileOpenOptions`
6. Apply parsed settings to the created OpenOptions

---

## 4. Main VDS Opening Flow

**File**: `src/OpenVDS/OpenVDS.cpp` (lines 1151-1185)

```cpp
VDS* OpenVDSInterfaceImpl::Open(const OpenOptions& options, Error& error)
{
  std::unique_ptr<VDS> ret(new VDS(options.requestThreadCount, options.logLevel));
  std::unique_ptr<VolumeDataStore> volumeDataStore;

  if (options.connectionType != OpenOptions::VDSFile)
  {
    // CLOUD STORAGE PATH - Uses IOManager
    std::unique_ptr<IOManager> ioManager(
      IOManager::CreateIOManager(options, IOManager::AccessPattern::ReadOnly, error)
    );
    volumeDataStore.reset(
      new VolumeDataStoreIOManager(*ret, ioManager.release(), IOManager::ReadOnly)
    );
  }
  else
  {
    // LOCAL VDS FILE PATH - Uses HueBulkDataStore
    const VDSFileOpenOptions& fileOptions = static_cast<const VDSFileOpenOptions&>(options);
    volumeDataStore.reset(
      new VolumeDataStoreVDSFile(*ret, fileOptions.fileName, VolumeDataStoreVDSFile::ReadOnly, error)
    );
  }

  // Initialize VDS with the volume data store
  if (Init(ret.get(), volumeDataStore.release(), error))
  {
    InitWaveletAdaptiveLoadLevel(*ret.get(), options);
    return ret.release();
  }
  return nullptr;
}
```

---

## 5. IOManager Factory

**File**: `src/OpenVDS/IO/IOManager.cpp` (lines 49-99)

The factory creates storage backend IOManagers based on connection type:

```cpp
IOManager* IOManager::CreateIOManager(const OpenOptions& options,
                                       IOManager::AccessPattern accessPattern,
                                       Error& error)
{
  switch(options.connectionType)
  {
    case OpenOptions::AWS:           return IOManagerAWSCurl::CreateIOManagerAWSCurl(...);
    case OpenOptions::Azure:         return IOManagerAzureSdkForCpp::CreateIOManagerAzureSdkForCpp(...);
    case OpenOptions::AzurePresigned: return IOManagerAzurePresigned::CreateIOManagerAzurePresigned(...);
    case OpenOptions::GoogleStorage: return IOManagerGoogle::CreateIOManagerGoogle(...);
    case OpenOptions::Http:          return IOManagerHttp::CreateIOManagerHttp(...);
    case OpenOptions::DMS:           return CreateDMSIOManager(...);
    case OpenOptions::InMemory:      return IOManagerInMemory::CreateIOManagerInMemory(...);
    default:
      error.code = -1;
      error.string = "Unknown type for OpenOptions";
      return nullptr;
  }
}
```

### Conditional Compilation

IOManagers can be disabled at compile time via preprocessor flags:

| Flag | Effect |
|------|--------|
| `OPENVDS_NO_AWS_IOMANAGER` | Disables AWS S3 support |
| `OPENVDS_NO_AZURE_SDK_FOR_CPP_IOMANAGER` | Disables Azure Blob Storage |
| `OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER` | Disables Azure presigned URLs |
| `OPENVDS_NO_GCP_IOMANAGER` | Disables Google Cloud Storage |
| `OPENVDS_NO_HTTP_IOMANAGER` | Disables HTTP backend |
| `OPENVDS_NO_DMS_IOMANAGER` | Disables OSDU/DMS support |

---

## 6. HueBulkDataStore Initialization

**File**: `src/OpenVDS/VDS/VolumeDataStoreVDSFile.cpp` (lines 606-678)

### VolumeDataStoreVDSFile Constructor

```cpp
VolumeDataStoreVDSFile::VolumeDataStoreVDSFile(VDS& vds, const std::string& vdsFileName,
                                                Mode mode, Error& error)
{
  if (mode == Mode::Create)
  {
    m_dataStore.reset(HueBulkDataStore::CreateNew(vdsFileName.c_str(), true));
  }
  else
  {
    m_dataStore.reset(HueBulkDataStore::Open(vdsFileName.c_str()));

    if (mode == ReadWrite && !m_dataStore->IsOpen())
    {
      m_dataStore.reset(HueBulkDataStore::CreateNew(vdsFileName.c_str(), false));
    }
    else if (mode == ReadWrite)
    {
      m_dataStore->EnableWriting();
    }
  }

  // Initialize layer files from HueBulkDataStore
  if (m_dataStore->IsOpen())
  {
    int fileCount = m_dataStore->GetFileCount();
    for (int fileIndex = 0; fileIndex < fileCount; fileIndex++)
    {
      std::string fileName(m_dataStore->GetFileName(fileIndex));
      int fileType = m_dataStore->GetFileType(fileIndex);

      if (fileType == FILETYPE_VDS_LAYER)
      {
        // Open and cache layer file interface
        HueBulkDataStore::FileInterface* fileInterface =
          m_dataStore->OpenFile(m_dataStore->GetFileName(fileIndex));
        m_layerFiles[fileName] = LayerFile(fileInterface, ...);
      }
      else if (fileType == FILETYPE_HUE_OBJECT)
      {
        m_isVDSObjectFilePresent = (fileName == "VDSObject");
      }
      else if (fileType == FILETYPE_JSON_OBJECT)
      {
        m_isVolumeDataLayoutFilePresent = (fileName == "VolumeDataLayout");
      }
    }
  }
}
```

---

## 7. HueBulkDataStore Class

**File**: `src/OpenVDS/BulkDataStore/HueBulkDataStore.h` (lines 38-127)

### Key Interfaces

```cpp
class HueBulkDataStore
{
  class FileInterface
  {
    virtual const char* GetFileName() = 0;
    virtual int GetFileType() = 0;
    virtual int GetChunkCount() = 0;

    virtual Buffer* ReadChunk(int chunk, void* metadata) = 0;
    virtual Buffer* ReadChunkData(int chunk) = 0;
    virtual bool ReadChunkMetadata(int chunk, void* metadata) = 0;

    virtual bool WriteChunk(int chunk, const void* data, int size, ...) = 0;
    virtual bool Commit() = 0;
  };

  static HueBulkDataStore* Open(const char* fileName);
  static HueBulkDataStore* CreateNew(const char* fileName, bool overwriteExisting);
  static void Close(HueBulkDataStore* hueBulkDataStore);

  bool IsOpen();
  int GetFileCount();
  const char* GetFileName(int fileIndex);
  int GetFileType(int fileIndex);
  FileInterface* OpenFile(const char* fileName);
};
```

---

## 8. VDS Initialization

**File**: `src/OpenVDS/OpenVDS.cpp` (lines 639-686)

### Init() Function

```cpp
static bool Init(VDS* vds, VolumeDataStore* volumeDataStore, Error& error)
{
  vds->volumeDataStore.reset(volumeDataStore);

  // Read and parse volume data layout
  std::vector<uint8_t> serializedVolumeDataLayout;
  if (!vds->volumeDataStore->ReadSerializedVolumeDataLayout(serializedVolumeDataLayout, error))
    return false;

  if (!ParseVolumeDataLayout(serializedVolumeDataLayout, vds->layoutDescriptor,
                             vds->axisDescriptors, vds->channelDescriptors, ...))
    return false;

  CreateVolumeDataLayout(*vds);

  // Create access manager
  vds->accessManager = std::shared_ptr<VolumeDataAccessManagerImpl>(
    VolumeDataAccessManagerImpl::Create(*vds), ...
  );

  return true;
}
```

### Wavelet Adaptive Initialization

```cpp
void InitWaveletAdaptiveLoadLevel(VDS& vds, const OpenOptions& options)
{
  VolumeDataLayer* volumeDataLayer = FindCachedLayer(vds);

  if (volumeDataLayer && volumeDataLayer->GetProduceStatus() == ProduceStatus_Normal)
  {
    CompressionInfo compressionInfo = vds.volumeDataStore->GetEffectiveAdaptiveLevel(
      volumeDataLayer,
      options.waveletAdaptiveMode,
      options.waveletAdaptiveTolerance,
      options.waveletAdaptiveRatio
    );

    vds.volumeDataLayout->SetCompressionMethod(compressionInfo.GetCompressionMethod());
    vds.volumeDataLayout->SetCompressionTolerance(compressionInfo.GetTolerance());
    vds.volumeDataLayout->SetWaveletAdaptiveLoadLevel(compressionInfo.GetAdaptiveLevel());
  }
}
```

---

## 9. Design Patterns

| Pattern | Usage |
|---------|-------|
| **Factory** | `IOManager::CreateIOManager()` creates backend-specific managers |
| **Strategy** | Different storage backends (AWS, Azure, Google, HTTP, Local file) |
| **Template Method** | `VolumeDataStore` base class with backend-specific implementations |
| **Proxy** | `VolumeDataStoreIOManager` wraps IOManager for cloud storage |
| **RAII** | Unique pointers for automatic resource cleanup |
| **Handle-Based API** | VDSHandle for opaque pointer management across DLL boundaries |

---

## 10. Code Flow Diagram

```
User calls Open(url, connectionString)
         │
         ▼
CreateOpenOptions(url, connectionString)
         │
         ├─► Parse protocol from URL
         │   └─► Match to urlToOpenOptions table
         │       └─► Create specific OpenOptions subclass
         │
         ├─► Parse connection string parameters
         │   ├─► Wavelet adaptive mode/tolerance/ratio
         │   ├─► Log level
         │   └─► Request thread count
         │
         ▼
OpenVDSInterfaceImpl::Open(options)
         │
         ├─► If VDSFile:
         │   └─► VolumeDataStoreVDSFile(fileName)
         │       └─► HueBulkDataStore::Open(fileName)
         │           └─► Initialize layer files
         │
         └─► If Cloud Storage:
             └─► IOManager::CreateIOManager(options)
                 └─► VolumeDataStoreIOManager(ioManager)
         │
         ▼
Init(vds, volumeDataStore)
         │
         ├─► ReadSerializedVolumeDataLayout()
         ├─► ParseVolumeDataLayout()
         ├─► CreateVolumeDataLayout()
         └─► Create VolumeDataAccessManagerImpl
         │
         ▼
InitWaveletAdaptiveLoadLevel()
         │
         ▼
Return VDSHandle
```

---

## 11. Key File Locations

| Component | Location |
|-----------|----------|
| Main API Headers | `src/OpenVDS/OpenVDS/` |
| OpenOptions & Open() | `src/OpenVDS/OpenVDS.h`, `src/OpenVDS/OpenVDS.cpp` |
| IOManager Factory | `src/OpenVDS/IO/IOManager.cpp` |
| HueBulkDataStore | `src/OpenVDS/BulkDataStore/HueBulkDataStore.h` |
| VolumeDataStoreVDSFile | `src/OpenVDS/VDS/VolumeDataStoreVDSFile.cpp` |
| VolumeDataStoreIOManager | `src/OpenVDS/VDS/VolumeDataStoreIOManager.cpp` |
