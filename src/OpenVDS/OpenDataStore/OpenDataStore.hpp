#ifndef OPENDATASTORE_HPP
#define OPENDATASTORE_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>

// On-disk format structures (must match HueBulkDataStoreFormat.h for compatibility)
struct ODSDataStoreHeader {
    char     m_magic[12];        // "HueDataStore"
    int32_t  m_version;          // (major << 16) + minor
    int64_t  m_fileTableOffset;
    int32_t  m_fileTableCount;
    int32_t  m_fileNameLength;
};

struct ODSFileHeader {
    int64_t  m_headPageDirectoryOffset;
    int32_t  m_headChunkCount;
    int32_t  m_headRevisionNumber;
    int32_t  m_indexPageEntryCount;
    int32_t  m_fileType;
    int32_t  m_chunkMetadataLength;
    int32_t  m_fileMetadataLength;
};

struct ODSPageDirectory {
    int64_t  m_previousPageDirectoryOffset;
    int32_t  m_previousChunkCount;
    int32_t  m_previousRevisionNumber;
};

struct ODSIndexEntry {
    int64_t  m_offset;
    int32_t  m_length;
    int32_t  m_reserved;
};

class OpenDataStore {
public:
    // Buffer class for chunk data
    class Buffer {
    public:
        virtual ~Buffer() = default;
        virtual const void* Data() const = 0;
        virtual int Size() const = 0;
    };

    // FileInterface for per-file operations
    class FileInterface {
    public:
        virtual ~FileInterface() = default;

        virtual const char* GetFileName() = 0;
        virtual int GetRevisionNumber() = 0;
        virtual int GetFileType() = 0;
        virtual int GetChunkMetadataLength() = 0;
        virtual int GetFileMetadataLength() = 0;

        virtual Buffer* ReadChunk(int chunk, void* metadata) = 0;
        virtual Buffer* ReadChunkData(int chunk) = 0;
        virtual bool ReadChunkMetadata(int chunk, void* metadata) = 0;
        virtual bool ReadIndexEntry(int chunk, ODSIndexEntry* indexEntry, void* metadata) = 0;

        virtual bool WriteChunk(int chunk, const void* data, int size, const void* metadata, int* oldSize, void* oldMetadata) = 0;
        bool WriteChunk(int chunk, const void* data, int size, const void* metadata) {
            return WriteChunk(chunk, data, size, metadata, nullptr, nullptr);
        }
        virtual bool WriteChunkData(int chunk, const void* data, int size) = 0;
        virtual bool WriteChunkMetadata(int chunk, const void* metadata, int* oldSize, void* oldMetadata) = 0;
        bool WriteChunkMetadata(int chunk, const void* metadata) {
            return WriteChunkMetadata(chunk, metadata, nullptr, nullptr);
        }
        virtual bool WriteIndexEntry(int chunk, const ODSIndexEntry& indexEntry, const void* metadata, int* oldSize, void* oldMetadata) = 0;
        bool WriteIndexEntry(int chunk, const ODSIndexEntry& indexEntry, const void* metadata) {
            return WriteIndexEntry(chunk, indexEntry, metadata, nullptr, nullptr);
        }

        virtual int GetRevision() = 0;
        virtual int GetChunkCount() = 0;
        virtual void ReadFileMetadata(void* fileMetadata) = 0;
        virtual void WriteFileMetadata(const void* fileMetadata) = 0;
        virtual void Rename(const char* fileName) = 0;

        virtual int GetIndexPageIndexForChunk(int chunk) const = 0;
        virtual int GetIndexEntryIndexForChunk(int chunk) const = 0;

        virtual bool Commit() = 0;
    };

    virtual ~OpenDataStore() = default;

    virtual const char* GetErrorMessage() = 0;
    virtual bool BuildExtentAllocator() = 0;
    virtual bool EnableWriting() = 0;

    virtual int GetFileCount() = 0;
    virtual const char* GetFileName(int fileIndex) = 0;
    virtual int GetHeadRevisionNumber(int fileIndex) = 0;
    virtual int GetFileType(int fileIndex) = 0;
    virtual int GetChunkMetadataLength(int fileIndex) = 0;
    virtual int GetFileMetadataLength(int fileIndex) = 0;

    virtual FileInterface* OpenFile(const char* fileName) = 0;
    virtual FileInterface* OpenFileRevision(const char* fileName, int revision) = 0;
    virtual void CloseFile(FileInterface* fileInterface) = 0;

    virtual FileInterface* AddFile(const char* fileName, int chunkCount, int indexPageEntryCount,
                                   int fileType, int chunkMetadataLength, int fileMetadataLength,
                                   bool overwriteExisting) = 0;
    virtual bool RemoveFile(const char* fileName) = 0;
    virtual bool IsOpen() = 0;
    virtual bool IsReadOnly() = 0;

    virtual void CreateChunkDataIndexEntry(ODSIndexEntry& indexEntry, int size) = 0;
    virtual Buffer* ReadChunkData(const ODSIndexEntry& indexEntry) = 0;
    virtual bool WriteChunkData(const ODSIndexEntry& indexEntry, const void* data, int size) = 0;

    // Static factory methods
    static OpenDataStore* Open(const char* fileName);
    static OpenDataStore* CreateNew(const char* fileName, bool overwriteExisting);
    static void Close(OpenDataStore* dataStore);
    static void ReleaseBuffer(Buffer* buffer);
};

// Provide typedefs for compatibility with test code expecting IndexEntry
using IndexEntry = ODSIndexEntry;

#endif // OPENDATASTORE_HPP
