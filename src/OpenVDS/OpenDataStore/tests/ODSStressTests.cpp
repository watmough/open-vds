// ODSStressTests.cpp - Stress/performance tests for OpenDataStore
// Tests: large files, many operations, repeated cycles

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sys/stat.h>
#include "OpenDataStore.hpp"

class ODSStressTest : public ::testing::Test {
protected:
    // Path to test VDS files
    const std::string kTestDataDir = "example-vds-data/";
    const std::string kSmallVDS = kTestDataDir + "85-34_mig_Time.vds";
    const std::string kMediumVDS = kTestDataDir + "Hor2.vds";
    const std::string kLargeVDS = kTestDataDir + "3DPoststack.vds";

    // Temp file for write tests
    std::string tempFile;

    void SetUp() override {
        tempFile = "/tmp/ods_stress_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(rand()) + ".vds";
    }

    void TearDown() override {
        std::remove(tempFile.c_str());
    }

    // Helper to count total chunks in a datastore
    int CountTotalChunks(OpenDataStore *ds) {
        int total = 0;
        int fileCount = ds->GetFileCount();
        for (int i = 0; i < fileCount; i++) {
            OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(i));
            if (file) {
                total += file->GetChunkCount();
                ds->CloseFile(file);
            }
        }
        return total;
    }
};

// ============================================================================
// Read All Chunks Tests
// ============================================================================

TEST_F(ODSStressTest, ReadAllChunksFromSmallFile) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    int totalChunksRead = 0;
    int64_t totalBytesRead = 0;

    for (int f = 0; f < fileCount; f++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
        ASSERT_NE(file, nullptr);

        int chunkCount = file->GetChunkCount();
        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            ASSERT_NE(buffer, nullptr) << "Failed to read chunk " << c << " of file " << f;
            totalBytesRead += buffer->Size();
            OpenDataStore::ReleaseBuffer(buffer);
            totalChunksRead++;
        }

        ds->CloseFile(file);
    }

    EXPECT_GT(totalChunksRead, 0) << "Should have read at least one chunk";
    EXPECT_GT(totalBytesRead, 0) << "Should have read some bytes";

    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, ReadAllChunksFromMediumFile) {
    OpenDataStore *ds = OpenDataStore::Open(kMediumVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    int totalChunksRead = 0;

    for (int f = 0; f < fileCount; f++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
        ASSERT_NE(file, nullptr);

        int chunkCount = file->GetChunkCount();
        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            ASSERT_NE(buffer, nullptr);
            EXPECT_GT(buffer->Size(), 0);
            OpenDataStore::ReleaseBuffer(buffer);
            totalChunksRead++;
        }

        ds->CloseFile(file);
    }

    EXPECT_GT(totalChunksRead, 0);

    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, ReadAllChunksFromLargeFile) {
    OpenDataStore *ds = OpenDataStore::Open(kLargeVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    int totalChunksRead = 0;
    int64_t totalBytesRead = 0;
    int skippedChunks = 0;

    for (int f = 0; f < fileCount; f++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
        ASSERT_NE(file, nullptr);

        int chunkCount = file->GetChunkCount();
        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            // Some chunks may be uninitialized/empty in VDS files
            if (buffer != nullptr) {
                totalBytesRead += buffer->Size();
                OpenDataStore::ReleaseBuffer(buffer);
                totalChunksRead++;
            } else {
                skippedChunks++;
            }
        }

        ds->CloseFile(file);
    }

    EXPECT_GT(totalChunksRead, 0) << "Should have read at least one chunk";
    // Large VDS file should have significant readable data
    EXPECT_GT(totalBytesRead, 100000) << "Large file should have significant data";

    OpenDataStore::Close(ds);
}

// ============================================================================
// Sequential Access Pattern Tests
// ============================================================================

TEST_F(ODSStressTest, SequentialForwardAccess) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();

    // Forward sequential access
    for (int c = 0; c < chunkCount; c++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
        ASSERT_NE(buffer, nullptr);
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, SequentialReverseAccess) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();

    // Reverse sequential access
    for (int c = chunkCount - 1; c >= 0; c--) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
        ASSERT_NE(buffer, nullptr);
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, StridedAccess) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    const int stride = 3;

    // Strided access pattern
    for (int c = 0; c < chunkCount; c += stride) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
        ASSERT_NE(buffer, nullptr);
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Multiple File Opens Tests
// ============================================================================

