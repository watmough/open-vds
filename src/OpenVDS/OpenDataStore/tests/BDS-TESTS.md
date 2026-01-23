# BulkDataStore API Summary and Test Plan

## Overview

The `source/BulkDataStore/` folder contains the HueBulkDataStore implementation - a file-based storage system for large chunked data. It provides:

- **HueBulkDataStore.h/cpp** - Main data store class and file interface
- **HueBulkDataStoreFormat.h** - Binary format structures (headers, index entries)
- **HueBulkDataStoreFileTypes.h** - File type definitions and metadata structures
- **ExtentAllocator.h/cpp** - Space allocation management within the data store

The data store uses a hierarchical structure:
```
DataStore
  └── FileTable (multiple files)
        └── PageDirectory (per revision)
              └── IndexPages (arrays of IndexEntry + metadata)
                    └── Chunks (raw data blocks)
```

---

## HueBulkDataStore Class API

### Nested Classes

#### `Buffer` (abstract)
```cpp
virtual const void* Data() const = 0;
virtual int Size() const = 0;
```

#### `FileInterface` (abstract) - see detailed API below

### Static Methods

| Method | Description |
|--------|-------------|
| `static HueBulkDataStore* Open(const char* fileName)` | Open existing data store (read-only initially) |
| `static HueBulkDataStore* CreateNew(const char* fileName, bool overwriteExisting)` | Create new data store |
| `static void Close(HueBulkDataStore* hueBulkDataStore)` | Close and delete data store |
| `static void ReleaseBuffer(Buffer* buffer)` | Free buffer memory |

### Instance Methods

#### State Management
| Method | Description |
|--------|-------------|
| `bool IsOpen()` | Check if data store is open |
| `bool IsReadOnly()` | Check if data store is read-only |
| `bool EnableWriting()` | Enable write mode (builds extent allocator) |
| `const char* GetErrorMessage()` | Get last error message (thread-local) |

#### File Enumeration
| Method | Description |
|--------|-------------|
| `int GetFileCount()` | Number of files in the data store |
| `const char* GetFileName(int fileIndex)` | Get file name by index |
| `int GetHeadRevisionNumber(int fileIndex)` | Get latest revision number |
| `int GetFileType(int fileIndex)` | Get file type code |
| `int GetChunkMetadataLength(int fileIndex)` | Get per-chunk metadata size |
| `int GetFileMetadataLength(int fileIndex)` | Get file-level metadata size |

#### File Operations
| Method | Description |
|--------|-------------|
| `FileInterface* OpenFile(const char* fileName)` | Open file at head revision |
| `FileInterface* OpenFileRevision(const char* fileName, int revision)` | Open specific revision |
| `void CloseFile(FileInterface* fileInterface)` | Close file handle |
| `FileInterface* AddFile(const char* fileName, int chunkCount, int indexPageEntryCount, int fileType, int chunkMetadataLength, int fileMetadataLength, bool overwriteExisting)` | Add new file |
| `bool RemoveFile(const char* fileName)` | Remove file from data store |

#### Direct Chunk Access (thread-safe if file I/O is thread-safe)
| Method | Description |
|--------|-------------|
| `void CreateChunkDataIndexEntry(IndexEntry& indexEntry, int size)` | Allocate space for chunk |
| `Buffer* ReadChunkData(IndexEntry const& indexEntry)` | Read chunk by index entry |
| `bool WriteChunkData(IndexEntry const& indexEntry, const void* data, int size)` | Write chunk by index entry |

#### Internal
| Method | Description |
|--------|-------------|
| `ExtentAllocator& GetExtentAllocator()` | Get extent allocator |
| `bool BuildExtentAllocator()` | Build extent map from file |

---

## HueBulkDataStore::FileInterface API

### File Properties
| Method | Description |
|--------|-------------|
| `const char* GetFileName()` | Get file name |
| `int GetRevisionNumber()` | Get current revision number |
| `int GetFileType()` | Get file type code |
| `int GetChunkMetadataLength()` | Get per-chunk metadata size |
| `int GetFileMetadataLength()` | Get file-level metadata size |
| `int GetRevision()` | Get revision number |
| `int GetChunkCount()` | Get total chunk count |

### Index Operations
| Method | Description |
|--------|-------------|
| `int GetIndexPageIndexForChunk(int chunk) const` | Get page index for chunk |
| `int GetIndexEntryIndexForChunk(int chunk) const` | Get entry index within page |
| `bool ReadIndexEntry(int chunk, IndexEntry* indexEntry, void* metadata)` | Read index entry + metadata |
| `bool WriteIndexEntry(int chunk, const IndexEntry& indexEntry, const void* metadata)` | Write index entry |
| `bool WriteIndexEntry(int chunk, const IndexEntry& indexEntry, const void* metadata, int* oldSize, void* oldMetadata)` | Write with old value retrieval |

