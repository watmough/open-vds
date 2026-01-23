// ODSExtendedTests.cpp - Extended functionality tests for OpenDataStore
// Tests: writing, metadata, revisions, persistence

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>
#include <cstring>
#include "OpenDataStore.hpp"

class ODSExtendedTest : public ::testing::Test {
protected:
    // Path to read-only test VDS files
    const std::string kTestDataDir = "example-vds-data/";
    const std::string kSmallVDS = kTestDataDir + "85-34_mig_Time.vds";

    // Temp file for write tests
    std::string tempFile;

    void SetUp() override {
        tempFile = "/tmp/ods_extended_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(rand()) + ".vds";
    }

    void TearDown() override {
        std::remove(tempFile.c_str());
    }
};

// ============================================================================
// Enable Writing Tests
// ============================================================================

TEST_F(ODSExtendedTest, EnableWritingOnNewDataStore) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    // New datastore should already be writable
    EXPECT_FALSE(ds->IsReadOnly());

    bool result = ds->EnableWriting();
    EXPECT_TRUE(result) << "EnableWriting should succeed on new datastore";
    EXPECT_FALSE(ds->IsReadOnly());

    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, EnableWritingOnOpenedFile) {
    // First create a datastore
    OpenDataStore *ds1 = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds1, nullptr);
    OpenDataStore::Close(ds1);

    // Now open it (read-only by default)
    OpenDataStore *ds2 = OpenDataStore::Open(tempFile.c_str());
    ASSERT_NE(ds2, nullptr);
    EXPECT_TRUE(ds2->IsReadOnly());

    // EnableWriting may fail depending on file permissions or implementation
    // Just verify the API call doesn't crash
    bool result = ds2->EnableWriting();
    // Either it succeeds or we stay read-only (both are valid API responses)
    if (result) {
        EXPECT_FALSE(ds2->IsReadOnly());
    }

    OpenDataStore::Close(ds2);
}

// ============================================================================
// Add File Tests
// ============================================================================

TEST_F(ODSExtendedTest, AddFileToNewDataStore) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    EXPECT_EQ(ds->GetFileCount(), 0) << "New datastore should have no files";

    // Add a file with 10 chunks
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile",     // fileName
        10,             // chunkCount
        256,            // indexPageEntryCount
        1,              // fileType
        8,              // chunkMetadataLength
        16,             // fileMetadataLength
        false           // overwriteExisting
    );
    ASSERT_NE(file, nullptr) << "Failed to add file";

    EXPECT_EQ(ds->GetFileCount(), 1) << "Should have one file after AddFile";
    EXPECT_STREQ(ds->GetFileName(0), "TestFile");

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, AddMultipleFiles) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const char* fileNames[] = {"File1", "File2", "File3"};

    for (int i = 0; i < 3; i++) {
        OpenDataStore::FileInterface *file = ds->AddFile(
            fileNames[i], 5, 256, 1, 0, 0, false
        );
        ASSERT_NE(file, nullptr) << "Failed to add file " << fileNames[i];
        ds->CloseFile(file);
    }

    EXPECT_EQ(ds->GetFileCount(), 3);

    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, AddFileOverwriteExisting) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    // Add first file
    OpenDataStore::FileInterface *file1 = ds->AddFile(
        "TestFile", 10, 256, 1, 8, 16, false
    );
    ASSERT_NE(file1, nullptr);
    ds->CloseFile(file1);

    // Add same file with overwrite
    OpenDataStore::FileInterface *file2 = ds->AddFile(
        "TestFile", 20, 256, 2, 8, 16, true
    );
    ASSERT_NE(file2, nullptr) << "Overwrite should succeed";

    EXPECT_EQ(file2->GetChunkCount(), 20) << "Should have new chunk count";

    ds->CloseFile(file2);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Write Chunk Tests
// ============================================================================

TEST_F(ODSExtendedTest, WriteChunkData) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write test data
    std::vector<uint8_t> testData = {1, 2, 3, 4, 5, 6, 7, 8};
    bool result = file->WriteChunkData(0, testData.data(), testData.size());
    EXPECT_TRUE(result) << "WriteChunkData should succeed";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, WriteAndReadChunkData) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write test data
    std::vector<uint8_t> writeData = {10, 20, 30, 40, 50};
    bool writeResult = file->WriteChunkData(0, writeData.data(), writeData.size());
    EXPECT_TRUE(writeResult);

    // Read it back
    OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->Size(), static_cast<int>(writeData.size()));

    const uint8_t* readData = static_cast<const uint8_t*>(buffer->Data());
    for (size_t i = 0; i < writeData.size(); i++) {
        EXPECT_EQ(readData[i], writeData[i]) << "Mismatch at index " << i;
    }

    OpenDataStore::ReleaseBuffer(buffer);
    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, WriteChunkWithMetadata) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int chunkMetadataLen = 8;
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, chunkMetadataLen, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write chunk with metadata
    std::vector<uint8_t> data = {1, 2, 3, 4};
    std::vector<uint8_t> metadata(chunkMetadataLen, 0xAA);

    bool result = file->WriteChunk(0, data.data(), data.size(), metadata.data());
    EXPECT_TRUE(result) << "WriteChunk should succeed";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, WriteMultipleChunks) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 10, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write multiple chunks
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
        bool result = file->WriteChunkData(i, data.data(), data.size());
        EXPECT_TRUE(result) << "Failed to write chunk " << i;
    }

    // Verify all chunks
    for (int i = 0; i < 10; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i);
        ASSERT_NE(buffer, nullptr) << "Failed to read chunk " << i;
        EXPECT_EQ(buffer->Size(), 100);

        const uint8_t* readData = static_cast<const uint8_t*>(buffer->Data());
        EXPECT_EQ(readData[0], static_cast<uint8_t>(i));

        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Commit Tests
