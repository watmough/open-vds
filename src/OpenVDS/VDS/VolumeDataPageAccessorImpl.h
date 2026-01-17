/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
****************************************************************************/

#ifndef VOLUMEDATAPAGEACCESSORIMPL_H
#define VOLUMEDATAPAGEACCESSORIMPL_H

#include <OpenVDS/VolumeDataAccess.h>
#include <OpenVDS/OpenVDS.h>
#include <VDS/Logging.h>
#include "IntrusiveList.h"
#include "VolumeDataPageImpl.h"

#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>

namespace OpenVDS
{
class VolumeDataPageImpl;
class VolumeDataLayer;
class VolumeDataAccessManagerImpl;
typedef struct VDSError Error;
struct DataBlock;

class VolumeDataPageAccessorImpl : public VolumeDataPageAccessor
{
private:
  VolumeDataAccessManagerImpl *m_accessManager;
  VolumeDataPageAccessorImpl *m_parentVolumeDataPageAccessor;
  VolumeDataLayer const *m_layer;
  VolumeDataFormat m_format;
  float m_noValue;
  bool m_useNoValue;
  std::unique_ptr<VolumeDataPartition>
      m_superPartition;
  int m_pagesFound;
  int m_pagesRead;
  int m_pagesWritten;
  int m_maxPages;
  std::atomic_int m_references;
  AccessMode m_accessMode;
  bool m_isCommitInProgress;
  bool m_isLayerWriteLocked;
  std::atomic<std::chrono::time_point<std::chrono::steady_clock>> m_lastUsed;
  std::unordered_map<int64_t, VolumeDataPageImpl *> m_pages;
  IntrusiveList<VolumeDataPageImpl, &VolumeDataPageImpl::m_lru> m_pages_lru;
  std::condition_variable m_pageReadCondition;
  std::condition_variable m_commitFinishedCondition;

  public:
  mutable std::mutex m_pagesMutex;
  IntrusiveListNode<VolumeDataPageAccessorImpl> m_volumeDataPageAccessorListNode;
  Logger &m_logger;

private:
  void LimitPageListSize(int maxPages, std::unique_lock<std::mutex> &pageListMutexLock);
  void CommitInternal(std::unique_lock<std::mutex>& pageListMutexLock);
  void CreateSuperPartition();
  void EnsureSuperPartitionCreated() const { if(!m_superPartition) { static std::mutex mutex; std::unique_lock<std::mutex> mutexLock(mutex); if(!m_superPartition) { const_cast<VolumeDataPageAccessorImpl *>(this)->CreateSuperPartition(); } } }

public:
  VolumeDataPageAccessorImpl(VolumeDataAccessManagerImpl *accessManager, VolumeDataPageAccessorImpl *parentVolumeDataPageAccessor, VolumeDataLayer const* layer, VolumeDataFormat format, bool useNoValue, float noValue, int maxPages, AccessMode accessMode, Logger &logHandler);
  ~VolumeDataPageAccessorImpl();

  void Invalidate();

  bool IsReadWrite() const { return m_accessMode != AccessMode_ReadOnly; }

  VolumeDataLayout const* GetLayout() const override;
  VolumeDataLayer const * GetLayer() const { return m_layer; }
  VolumeDataFormat GetFormat() const { return m_format; }
  bool UseNoValue() const { return m_useNoValue; }
  float GetNoValue() const { return m_noValue; }

  int   GetLOD() const override;
  int   GetChannelIndex() const override;
  VolumeDataChannelDescriptor GetChannelDescriptor() const override;
  void  GetNumSamples(int(&numSamples)[Dimensionality_Max]) const override;

  int64_t GetChunkCount() const override;
  void    GetChunkMinMax(int64_t chunk, int(&min)[Dimensionality_Max], int(&max)[Dimensionality_Max]) const override;
  void    GetChunkMinMaxExcludingMargin(int64_t iChunk, int(&minExcludingMargin)[Dimensionality_Max], int(&maxExcludingMargin)[Dimensionality_Max]) const override;
  uint64_t GetChunkVolumeDataHash(int64_t chunkIndex) const override;
  int64_t GetChunkIndex(const int(&position)[Dimensionality_Max]) const override;
  int64_t GetMappedChunkIndex(int64_t primaryChannelChunkIndex) const override;
  int64_t GetPrimaryChannelChunkIndex(int64_t chunkIndex) const override;

  int64_t GetSuperChunkCount() const override;
  int     GetChunkCountInSuperChunk(int64_t superChunk) const override;
  void    GetChunkIndicesInSuperChunk(int64_t *chunkIndices, int64_t superChunk) const override;

  int   AddReference() override;
  int   RemoveReference() override;
  int   GetReferenceCount() const { return m_references.load(); }

  VolumeDataPageImpl* PrepareReadPage(int64_t chunk, Error &error);
  bool ReadPreparedPage(VolumeDataPageImpl *page);
  void CancelPreparedReadPage(VolumeDataPageImpl *page);
  bool GetError(VolumeDataPageImpl *page, OpenVDS::Error &error);

  // Two-phase read for parallel decompression support:
  // Phase 1: FetchPageData - I/O only, fetches compressed data from storage (single-threaded safe)
  // Phase 2: DecompressPageData - CPU only, decompresses data (can run in parallel)
  struct FetchedData
  {
    bool success = false;
    bool sparse = false;
    std::vector<uint8_t> serializedData;
    std::vector<uint8_t> metadata;
    CompressionMethod compressionMethod = CompressionMethod::None;
    int adaptiveLevel = 0;
    Error error;
  };
  FetchedData FetchPageData(VolumeDataPageImpl *page);
  bool DecompressPageData(VolumeDataPageImpl *page, FetchedData &fetchedData);

  int   GetMaxPages() override;
  void  SetMaxPages(int maxPages) override;

  VolumeDataPage *CreatePage(int64_t chunk) override;
  void  CopyPage(int64_t chunkIndex, VolumeDataPageAccessor const &source) override;
  VolumeDataPage *ReadPage(int64_t chunk) override;
  VolumeDataPage *ReadPageAtPosition(const int (&position)[Dimensionality_Max]) override;
 
  int64_t RequestWritePage(int64_t chunk, const DataBlock &dataBlock, const std::vector<uint8_t> &data, uint64_t hash);
  void AcquireLayerWriteLock();

  void  Commit() override;

  VolumeDataAccessManagerImpl *GetManager() const { return m_accessManager; }

  void SetLastUsed(std::chrono::time_point<std::chrono::steady_clock> lastUsed) { m_lastUsed = lastUsed; }
  std::chrono::time_point<std::chrono::steady_clock> GetLastUsed() const { return m_lastUsed.load(); }

};

}
#endif //VOLUMEDATAPAGEACCESSORIMPL_H