### Chunk Operations
| Method | Description |
|--------|-------------|
| `Buffer* ReadChunk(int chunk, void* metadata)` | Read chunk data + metadata |
| `Buffer* ReadChunkData(int chunk)` | Read chunk data only |
| `bool ReadChunkMetadata(int chunk, void* metadata)` | Read chunk metadata only |
| `bool WriteChunk(int chunk, const void* data, int size, const void* metadata)` | Write chunk data + metadata |
| `bool WriteChunk(int chunk, const void* data, int size, const void* metadata, int* oldSize, void* oldMetadata)` | Write with old value retrieval |
| `bool WriteChunkData(int chunk, const void* data, int size)` | Write chunk data only |
| `bool WriteChunkMetadata(int chunk, const void* metadata)` | Write metadata only |
| `bool WriteChunkMetadata(int chunk, const void* metadata, int* oldSize, void* oldMetadata)` | Write with old value retrieval |

### File Metadata
| Method | Description |
|--------|-------------|
| `void ReadFileMetadata(void* fileMetadata)` | Read file-level metadata |
| `void WriteFileMetadata(const void* fileMetadata)` | Write file-level metadata |
| `void Rename(const char* fileName)` | Rename the file |

### Commit
| Method | Description |
|--------|-------------|
| `bool Commit()` | Commit all pending changes |

---

## Data Structures (HueBulkDataStoreFormat.h)

```cpp
struct DataStoreHeader {
    char     m_magic[12];        // "HueDataStore"
    int32_t  m_version;          // (major << 16) + minor
    int64_t  m_fileTableOffset;
    int32_t  m_fileTableCount;
    int32_t  m_fileNameLength;
};

struct FileHeader {
    int64_t  m_headPageDirectoryOffset;
    int32_t  m_headChunkCount;
    int32_t  m_headRevisionNumber;
    int32_t  m_indexPageEntryCount;
    int32_t  m_fileType;
    int32_t  m_chunkMetadataLength;
    int32_t  m_fileMetadataLength;
};

struct PageDirectory {
    int64_t  m_previousPageDirectoryOffset;
    int32_t  m_previousChunkCount;
    int32_t  m_previousRevisionNumber;
};

struct IndexEntry {
    int64_t  m_offset;
    int32_t  m_length;
    int32_t  m_reserved;
};
```

---

## GTest Implementation Plan

### Test Fixture Setup

```cpp
class HueBulkDataStoreTest : public ::testing::Test {
protected:
    std::string testFilePath;

    void SetUp() override {
        testFilePath = "/tmp/test_datastore_" + std::to_string(getpid()) + ".bds";
    }

    void TearDown() override {
        std::remove(testFilePath.c_str());
    }
};
```

### Test Categories

#### 1. DataStore Lifecycle Tests
- `CreateNew_ValidPath_ReturnsValidDataStore`
- `CreateNew_OverwriteExisting_Succeeds`
- `CreateNew_NoOverwrite_ExistingFile_Fails`
- `Open_ValidFile_ReturnsValidDataStore`
- `Open_NonExistentFile_ReturnsInvalidState`
- `Open_InvalidMagic_ReturnsError`
- `Close_ValidDataStore_ClosesSuccessfully`
- `IsOpen_AfterCreate_ReturnsTrue`
- `IsOpen_AfterClose_ReturnsFalse`

#### 2. Read-Only vs Writable Mode Tests
- `IsReadOnly_AfterOpen_ReturnsTrue`
- `IsReadOnly_AfterEnableWriting_ReturnsFalse`
- `EnableWriting_Success_ReturnsTrue`
- `WriteOperation_WhileReadOnly_Fails`

#### 3. File Management Tests
- `AddFile_NewFile_IncrementsFileCount`
- `AddFile_WithOverwrite_ReplacesExisting`
- `AddFile_NoOverwrite_ExistingFile_Fails`
- `AddFile_InvalidChunkCount_Fails`
- `AddFile_InvalidIndexPageEntryCount_Fails`
- `RemoveFile_ExistingFile_Succeeds`
- `RemoveFile_NonExistentFile_Fails`
- `GetFileCount_MultipleFiles_ReturnsCorrectCount`
- `GetFileName_ValidIndex_ReturnsCorrectName`
- `OpenFile_NonExistentFile_ReturnsNull`