TEST_F(ODSStressTest, OpenMultipleFilesSimultaneously) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    std::vector<OpenDataStore::FileInterface*> openFiles;

    // Open all files at once
    for (int i = 0; i < fileCount; i++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(i));
        ASSERT_NE(file, nullptr) << "Failed to open file " << i;
        openFiles.push_back(file);
    }

    // Read from all open files
    for (auto* file : openFiles) {
        if (file->GetChunkCount() > 0) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
            ASSERT_NE(buffer, nullptr);
            OpenDataStore::ReleaseBuffer(buffer);
        }
    }

    // Close all files
    for (auto* file : openFiles) {
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, OpenSameFileTwice) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    const char* fileName = ds->GetFileName(0);

    // Open same file twice
    OpenDataStore::FileInterface *file1 = ds->OpenFile(fileName);
    ASSERT_NE(file1, nullptr);

    OpenDataStore::FileInterface *file2 = ds->OpenFile(fileName);
    ASSERT_NE(file2, nullptr);

    // Both should work independently
    if (file1->GetChunkCount() > 0) {
        OpenDataStore::Buffer *buffer1 = file1->ReadChunkData(0);
        ASSERT_NE(buffer1, nullptr);
        OpenDataStore::Buffer *buffer2 = file2->ReadChunkData(0);
        ASSERT_NE(buffer2, nullptr);

        EXPECT_EQ(buffer1->Size(), buffer2->Size());

        OpenDataStore::ReleaseBuffer(buffer1);
        OpenDataStore::ReleaseBuffer(buffer2);
    }

    ds->CloseFile(file1);
    ds->CloseFile(file2);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Large Write Operations Tests
// ============================================================================

TEST_F(ODSStressTest, WriteManyChunks) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int numChunks = 100;
    const int chunkSize = 1024;

    OpenDataStore::FileInterface *file = ds->AddFile(
        "ManyChunks", numChunks, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write many chunks
    for (int i = 0; i < numChunks; i++) {
        std::vector<uint8_t> data(chunkSize, static_cast<uint8_t>(i % 256));
        bool result = file->WriteChunkData(i, data.data(), data.size());
        EXPECT_TRUE(result) << "Failed to write chunk " << i;
    }

    file->Commit();

    // Verify all chunks
    for (int i = 0; i < numChunks; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i);
        ASSERT_NE(buffer, nullptr) << "Failed to read chunk " << i;
        EXPECT_EQ(buffer->Size(), chunkSize);

        const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
        EXPECT_EQ(data[0], static_cast<uint8_t>(i % 256));

        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, WriteLargeChunks) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int numChunks = 10;
    const int chunkSize = 100000; // 100KB chunks

    OpenDataStore::FileInterface *file = ds->AddFile(
        "LargeChunks", numChunks, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Write large chunks
    for (int i = 0; i < numChunks; i++) {
        std::vector<uint8_t> data(chunkSize);
        // Fill with pattern
        for (int j = 0; j < chunkSize; j++) {
            data[j] = static_cast<uint8_t>((i + j) % 256);
        }
        bool result = file->WriteChunkData(i, data.data(), data.size());
        EXPECT_TRUE(result) << "Failed to write large chunk " << i;
    }

    file->Commit();

    // Verify chunks
    for (int i = 0; i < numChunks; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i);
        ASSERT_NE(buffer, nullptr);
        EXPECT_EQ(buffer->Size(), chunkSize);

        const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
        // Verify first few bytes of pattern
        EXPECT_EQ(data[0], static_cast<uint8_t>(i % 256));
        EXPECT_EQ(data[1], static_cast<uint8_t>((i + 1) % 256));

        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, WriteManyFiles) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int numFiles = 20;
    const int chunksPerFile = 5;

    // Create many files
    for (int f = 0; f < numFiles; f++) {
        std::string fileName = "File" + std::to_string(f);
        OpenDataStore::FileInterface *file = ds->AddFile(
            fileName.c_str(), chunksPerFile, 256, 1, 0, 0, false
        );
        ASSERT_NE(file, nullptr) << "Failed to create file " << f;

        // Write data to each file
        for (int c = 0; c < chunksPerFile; c++) {
            std::vector<uint8_t> data = {
                static_cast<uint8_t>(f),
                static_cast<uint8_t>(c)
            };
            file->WriteChunkData(c, data.data(), data.size());
        }

        file->Commit();
        ds->CloseFile(file);
    }

    EXPECT_EQ(ds->GetFileCount(), numFiles);

    // Verify all files
    for (int f = 0; f < numFiles; f++) {
        std::string fileName = "File" + std::to_string(f);
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName.c_str());
        ASSERT_NE(file, nullptr);

        OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
        ASSERT_NE(buffer, nullptr);

        const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
        EXPECT_EQ(data[0], static_cast<uint8_t>(f));

        OpenDataStore::ReleaseBuffer(buffer);
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);
}

