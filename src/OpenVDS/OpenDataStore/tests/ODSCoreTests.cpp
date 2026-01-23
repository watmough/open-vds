// ODSCoreTests.cpp - Core functionality tests for OpenDataStore
// Tests: lifecycle, file enumeration, basic file operations, chunk reading

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <string>
#include "OpenDataStore.hpp"

class ODSCoreTest : public ::testing::Test {
protected:
    // Path to read-only test VDS files
    const std::string kTestDataDir = "example-vds-data/";
    const std::string kSmallVDS = kTestDataDir + "85-34_mig_Time.vds";
    const std::string kMediumVDS = kTestDataDir + "Hor2.vds";

    // Temp file for write tests
    std::string tempFile;

    void SetUp() override {
        tempFile = "/tmp/ods_core_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(rand()) + ".vds";
    }

    void TearDown() override {
        std::remove(tempFile.c_str());
    }
};

// ============================================================================
// DataStore Lifecycle Tests
// ============================================================================

TEST_F(ODSCoreTest, OpenValidFile) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr) << "Failed to open valid VDS file";
    EXPECT_TRUE(ds->IsOpen());
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, OpenInvalidFile) {
    OpenDataStore *ds = OpenDataStore::Open("/nonexistent/path.vds");
    // API returns a datastore object but IsOpen() will be false
    if (ds != nullptr) {
        EXPECT_FALSE(ds->IsOpen()) << "Datastore for invalid file should not be open";
        OpenDataStore::Close(ds);
    }
    // Either nullptr or non-open datastore is acceptable
}

TEST_F(ODSCoreTest, CreateNewDataStore) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr) << "Failed to create new datastore";
    EXPECT_TRUE(ds->IsOpen());
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, CreateNewOverwriteExisting) {
    // Create first datastore
    OpenDataStore *ds1 = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds1, nullptr);
    OpenDataStore::Close(ds1);

    // Create second datastore with overwrite
    OpenDataStore *ds2 = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds2, nullptr) << "Failed to overwrite existing datastore";
    OpenDataStore::Close(ds2);
}

TEST_F(ODSCoreTest, CloseDataStore) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    OpenDataStore::Close(ds);
    // No crash means success - ds is now invalid
}

// ============================================================================
// Read-Only Mode Tests
// ============================================================================

TEST_F(ODSCoreTest, OpenedFileIsReadOnly) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    EXPECT_TRUE(ds->IsReadOnly()) << "Opened file should be read-only by default";
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, CreatedFileIsNotReadOnly) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);
    EXPECT_FALSE(ds->IsReadOnly()) << "Created file should not be read-only";
    OpenDataStore::Close(ds);
}

// ============================================================================
// File Enumeration Tests
// ============================================================================

TEST_F(ODSCoreTest, GetFileCount) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    EXPECT_GT(fileCount, 0) << "VDS file should contain at least one file/layer";

    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, GetFileName) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    for (int i = 0; i < fileCount; i++) {
        const char* fileName = ds->GetFileName(i);
        EXPECT_NE(fileName, nullptr) << "File name at index " << i << " should not be null";
        EXPECT_GT(strlen(fileName), 0u) << "File name at index " << i << " should not be empty";
    }

    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, GetFileType) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    for (int i = 0; i < fileCount; i++) {
        int fileType = ds->GetFileType(i);
        EXPECT_GE(fileType, 0) << "File type should be non-negative";
    }

    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, GetHeadRevisionNumber) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    for (int i = 0; i < fileCount; i++) {
        int revision = ds->GetHeadRevisionNumber(i);
        EXPECT_GE(revision, 0) << "Revision number should be non-negative";
    }

    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, GetMetadataLengths) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    for (int i = 0; i < fileCount; i++) {
        int chunkMetadataLength = ds->GetChunkMetadataLength(i);
        int fileMetadataLength = ds->GetFileMetadataLength(i);
        EXPECT_GE(chunkMetadataLength, 0) << "Chunk metadata length should be non-negative";
        EXPECT_GE(fileMetadataLength, 0) << "File metadata length should be non-negative";
    }

    OpenDataStore::Close(ds);
}

// ============================================================================
// Basic File Open/Close Tests
// ============================================================================

TEST_F(ODSCoreTest, OpenFile) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    const char* fileName = ds->GetFileName(0);
    OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
    ASSERT_NE(file, nullptr) << "Failed to open file: " << fileName;

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, OpenInvalidFileName) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->OpenFile("nonexistent_file");
    EXPECT_EQ(file, nullptr) << "Should return nullptr for invalid file name";

    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, OpenAllFiles) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();

    for (int i = 0; i < fileCount; i++) {
        const char* fileName = ds->GetFileName(i);
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
        ASSERT_NE(file, nullptr) << "Failed to open file at index " << i;
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);
}