#### 4. Chunk Read/Write Tests
- `WriteChunk_ValidData_Succeeds`
- `ReadChunkData_AfterWrite_ReturnsCorrectData`
- `WriteChunk_WithMetadata_StoresMetadata`
- `ReadChunkMetadata_AfterWrite_ReturnsCorrectMetadata`
- `WriteChunk_ZeroSize_HandlesCorrectly`
- `ReadChunk_InvalidIndex_ReturnsNull`
- `WriteChunk_MultipleTimes_AllDataPersisted`

#### 5. Index Entry Tests
- `WriteIndexEntry_ValidEntry_Succeeds`
- `ReadIndexEntry_AfterWrite_ReturnsCorrectEntry`
- `WriteIndexEntry_RetrieveOldValues_ReturnsOld`
- `GetIndexPageIndexForChunk_ReturnsCorrectPage`
- `GetIndexEntryIndexForChunk_ReturnsCorrectIndex`

#### 6. File Metadata Tests
- `WriteFileMetadata_ValidMetadata_Succeeds`
- `ReadFileMetadata_AfterWrite_ReturnsCorrectData`
- `Rename_ValidName_UpdatesFileName`

#### 7. Revision Tests
- `Commit_CreatesNewRevision`
- `OpenFileRevision_SpecificRevision_OpensCorrectRevision`
- `OpenFileRevision_NonExistentRevision_ReturnsNull`
- `GetHeadRevisionNumber_AfterCommit_Increments`

#### 8. Buffer Management Tests
- `ReleaseBuffer_ValidBuffer_FreesMemory`
- `Buffer_Data_ReturnsValidPointer`
- `Buffer_Size_ReturnsCorrectSize`

#### 9. ExtentAllocator Tests
- `Allocate_ValidSize_ReturnsValidOffset`
- `Allocate_MultipleAllocations_NoOverlap`
- `AddReference_ValidExtent_IncrementsRefCount`
- `Allocate_ReusesFreedSpace`

#### 10. Error Handling Tests
- `GetErrorMessage_AfterError_ReturnsDescription`
- `WriteChunk_FullDisk_ReturnsError` (may need mocking)
- `ReadChunk_CorruptedIndex_HandlesGracefully`

#### 11. Persistence Tests
- `CreateAndClose_ReopenAndRead_DataPersisted`
- `MultipleCommits_AllRevisionsAccessible`
- `LargeChunks_PersistCorrectly`

#### 12. Edge Cases
- `EmptyDataStore_GetFileCount_ReturnsZero`
- `MaxChunkCount_HandledCorrectly`
- `LongFileName_HandledCorrectly`
- `SpecialCharactersInFileName_HandledCorrectly`

### Implementation Files Needed

```
tests/
├── BDS-TESTS.md          (this file)
├── CMakeLists.txt        (gtest configuration)
├── HueBulkDataStoreTest.cpp
├── FileInterfaceTest.cpp
├── ExtentAllocatorTest.cpp
└── TestHelpers.h         (common fixtures and utilities)
```

### CMakeLists.txt Addition

```cmake
# Add Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(bds_tests
    tests/HueBulkDataStoreTest.cpp
    tests/FileInterfaceTest.cpp
    tests/ExtentAllocatorTest.cpp
)

target_link_libraries(bds_tests
    PRIVATE
    hue_bds_objects
    GTest::gtest_main
    fmt::fmt
)

include(GoogleTest)
gtest_discover_tests(bds_tests)
```

### Priority Order for Implementation

1. **High Priority** - Core functionality
   - DataStore lifecycle (create, open, close)
   - Basic file add/open/close
   - Chunk read/write
   - Commit and persistence

2. **Medium Priority** - Extended functionality
   - Revision handling
   - File metadata
   - Index entry operations
   - Error handling

3. **Lower Priority** - Edge cases
   - ExtentAllocator unit tests
   - Concurrent access scenarios
   - Large data handling
   - Stress tests

---

## OpenDataStore Allocation Strategy Investigation

### Background

After implementing OpenDataStore as a clean reimplementation of HueBulkDataStore, stress testing revealed significant fragmentation issues when rewriting chunks with larger data. A test was created that reads all chunks from a VDS file and rewrites them with 10% more data, measuring the ratio of file size growth to data size growth.

**Original Results (efficiency ratio = file growth / data growth):**
- BDS (HueBulkDataStore): 9.92x (small), 11.22x (medium), 11.29x (large)
- ODS baseline: 6.82x (small), 8.79x (medium), 3.32x (large)