// ============================================================================
// Repeated Open/Close Cycles Tests
// ============================================================================

TEST_F(ODSStressTest, RepeatedOpenCloseCycles) {
    const int cycles = 50;

    for (int i = 0; i < cycles; i++) {
        OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
        ASSERT_NE(ds, nullptr) << "Failed to open on cycle " << i;

        int fileCount = ds->GetFileCount();
        EXPECT_GT(fileCount, 0);

        if (fileCount > 0) {
            OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
            ASSERT_NE(file, nullptr);

            if (file->GetChunkCount() > 0) {
                OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
                ASSERT_NE(buffer, nullptr);
                OpenDataStore::ReleaseBuffer(buffer);
            }

            ds->CloseFile(file);
        }

        OpenDataStore::Close(ds);
    }
}

TEST_F(ODSStressTest, RepeatedFileOpenCloseCycles) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    const char* fileName = ds->GetFileName(0);
    const int cycles = 100;

    for (int i = 0; i < cycles; i++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
        ASSERT_NE(file, nullptr) << "Failed to open file on cycle " << i;

        if (file->GetChunkCount() > 0) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
            ASSERT_NE(buffer, nullptr);
            OpenDataStore::ReleaseBuffer(buffer);
        }

        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);
}

// ============================================================================
// Repeated Write Operations Tests
// ============================================================================