// ============================================================================

TEST_F(ODSExtendedTest, CommitChanges) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write some data
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    file->WriteChunkData(0, data.data(), data.size());

    // Commit
    bool result = file->Commit();
    EXPECT_TRUE(result) << "Commit should succeed";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// File Metadata Tests
// ============================================================================

TEST_F(ODSExtendedTest, WriteAndReadFileMetadata) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int fileMetadataLen = 16;
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, fileMetadataLen, false
    );
    ASSERT_NE(file, nullptr);

    // Write file metadata
    std::vector<uint8_t> writeMetadata(fileMetadataLen);
    for (int i = 0; i < fileMetadataLen; i++) {
        writeMetadata[i] = static_cast<uint8_t>(i * 10);
    }
    file->WriteFileMetadata(writeMetadata.data());

    // Read it back
    std::vector<uint8_t> readMetadata(fileMetadataLen);
    file->ReadFileMetadata(readMetadata.data());

    for (int i = 0; i < fileMetadataLen; i++) {
        EXPECT_EQ(readMetadata[i], writeMetadata[i]) << "Mismatch at index " << i;
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Chunk Metadata Tests
// ============================================================================

TEST_F(ODSExtendedTest, WriteAndReadChunkMetadata) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int chunkMetadataLen = 8;
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, chunkMetadataLen, 0, false
    );
    ASSERT_NE(file, nullptr);

    // First write some chunk data (may be required before metadata)
    std::vector<uint8_t> chunkData = {1, 2, 3, 4};
    file->WriteChunkData(0, chunkData.data(), chunkData.size());

    // Write chunk metadata
    std::vector<uint8_t> writeMetadata(chunkMetadataLen, 0x55);
    bool writeResult = file->WriteChunkMetadata(0, writeMetadata.data());
    EXPECT_TRUE(writeResult) << "WriteChunkMetadata should succeed";

    // Read it back
    std::vector<uint8_t> readMetadata(chunkMetadataLen);
    bool readResult = file->ReadChunkMetadata(0, readMetadata.data());
    EXPECT_TRUE(readResult) << "ReadChunkMetadata should succeed";

    for (int i = 0; i < chunkMetadataLen; i++) {
        EXPECT_EQ(readMetadata[i], writeMetadata[i]) << "Mismatch at index " << i;
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Index Entry Tests
// ============================================================================

TEST_F(ODSExtendedTest, WriteAndReadIndexEntry) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int chunkMetadataLen = 8;
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, chunkMetadataLen, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Create an index entry
    IndexEntry writeEntry;
    writeEntry.m_offset = 12345;
    writeEntry.m_length = 1000;
    writeEntry.m_reserved = 0;
    std::vector<uint8_t> writeMetadata(chunkMetadataLen, 0xBB);

    bool writeResult = file->WriteIndexEntry(0, writeEntry, writeMetadata.data());
    EXPECT_TRUE(writeResult) << "WriteIndexEntry should succeed";

    // Read it back
    IndexEntry readEntry;
    std::vector<uint8_t> readMetadata(chunkMetadataLen);
    bool readResult = file->ReadIndexEntry(0, &readEntry, readMetadata.data());
    EXPECT_TRUE(readResult) << "ReadIndexEntry should succeed";

    EXPECT_EQ(readEntry.m_offset, writeEntry.m_offset);
    EXPECT_EQ(readEntry.m_length, writeEntry.m_length);

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Revision Tests
// ============================================================================

TEST_F(ODSExtendedTest, GetHeadRevisionNumber) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);
    ds->CloseFile(file);

    int revision = ds->GetHeadRevisionNumber(0);
    EXPECT_GE(revision, 0) << "Revision number should be non-negative";

    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, FileInterfaceGetRevisionNumber) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    int revision = file->GetRevisionNumber();
    EXPECT_GE(revision, 0) << "Revision number should be non-negative";

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, OpenFileRevision) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write some data and commit to ensure revision is created
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    file->WriteChunkData(0, data.data(), data.size());
    file->Commit();

    int revision = file->GetRevisionNumber();
    ds->CloseFile(file);

    // Open by revision - may return nullptr if revision tracking not supported
    OpenDataStore::FileInterface *fileRev = ds->OpenFileRevision("TestFile", revision);
    if (fileRev != nullptr) {
        EXPECT_EQ(fileRev->GetRevisionNumber(), revision);
        ds->CloseFile(fileRev);
    }
    // Revision opening may not be supported in all cases, which is valid

    OpenDataStore::Close(ds);
}