Lower ratio = better. A ratio of 1.0 would mean the file grows exactly as much as the data.

### Allocation Strategies Tested

Five different allocation strategies were implemented and tested:

| # | Strategy | Description | Files |
|---|----------|-------------|-------|
| 00 | Original | Initial baseline before optimizations | `-00-ORIGINAL` |
| 01 | Baseline | Best-fit allocation with extent coalescing | `-01-BASELINE-COALESCE-BESTFIT` |
| 02 | Size Classes | Power-of-2 size buckets (256B to 2GB) | `-02-SIZE-CLASSES` |
| 03 | Overallocation | 25% slack space for in-place growth | `-03-OVERALLOCATION` |
| 04 | Buddy System | Power-of-2 blocks with buddy merging | `-04-BUDDY-SYSTEM` |
| 05 | Compaction | Baseline + Compact() defragmentation method | `-05-COMPACTION` |

### Results Summary

| Strategy | Small VDS | Medium VDS | Large VDS | Notes |
|----------|-----------|------------|-----------|-------|
| BDS (original) | 9.92x | 11.22x | 11.29x | Reference implementation |
| Baseline | 6.82x | 8.79x | 3.32x | Best-fit + coalescing |
| Size Classes | 3.10x | 0.01x | 2.40x | Good for varied sizes |
| **Overallocation** | **1.16x** | **0.22x** | **0.21x** | **Best overall** |
| Buddy System | 6.97x | 0.01x | 2.60x | Classic buddy allocator |
| Compaction | 4.45x* | - | - | *After compacting baseline |

### Key Findings

1. **Overallocation (25% slack) is the clear winner**
   - Allocates 25% more space than requested
   - Allows chunks to grow up to 25% without reallocation
   - Achieved 0.21x efficiency on large files (50x better than BDS)
   - Trade-off: Uses ~25% more disk space upfront

2. **Size Classes work well for certain workloads**
   - Power-of-2 buckets reduce external fragmentation
   - Excellent for medium files (0.01x)
   - Some internal fragmentation for non-power-of-2 sizes

3. **Compaction can recover wasted space**
   - Improved efficiency from 6.82x to 4.45x after compaction
   - Useful for periodic maintenance windows
   - Can be combined with any base allocation strategy

4. **Buddy System has trade-offs**
   - Good merging behavior when blocks are freed
   - Wastes space for non-power-of-2 sizes
   - Better suited for memory allocators than file stores

### Implementation Details

#### Overallocation Strategy (Winner)
```cpp
const double OVERALLOC_FACTOR = 1.25;

inline int64_t allocSizeWithSlack(int64_t requestedSize) {
    int64_t withSlack = static_cast<int64_t>(requestedSize * OVERALLOC_FACTOR);
    return align8(withSlack);
}
```

Key changes:
- `allocateExtent()` allocates `allocSizeWithSlack(size)` instead of just `size`
- `WriteChunkData()` checks if new data fits in existing slack before reallocating
- `freeExtent()` frees the full overallocated size

#### Compact() Method
Added `virtual int64_t Compact() = 0` to OpenDataStore API:
- Collects all chunk locations from all files
- Sorts by offset and packs sequentially after metadata
- Updates all index entries to point to new locations
- Truncates file to reclaim space
- Returns bytes reclaimed

### Files Created

Alternate implementations stored in `source/OpenDataStore-Alt-Implementations/`:
```
OpenDataStore.cpp-00-ORIGINAL
OpenDataStore.cpp-01-BASELINE-COALESCE-BESTFIT
OpenDataStore.cpp-02-SIZE-CLASSES
OpenDataStore.cpp-03-OVERALLOCATION          <- Winning implementation
OpenDataStore.cpp-04-BUDDY-SYSTEM
OpenDataStore.cpp-05-COMPACTION
```

The winning Overallocation implementation is now the active `OpenDataStore.cpp`.

### Test Files

Stress tests added to `tests/ODSStressTests.cpp`:
- `RewriteAllChunksWithLargerData` - Small VDS test
- `RewriteAllChunksWithLargerData_MediumFile` - Medium VDS test
- `RewriteAllChunksWithLargerData_LargeFile` - Large VDS test

Equivalent tests added for BDS in `tests/BDSStressTests.cpp` for comparison.

### Recommendations

1. **Use Overallocation** for workloads where chunks may grow incrementally
2. **Consider Size Classes** for workloads with highly variable chunk sizes
3. **Run Compact()** periodically during maintenance windows if fragmentation accumulates
4. **Monitor efficiency ratio** in production to detect fragmentation issues early