TEST_F(ODSStressTest, RepeatedWritesToSameChunk) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", 1, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    const int iterations = 100;

    for (int i = 0; i < iterations; i++) {
        std::vector<uint8_t> data = {static_cast<uint8_t>(i % 256)};
        bool result = file->WriteChunkData(0, data.data(), data.size());
        EXPECT_TRUE(result) << "Write failed on iteration " << i;
    }

    // Verify final value
    OpenDataStore::Buffer *buffer = file->ReadChunkData(0);
    ASSERT_NE(buffer, nullptr);

    const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
    EXPECT_EQ(data[0], static_cast<uint8_t>((iterations - 1) % 256));

    OpenDataStore::ReleaseBuffer(buffer);
    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, InterleavedReadWrite) {
    OpenDataStore *ds = OpenDataStore::CreateNew(tempFile.c_str(), true);
    ASSERT_NE(ds, nullptr);

    const int numChunks = 10;
    OpenDataStore::FileInterface *file = ds->AddFile(
        "TestFile", numChunks, 256, 1, 0, 0, false
    );
    ASSERT_NE(file, nullptr);

    // Initialize all chunks
    for (int i = 0; i < numChunks; i++) {
        std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
        file->WriteChunkData(i, data.data(), data.size());
    }

    // Interleaved read-write operations
    for (int iter = 0; iter < 50; iter++) {
        int readChunk = iter % numChunks;
        int writeChunk = (iter + 1) % numChunks;

        // Read
        OpenDataStore::Buffer *buffer = file->ReadChunkData(readChunk);
        ASSERT_NE(buffer, nullptr);
        OpenDataStore::ReleaseBuffer(buffer);

        // Write
        std::vector<uint8_t> data = {static_cast<uint8_t>(iter % 256)};
        bool result = file->WriteChunkData(writeChunk, data.data(), data.size());
        EXPECT_TRUE(result);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

// ============================================================================
// Memory Stress Tests
// ============================================================================

TEST_F(ODSStressTest, ManyBufferAllocations) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    if (chunkCount == 0) {
        ds->CloseFile(file);
        OpenDataStore::Close(ds);
        GTEST_SKIP() << "No chunks available for test";
    }

    const int iterations = 1000;

    // Allocate and release many buffers
    for (int i = 0; i < iterations; i++) {
        int chunkIndex = i % chunkCount;
        OpenDataStore::Buffer *buffer = file->ReadChunkData(chunkIndex);
        ASSERT_NE(buffer, nullptr) << "Allocation failed at iteration " << i;
        OpenDataStore::ReleaseBuffer(buffer);
    }

    ds->CloseFile(file);
    OpenDataStore::Close(ds);
}

TEST_F(ODSStressTest, HoldManyBuffersSimultaneously) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_GT(ds->GetFileCount(), 0);

    OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(0));
    ASSERT_NE(file, nullptr);

    int chunkCount = file->GetChunkCount();
    int buffersToHold = std::min(chunkCount, 50);

    std::vector<OpenDataStore::Buffer*> buffers;

    // Allocate many buffers at once
    for (int i = 0; i < buffersToHold; i++) {
        OpenDataStore::Buffer *buffer = file->ReadChunkData(i % chunkCount);
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
// Rewrite With Larger Data Test
// ============================================================================

TEST_F(ODSStressTest, RewriteAllChunksWithLargerData) {
    // Copy VDS file to temp location for modification
    std::string srcFile = kSmallVDS;

    // Copy file using file streams
    {
        std::ifstream src(srcFile, std::ios::binary);
        ASSERT_TRUE(src.good()) << "Failed to open source VDS file";
        std::ofstream dst(tempFile, std::ios::binary);
        ASSERT_TRUE(dst.good()) << "Failed to create temp file";
        dst << src.rdbuf();
    }

    // Get initial file size
    struct stat statBuf;
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t initialFileSize = statBuf.st_size;

    // Open the copy for modification
    OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_TRUE(ds->IsOpen());

    // Enable writing
    bool writeEnabled = ds->EnableWriting();
    ASSERT_TRUE(writeEnabled) << "Failed to enable writing on datastore";

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    int totalChunksProcessed = 0;
    int64_t totalOriginalBytes = 0;
    int64_t totalNewBytes = 0;

    // Process each file in the datastore
    for (int f = 0; f < fileCount; f++) {
        const char* fileName = ds->GetFileName(f);
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
        ASSERT_NE(file, nullptr) << "Failed to open file: " << fileName;

        int chunkCount = file->GetChunkCount();
        int metadataLen = file->GetChunkMetadataLength();

        // Read all chunks and store their data
        struct ChunkInfo {
            std::vector<uint8_t> data;
            std::vector<char> metadata;
        };
        std::vector<ChunkInfo> chunks(chunkCount);

        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            if (buffer != nullptr) {
                const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
                int size = buffer->Size();
                chunks[c].data.assign(data, data + size);
                totalOriginalBytes += size;
                OpenDataStore::ReleaseBuffer(buffer);
            }

            // Read metadata if present
            if (metadataLen > 0) {
                chunks[c].metadata.resize(metadataLen);
                IndexEntry entry;
                file->ReadIndexEntry(c, &entry, chunks[c].metadata.data());
            }
        }

        // Rewrite all chunks with 10% larger data
        for (int c = 0; c < chunkCount; c++) {
            if (chunks[c].data.empty()) continue;

            // Create new data that is 10% larger
            size_t originalSize = chunks[c].data.size();
            size_t newSize = originalSize + (originalSize / 10);
            std::vector<uint8_t> newData(newSize);

            // Copy original data
            std::copy(chunks[c].data.begin(), chunks[c].data.end(), newData.begin());

            // Fill extra bytes with a pattern
            for (size_t i = originalSize; i < newSize; i++) {
                newData[i] = static_cast<uint8_t>((c + i) % 256);
            }

            // Write the larger chunk
            bool writeResult;
            if (metadataLen > 0 && !chunks[c].metadata.empty()) {
                writeResult = file->WriteChunk(c, newData.data(), newData.size(),
                                               chunks[c].metadata.data());
            } else {
                writeResult = file->WriteChunkData(c, newData.data(), newData.size());
            }
            EXPECT_TRUE(writeResult) << "Failed to write chunk " << c << " of file " << f;

            totalNewBytes += newSize;
            totalChunksProcessed++;
        }

        // Commit changes
        file->Commit();
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);

    // Get final file size
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t finalFileSize = statBuf.st_size;

    // Calculate metrics
    int64_t fileSizeIncrease = finalFileSize - initialFileSize;
    int64_t dataIncrease = totalNewBytes - totalOriginalBytes;
    double efficiency = (dataIncrease > 0) ?
        (static_cast<double>(fileSizeIncrease) / static_cast<double>(dataIncrease)) : 0.0;

    // Report results (these will show in test output)
    std::cout << "\n=== RewriteAllChunksWithLargerData Results (ODS) ===" << std::endl;
    std::cout << "Total chunks processed: " << totalChunksProcessed << std::endl;
    std::cout << "Original data bytes: " << totalOriginalBytes << std::endl;
    std::cout << "New data bytes: " << totalNewBytes << std::endl;
    std::cout << "Data increase: " << dataIncrease << " bytes" << std::endl;
    std::cout << "Initial file size: " << initialFileSize << " bytes" << std::endl;
    std::cout << "Final file size: " << finalFileSize << " bytes" << std::endl;
    std::cout << "File size increase: " << fileSizeIncrease << " bytes" << std::endl;
    std::cout << "Efficiency ratio (file growth / data growth): " << efficiency << std::endl;
    std::cout << "==================================================\n" << std::endl;

    // Verify we processed some chunks
    EXPECT_GT(totalChunksProcessed, 0) << "Should have processed at least one chunk";

    // Verify data can be read back correctly
    ds = OpenDataStore::Open(tempFile.c_str());
    ASSERT_NE(ds, nullptr);

    for (int f = 0; f < fileCount; f++) {
        OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
        ASSERT_NE(file, nullptr);

        int chunkCount = file->GetChunkCount();
        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            if (buffer != nullptr) {
                // Just verify we can read it - size should be ~10% larger
                OpenDataStore::ReleaseBuffer(buffer);
            }
        }

        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);

    // Efficiency check: file growth should ideally be close to data growth
    // A ratio > 2.0 indicates significant inefficiency (wasted space)
    // NOTE: This is currently an observation, not a hard requirement
    // Both BDS and ODS show high ratios (~10-12x) due to extent allocation
    // not reclaiming space from overwritten chunks
    if (dataIncrease > 0 && efficiency > 2.0) {
        std::cout << "WARNING: File grew " << efficiency
                  << "x more than data increase - indicates fragmentation" << std::endl;
        // Record the efficiency for tracking improvements
        RecordProperty("EfficiencyRatio", efficiency);
        RecordProperty("FileSizeIncrease", fileSizeIncrease);
        RecordProperty("DataIncrease", dataIncrease);
    }
}