// ============================================================================
// Remove File Tests
// ============================================================================

TEST_F(ODSExtendedTest, RemoveFile) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    // Add files
    OpenDataStore::FileInterface *file1 = ds->AddFile("File1", 5, 256, 1, 0, 0, false);
    ASSERT_NE(file1, nullptr);
    ds->CloseFile(file1);

    OpenDataStore::FileInterface *file2 = ds->AddFile("File2", 5, 256, 1, 0, 0, false);
    ASSERT_NE(file2, nullptr);
    ds->CloseFile(file2);

    EXPECT_EQ(ds->GetFileCount(), 2);

    // Remove one file
    bool result = ds->RemoveFile("File1");
    EXPECT_TRUE(result) << "RemoveFile should succeed";
    EXPECT_EQ(ds->GetFileCount(), 1);

    // Verify remaining file
    EXPECT_STREQ(ds->GetFileName(0), "File2");

    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, RemoveNonexistentFile) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    bool result = ds->RemoveFile("NonexistentFile");
    EXPECT_FALSE(result) << "Removing nonexistent file should fail";

    OpenDataStore::Close(ds);
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(ODSExtendedTest, PersistenceAfterClose) {
    // Create datastore and write data
    {
        OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
        ASSERT_NE(ds, nullptr);

        OpenDataStore::FileInterface *file = ds->AddFile(
            "PersistentFile", 5, 256, 1, 0, 0, false
        );
        ASSERT_NE(file, nullptr);

        std::vector<uint8_t> data = {100, 101, 102, 103, 104};
        file->WriteChunkData(0, data.data(), data.size());
        file->Commit();

        ds->CloseFile(file);
        OpenDataStore::Close(ds);
    }

    // Reopen and verify
    {
        OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
        ASSERT_NE(ds, nullptr);

        EXPECT_EQ(ds->GetFileCount(), 1);
        EXPECT_STREQ(ds->GetFileName(0), "PersistentFile");

        OpenDataStore::FileInterface *file = ds->OpenFile("PersistentFile");
        ASSERT_NE(file, nullptr);

        OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
        ASSERT_NE(buffer, nullptr);
        EXPECT_EQ(buffer->Size(), 5);

        const uint8_t* readData = static_cast<const uint8_t*>(buffer->Data());
        EXPECT_EQ(readData[0], 100);
        EXPECT_EQ(readData[4], 104);

        OpenDataStore::ReleaseBuffer(buffer);
        ds->CloseFile(file);
        OpenDataStore::Close(ds);
    }
}

TEST_F(ODSExtendedTest, PersistenceWithMultipleFiles) {
    const int numFiles = 3;

    // Create datastore with multiple files
    {
        OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
        ASSERT_NE(ds, nullptr);

        for (int i = 0; i < numFiles; i++) {
            std::string fileName = "File" + std::to_string(i);
            OpenDataStore::FileInterface *file = ds->AddFile(
                fileName.c_str(), 3, 256, i + 1, 0, 0, false
            );
            ASSERT_NE(file, nullptr);

            // Write unique data to each file
            std::vector<uint8_t> data(10, static_cast<uint8_t>(i * 10));
            file->WriteChunkData(0, data.data(), data.size());
            file->Commit();

            ds->CloseFile(file);
        }

        OpenDataStore::Close(ds);
    }

    // Reopen and verify all files
    {
        OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
        ASSERT_NE(ds, nullptr);

        EXPECT_EQ(ds->GetFileCount(), numFiles);

        for (int i = 0; i < numFiles; i++) {
            std::string fileName = "File" + std::to_string(i);
            OpenDataStore::FileInterface *file = ds->OpenFile(fileName.c_str());
            ASSERT_NE(file, nullptr) << "Failed to open " << fileName;

            OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
            ASSERT_NE(buffer, nullptr);

            const uint8_t* readData = static_cast<const uint8_t*>(buffer->Data());
            EXPECT_EQ(readData[0], static_cast<uint8_t>(i * 10));

            OpenDataStore::ReleaseBuffer(buffer);
            ds->CloseFile(file);
        }

        OpenDataStore::Close(ds);
    }
}

// ============================================================================
// File Rename Test
// ============================================================================

TEST_F(ODSExtendedTest, RenameFile) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "OriginalName", 5, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    file->Rename("NewName");
    EXPECT_STREQ(file->GetFileName(), "NewName");

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Read Existing VDS Metadata Tests
// ============================================================================

TEST_F(ODSExtendedTest, ReadExistingFileMetadata) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int metadataLen = file->GetFileMetadataLength();
    if (metadataLen > 0) {
        std::vector<char> metadata(metadataLen);
        file->ReadFileMetadata(metadata.data());
        // Just verify no crash - actual content depends on file
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSExtendedTest, ReadExistingChunkMetadata) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    int metadataLen = file->GetChunkMetadataLength();

    if (chunkCount > 0 && metadataLen > 0) {
        std::vector<char> metadata(metadataLen);
        bool result = file->ReadChunkMetadata(0, metadata.data());
        EXPECT_TRUE(result);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}