// ============================================================================
// FileInterface Property Tests
// ============================================================================

TEST_F(ODSCoreTest, FileInterfaceGetFileName) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    const char* originalName = ds->GetFileName(0);
    OpenDataStore::FileInterface *file = ds->OpenFile(originalName);
    ASSERT_NE(file, nullptr);

    const char* retrievedName = file->GetFileName();
    EXPECT_STREQ(originalName, retrievedName) << "File name should match";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, FileInterfaceGetChunkCount) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    EXPECT_GE(chunkCount, 0) << "Chunk count should be non-negative";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, FileInterfaceGetRevision) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int revision = file->GetRevision();
    EXPECT_GE(revision, 0) << "Revision should be non-negative";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Chunk Reading Tests
// ============================================================================

TEST_F(ODSCoreTest, ReadChunkData) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    ASSERT_GT(chunkCount, 0) << "Need at least one chunk to test";

    OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
    ASSERT_NE(buffer, nullptr) << "Failed to read chunk 0";

    EXPECT_NE(buffer->Data(), nullptr) << "Buffer data should not be null";
    EXPECT_GT(buffer->Size(), 0) << "Buffer size should be positive";

    OpenDataStore::ReleaseBuffer(buffer);
    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, ReadMultipleChunks) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    int chunksToRead = std::min(chunkCount, 5);

    for (int i = 0; i < chunksToRead; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i);
        ASSERT_NE(buffer, nullptr) << "Failed to read chunk " << i;
        EXPECT_GT(buffer->Size(), 0) << "Chunk " << i << " should have positive size";
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, BufferDataAndSize) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);
    ASSERT_GT(file->GetChunkCount(), 0);

    OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
    ASSERT_NE(buffer, nullptr);

    const void* data = buffer->Data();
    int size = buffer->Size();

    EXPECT_NE(data, nullptr);
    EXPECT_GT(size, 0);

    // Verify we can access the data (read first byte)
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    (void)bytes[0]; // Access first byte - should not crash

    OpenDataStore::ReleaseBuffer(buffer);
    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Buffer Release Tests
// ============================================================================

TEST_F(ODSCoreTest, ReleaseBufferAfterRead) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);
    ASSERT_GT(file->GetChunkCount(), 0);

    OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
    ASSERT_NE(buffer, nullptr);

    // Release should not crash
    OpenDataStore::ReleaseBuffer(buffer);

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, MultipleBufferAllocations) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    int chunksToRead = std::min(chunkCount, 3);

    // Allocate multiple buffers
    std::vector<OpenDataStore::Buffer*> buffers;
    for (int i = 0; i < chunksToRead; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i);
        ASSERT_NE(buffer, nullptr);
        buffers.push_back(buffer);
    }

    // Release all buffers
    for (auto* buffer : buffers) {
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Index Entry Tests
// ============================================================================

TEST_F(ODSCoreTest, ReadIndexEntry) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);
    ASSERT_GT(file->GetChunkCount(), 0);

    IndexEntry indexEntry;
    int metadataLen = file->GetChunkMetadataLength();
    std::vector<char> metadata(metadataLen > 0 ? metadataLen : 1);

    bool success = file->ReadIndexEntry(0, &indexEntry, metadata.data());
    EXPECT_TRUE(success) << "Failed to read index entry";
    EXPECT_GE(indexEntry.m_offset, 0) << "Index entry offset should be non-negative";
    EXPECT_GE(indexEntry.m_length, 0) << "Index entry length should be non-negative";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSCoreTest, GetIndexPageAndEntryIndices) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    if (chunkCount > 0) {
        int pageIndex = file->GetIndexPageIndexForChunk(0);
        int entryIndex = file->GetIndexEntryIndexForChunk(0);

        EXPECT_GE(pageIndex, 0) << "Page index should be non-negative";
        EXPECT_GE(entryIndex, 0) << "Entry index should be non-negative";
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Multiple VDS Files Tests
// ============================================================================

TEST_F(ODSCoreTest, OpenDifferentVDSFiles) {
    // Open small VDS
    OpenDataStore *ds1 = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds1, nullptr) << "Failed to open small VDS";
    int count1 = ds1->GetFileCount();

    // Open medium VDS
    OpenDataStore *ds2 = OpenDataStore::Open(kMediumVDS.c_str());
    ASSERT_NE(ds2, nullptr) << "Failed to open medium VDS";
    int count2 = ds2->GetFileCount();

    EXPECT_GT(count1, 0);
    EXPECT_GT(count2, 0);

    OpenDataStore::Close(ds1);
    OpenDataStore::Close(ds2);
}