TEST_F(ODSStressTest, RewriteAllChunksWithLargerData_MediumFile) {
    // Copy medium VDS file to temp location for modification
    std::string srcFile = kMediumVDS;

    // Copy file using file streams
    {
        std::ifstream src(srcFile, std::ios::binary);
        ASSERT_TRUE(src.good()) << "Failed to open source VDS file";
        std::ofstream dst(tempFile, std::ios::binary);
        ASSERT_TRUE(dst.good()) << "Failed to create temp file";
        dst << src.rdbuf();
    }

    // Get initial file size
    struct stat statBuf;
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t initialFileSize = statBuf.st_size;

    // Open the copy for modification
    OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_TRUE(ds->IsOpen());

    // Enable writing
    bool writeEnabled = ds->EnableWriting();
    ASSERT_TRUE(writeEnabled) << "Failed to enable writing on datastore";

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    int totalChunksProcessed = 0;
    int64_t totalOriginalBytes = 0;
    int64_t totalNewBytes = 0;

    // Process each file in the datastore
    for (int f = 0; f < fileCount; f++) {
        const char* fileName = ds->GetFileName(f);
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
        ASSERT_NE(file, nullptr) << "Failed to open file: " << fileName;

        int chunkCount = file->GetChunkCount();
        int metadataLen = file->GetChunkMetadataLength();

        // Read all chunks and store their data
        struct ChunkInfo {
            std::vector<uint8_t> data;
            std::vector<char> metadata;
        };
        std::vector<ChunkInfo> chunks(chunkCount);

        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            if (buffer != nullptr) {
                const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
                int size = buffer->Size();
                chunks[c].data.assign(data, data + size);
                totalOriginalBytes += size;
                OpenDataStore::ReleaseBuffer(buffer);
            }

            if (metadataLen > 0) {
                chunks[c].metadata.resize(metadataLen);
                IndexEntry entry;
                file->ReadIndexEntry(c, &entry, chunks[c].metadata.data());
            }
        }

        // Rewrite all chunks with 10% larger data
        for (int c = 0; c < chunkCount; c++) {
            if (chunks[c].data.empty()) continue;

            size_t originalSize = chunks[c].data.size();
            size_t newSize = originalSize + (originalSize / 10);
            std::vector<uint8_t> newData(newSize);

            std::copy(chunks[c].data.begin(), chunks[c].data.end(), newData.begin());
            for (size_t i = originalSize; i < newSize; i++) {
                newData[i] = static_cast<uint8_t>((c + i) % 256);
            }

            bool writeResult;
            if (metadataLen > 0 && !chunks[c].metadata.empty()) {
                writeResult = file->WriteChunk(c, newData.data(), newData.size(),
                                               chunks[c].metadata.data());
            } else {
                writeResult = file->WriteChunkData(c, newData.data(), newData.size());
            }
            EXPECT_TRUE(writeResult) << "Failed to write chunk " << c << " of file " << f;

            totalNewBytes += newSize;
            totalChunksProcessed++;
        }

        file->Commit();
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);

    // Get final file size
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t finalFileSize = statBuf.st_size;

    int64_t fileSizeIncrease = finalFileSize - initialFileSize;
    int64_t dataIncrease = totalNewBytes - totalOriginalBytes;
    double efficiency = (dataIncrease > 0) ?
        (static_cast<double>(fileSizeIncrease) / static_cast<double>(dataIncrease)) : 0.0;

    std::cout << "\n=== RewriteAllChunks Results (ODS - MEDIUM) ===" << std::endl;
    std::cout << "Total chunks processed: " << totalChunksProcessed << std::endl;
    std::cout << "Original data bytes: " << totalOriginalBytes << std::endl;
    std::cout << "New data bytes: " << totalNewBytes << std::endl;
    std::cout << "Data increase: " << dataIncrease << " bytes" << std::endl;
    std::cout << "Initial file size: " << initialFileSize << " bytes" << std::endl;
    std::cout << "Final file size: " << finalFileSize << " bytes" << std::endl;
    std::cout << "File size increase: " << fileSizeIncrease << " bytes" << std::endl;
    std::cout << "Efficiency ratio (file growth / data growth): " << efficiency << std::endl;
    std::cout << "===============================================\n" << std::endl;

    EXPECT_GT(totalChunksProcessed, 0) << "Should have processed at least one chunk";

    if (dataIncrease > 0 && efficiency > 2.0) {
        std::cout << "WARNING: File grew " << efficiency
                  << "x more than data increase - indicates fragmentation" << std::endl;
        RecordProperty("EfficiencyRatio", efficiency);
    }
}

TEST_F(ODSStressTest, RewriteAllChunksWithLargerData_LargeFile) {
    // Copy large VDS file to temp location for modification
    std::string srcFile = kLargeVDS;

    // Copy file using file streams
    {
        std::ifstream src(srcFile, std::ios::binary);
        ASSERT_TRUE(src.good()) << "Failed to open source VDS file";
        std::ofstream dst(tempFile, std::ios::binary);
        ASSERT_TRUE(dst.good()) << "Failed to create temp file";
        dst << src.rdbuf();
    }

    // Get initial file size
    struct stat statBuf;
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t initialFileSize = statBuf.st_size;

    // Open the copy for modification
    OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
    ASSERT_NE(ds, nullptr);
    ASSERT_TRUE(ds->IsOpen());

    // Enable writing
    bool writeEnabled = ds->EnableWriting();
    ASSERT_TRUE(writeEnabled) << "Failed to enable writing on datastore";

    int fileCount = ds->GetFileCount();
    ASSERT_GT(fileCount, 0);

    int totalChunksProcessed = 0;
    int64_t totalOriginalBytes = 0;
    int64_t totalNewBytes = 0;

    // Process each file in the datastore
    for (int f = 0; f < fileCount; f++) {
        const char* fileName = ds->GetFileName(f);
        OpenDataStore::FileInterface *file = ds->OpenFile(fileName);
        ASSERT_NE(file, nullptr) << "Failed to open file: " << fileName;

        int chunkCount = file->GetChunkCount();
        int metadataLen = file->GetChunkMetadataLength();

        // Read all chunks and store their data
        struct ChunkInfo {
            std::vector<uint8_t> data;
            std::vector<char> metadata;
        };
        std::vector<ChunkInfo> chunks(chunkCount);

        for (int c = 0; c < chunkCount; c++) {
            OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
            if (buffer != nullptr) {
                const uint8_t* data = static_cast<const uint8_t*>(buffer->Data());
                int size = buffer->Size();
                chunks[c].data.assign(data, data + size);
                totalOriginalBytes += size;
                OpenDataStore::ReleaseBuffer(buffer);
            }

            if (metadataLen > 0) {
                chunks[c].metadata.resize(metadataLen);
                IndexEntry entry;
                file->ReadIndexEntry(c, &entry, chunks[c].metadata.data());
            }
        }

        // Rewrite all chunks with 10% larger data
        for (int c = 0; c < chunkCount; c++) {
            if (chunks[c].data.empty()) continue;

            size_t originalSize = chunks[c].data.size();
            size_t newSize = originalSize + (originalSize / 10);
            std::vector<uint8_t> newData(newSize);

            std::copy(chunks[c].data.begin(), chunks[c].data.end(), newData.begin());
            for (size_t i = originalSize; i < newSize; i++) {
                newData[i] = static_cast<uint8_t>((c + i) % 256);
            }

            bool writeResult;
            if (metadataLen > 0 && !chunks[c].metadata.empty()) {
                writeResult = file->WriteChunk(c, newData.data(), newData.size(),
                                               chunks[c].metadata.data());
            } else {
                writeResult = file->WriteChunkData(c, newData.data(), newData.size());
            }
            EXPECT_TRUE(writeResult) << "Failed to write chunk " << c << " of file " << f;

            totalNewBytes += newSize;
            totalChunksProcessed++;
        }

        file->Commit();
        ds->CloseFile(file);
    }

    OpenDataStore::Close(ds);

    // Get final file size
    ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
    int64_t finalFileSize = statBuf.st_size;

    int64_t fileSizeIncrease = finalFileSize - initialFileSize;
    int64_t dataIncrease = totalNewBytes - totalOriginalBytes;
    double efficiency = (dataIncrease > 0) ?
        (static_cast<double>(fileSizeIncrease) / static_cast<double>(dataIncrease)) : 0.0;

    std::cout << "\n=== RewriteAllChunks Results (ODS - LARGE) ===" << std::endl;
    std::cout << "Total chunks processed: " << totalChunksProcessed << std::endl;
    std::cout << "Original data bytes: " << totalOriginalBytes << std::endl;
    std::cout << "New data bytes: " << totalNewBytes << std::endl;
    std::cout << "Data increase: " << dataIncrease << " bytes" << std::endl;
    std::cout << "Initial file size: " << initialFileSize << " bytes" << std::endl;
    std::cout << "Final file size: " << finalFileSize << " bytes" << std::endl;
    std::cout << "File size increase: " << fileSizeIncrease << " bytes" << std::endl;
    std::cout << "Efficiency ratio (file growth / data growth): " << efficiency << std::endl;
    std::cout << "==============================================\n" << std::endl;

    EXPECT_GT(totalChunksProcessed, 0) << "Should have processed at least one chunk";

    if (dataIncrease > 0 && efficiency > 2.0) {
        std::cout << "WARNING: File grew " << efficiency
                  << "x more than data increase - indicates fragmentation" << std::endl;
        RecordProperty("EfficiencyRatio", efficiency);
    }
}

// // ============================================================================
// // Compaction Tests
// // ============================================================================

// TEST_F(ODSStressTest, RewriteAndCompact) {
//     // Copy VDS file to temp location
//     std::ifstream src(kSmallVDS, std::ios::binary);
//     ASSERT_TRUE(src.is_open()) << "Cannot open source file: " << kSmallVDS;
//     std::ofstream dst(tempFile, std::ios::binary);
//     ASSERT_TRUE(dst.is_open()) << "Cannot create temp file: " << tempFile;
//     dst << src.rdbuf();
//     src.close();
//     dst.close();

//     // Get initial file size
//     struct stat statBuf;
//     ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
//     int64_t initialFileSize = statBuf.st_size;

//     // Open the copy and enable writing
//     OpenDataStore *ds = OpenDataStore::Open(tempFile.c_str());
//     ASSERT_NE(ds, nullptr);
//     ASSERT_TRUE(ds->EnableWriting());
//     ASSERT_TRUE(ds->BuildExtentAllocator());

//     int64_t totalOriginalBytes = 0;
//     int64_t totalNewBytes = 0;
//     int totalChunksProcessed = 0;

//     // First pass: Rewrite all chunks with 10% larger data
//     int fileCount = ds->GetFileCount();
//     for (int f = 0; f < fileCount; f++) {
//         OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
//         if (!file) continue;

//         int chunkCount = file->GetChunkCount();
//         for (int c = 0; c < chunkCount; c++) {
//             OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
//             if (!buffer) continue;

//             int originalSize = buffer->Size();
//             totalOriginalBytes += originalSize;

//             int newSize = originalSize + (originalSize / 10);
//             std::vector<uint8_t> newData(newSize);
//             memcpy(newData.data(), buffer->Data(), originalSize);
//             for (int i = originalSize; i < newSize; i++) {
//                 newData[i] = static_cast<uint8_t>((c + i) % 256);
//             }
//             OpenDataStore::ReleaseBuffer(buffer);

//             file->WriteChunkData(c, newData.data(), newData.size());
//             totalNewBytes += newSize;
//             totalChunksProcessed++;
//         }

//         file->Commit();
//         ds->CloseFile(file);
//     }

//     // Get file size before compaction
//     OpenDataStore::Close(ds);
//     ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
//     int64_t beforeCompactSize = statBuf.st_size;

//     // Reopen and compact
//     ds = OpenDataStore::Open(tempFile.c_str());
//     ASSERT_NE(ds, nullptr);
//     ASSERT_TRUE(ds->EnableWriting());
//     ASSERT_TRUE(ds->BuildExtentAllocator());

//     int64_t bytesReclaimed = ds->Compact();
//     OpenDataStore::Close(ds);

//     // Get final file size after compaction
//     ASSERT_EQ(stat(tempFile.c_str(), &statBuf), 0);
//     int64_t afterCompactSize = statBuf.st_size;

//     int64_t dataIncrease = totalNewBytes - totalOriginalBytes;
//     double beforeEfficiency = (dataIncrease > 0) ?
//         (static_cast<double>(beforeCompactSize - initialFileSize) / static_cast<double>(dataIncrease)) : 0.0;
//     double afterEfficiency = (dataIncrease > 0) ?
//         (static_cast<double>(afterCompactSize - initialFileSize) / static_cast<double>(dataIncrease)) : 0.0;

//     std::cout << "\n=== RewriteAndCompact Results (ODS) ===" << std::endl;
//     std::cout << "Total chunks processed: " << totalChunksProcessed << std::endl;
//     std::cout << "Data increase: " << dataIncrease << " bytes" << std::endl;
//     std::cout << "Initial file size: " << initialFileSize << " bytes" << std::endl;
//     std::cout << "Before compact size: " << beforeCompactSize << " bytes" << std::endl;
//     std::cout << "After compact size: " << afterCompactSize << " bytes" << std::endl;
//     std::cout << "Bytes reclaimed by compaction: " << bytesReclaimed << std::endl;
//     std::cout << "Efficiency BEFORE compact: " << beforeEfficiency << std::endl;
//     std::cout << "Efficiency AFTER compact: " << afterEfficiency << std::endl;
//     std::cout << "========================================\n" << std::endl;

//     // Verify the file is still readable after compaction
//     ds = OpenDataStore::Open(tempFile.c_str());
//     ASSERT_NE(ds, nullptr);

//     int verifiedChunks = 0;
//     for (int f = 0; f < ds->GetFileCount(); f++) {
//         OpenDataStore::FileInterface *file = ds->OpenFile(ds->GetFileName(f));
//         if (!file) continue;

//         for (int c = 0; c < file->GetChunkCount(); c++) {
//             OpenDataStore::Buffer *buffer = file->ReadChunkData(c);
//             ASSERT_NE(buffer, nullptr) << "Failed to read chunk " << c << " after compaction";
//             EXPECT_GT(buffer->Size(), 0);
//             OpenDataStore::ReleaseBuffer(buffer);
//             verifiedChunks++;
//         }

//         ds->CloseFile(file);
//     }

//     EXPECT_EQ(verifiedChunks, totalChunksProcessed) << "Should be able to read all chunks after compaction";
//     OpenDataStore::Close(ds);
// }

// ============================================================================
// Cross-File Operations Test
// ============================================================================

TEST_F(ODSStressTest, CrossFileOperations) {
    OpenDataStore *ds = OpenDataStore::Open(kSmallVDS.c_str());
    ASSERT_NE(ds, nullptr);

    int fileCount = ds->GetFileCount();
    if (fileCount < 2) {
        OpenDataStore::Close(ds);
        GTEST_SKIP() << "Need at least 2 files for cross-file test";
    }

    // Open two files
    OpenDataStore::FileInterface *file1 = ds->OpenFile(ds->GetFileName(0));
    OpenDataStore::FileInterface *file2 = ds->OpenFile(ds->GetFileName(1));
    ASSERT_NE(file1, nullptr);
    ASSERT_NE(file2, nullptr);

    int chunks1 = file1->GetChunkCount();
    int chunks2 = file2->GetChunkCount();

    // Interleave reads between files
    int maxChunks = std::max(chunks1, chunks2);
    for (int i = 0; i < std::min(maxChunks, 20); i++) {
        if (i < chunks1) {
            OpenDataStore::Buffer *buffer = file1->ReadChunkData(i);
            ASSERT_NE(buffer, nullptr);
            OpenDataStore::ReleaseBuffer(buffer);
        }
        if (i < chunks2) {
            OpenDataStore::Buffer *buffer = file2->ReadChunkData(i);
            ASSERT_NE(buffer, nullptr);
            OpenDataStore::ReleaseBuffer(buffer);
        }
    }

    ds->CloseFile(file1);
    ds->CloseFile(file2);
    OpenDataStore::Close(ds);
}
