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

#include "VolumeDataPageAccessorImpl.h"
#include "VolumeDataLayoutImpl.h"
#include "VolumeDataChannelMapping.h"
#include "VolumeDataAccessManagerImpl.h"
#include "VolumeDataLayer.h"
#include "VolumeDataRegion.h"
#include "VolumeDataPageImpl.h"
#include "VolumeDataStore.h"
#include "MetadataManager.h"
#include "Logging.h"

#include <IO/IOManager.h>

#include "DimensionGroup.h"

#include <chrono>
#include <algorithm>

#include <fmt/printf.h>

namespace OpenVDS
{

VolumeDataPageAccessorImpl::VolumeDataPageAccessorImpl(VolumeDataAccessManagerImpl *accessManager, VolumeDataPageAccessorImpl *parentVolumeDataPageAccessor, VolumeDataLayer const* layer, VolumeDataFormat format, bool useNoValue, float noValue, int maxPages, AccessMode accessMode, Logger &logHandler)
  : m_accessManager(accessManager)
  , m_parentVolumeDataPageAccessor(parentVolumeDataPageAccessor)
  , m_layer(layer)
  , m_format(format)
  , m_noValue(noValue)
  , m_useNoValue(useNoValue)
  , m_pagesFound(0)
  , m_pagesRead(0)
  , m_pagesWritten(0)
  , m_maxPages(maxPages)
  , m_references(1)
  , m_accessMode(accessMode)
  , m_isCommitInProgress(false)
  , m_isLayerWriteLocked(false)
  , m_lastUsed(std::chrono::steady_clock::now())
  , m_logger(logHandler)
{
  if (accessMode != AccessMode::AccessMode_ReadOnly)
  {
    Error error;
    if (!m_accessManager->GetVolumeDataStore()->EnableWriting(error))
    {
      throw ReadErrorException(fmt::format("Failed to create PageAccessor in write mode: {}", error.string).c_str(), error.code);
    }
  }
}

VolumeDataPageAccessorImpl::~VolumeDataPageAccessorImpl()
{
  m_pages.clear();
  while(auto page = m_pages_lru.GetFirstItem())
  {
    m_pages_lru.Remove(page);
    delete page;
  }

  if(m_parentVolumeDataPageAccessor)
  {
    if(m_parentVolumeDataPageAccessor->RemoveReference() == 0)
    {
      delete m_parentVolumeDataPageAccessor;
    }
  }
}

void
VolumeDataPageAccessorImpl::CreateSuperPartition()
{
  assert(m_layer);
  VolumeDataLayer const *remapFromLayer = m_layer->GetLayerToRemapFrom();
  m_superPartition.reset(new VolumeDataPartition(VolumeDataPartition::StaticFindSuperPartition(*m_layer, *remapFromLayer)));
}

void
VolumeDataPageAccessorImpl::Invalidate()
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  m_maxPages = 0;
  m_layer = nullptr;
  LimitPageListSize(0, pageListMutexLock);
}

VolumeDataLayout const* VolumeDataPageAccessorImpl::GetLayout() const
{
  return m_layer->GetLayout();
}

int VolumeDataPageAccessorImpl::GetLOD() const
{
  return m_layer->GetLOD();
}

int VolumeDataPageAccessorImpl::GetChannelIndex() const
{
  return m_layer->GetChannelIndex();
}

VolumeDataChannelDescriptor VolumeDataPageAccessorImpl::GetChannelDescriptor() const
{
  return m_layer->GetVolumeDataChannelDescriptor();
}

void  VolumeDataPageAccessorImpl::GetNumSamples(int(&numSamples)[Dimensionality_Max]) const
{
  for (int i = 0; i < Dimensionality_Max; i++)
  {
    numSamples[i] = m_layer->GetDimensionNumSamples(i);
  }
}

int64_t VolumeDataPageAccessorImpl::GetChunkCount() const
{
  return m_layer->GetTotalChunkCount();
}

void  VolumeDataPageAccessorImpl::GetChunkMinMax(int64_t chunk, int(&min)[Dimensionality_Max], int(&max)[Dimensionality_Max]) const
{
  m_layer->GetChunkMinMax(chunk, min, max, true);
}

void  VolumeDataPageAccessorImpl::GetChunkMinMaxExcludingMargin(int64_t chunk, int(&min)[Dimensionality_Max], int(&max)[Dimensionality_Max]) const
{
  m_layer->GetChunkMinMax(chunk, min, max, false);
}

uint64_t VolumeDataPageAccessorImpl::GetChunkVolumeDataHash(int64_t chunkIndex) const
{
  if (chunkIndex < 0 || chunkIndex > m_layer->GetTotalChunkCount())
  {
    throw InvalidArgument("The requested chunk doesn't exist", "chunkIndex");
  }

  uint64_t
    chunkDataHash = VolumeDataHash::UNKNOWN;

  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  auto page_it = m_pages.find(chunkIndex);
  if(page_it != m_pages.end())
  {
    return (*page_it).second->GetVolumeDataHash();
  }
  pageListMutexLock.unlock();

  if(m_layer->GetProduceStatus() == VolumeDataLayer::ProduceStatus_Normal)
  {
    Error error;
    m_accessManager->GetVolumeDataStore()->ReadChunkDataHash(m_layer->GetChunkFromIndex(chunkIndex), chunkDataHash, error);
  }

  return chunkDataHash;
}

int64_t VolumeDataPageAccessorImpl::GetChunkIndex(const int(&position)[Dimensionality_Max]) const
{
  int32_t index_array[Dimensionality_Max];
  for (int i = 0; i < Dimensionality_Max; i++)
  {
    index_array[i] = m_layer->VoxelToIndex(position[i], i);
  }
  return m_layer->IndexArrayToChunkIndex(index_array);
}

int64_t VolumeDataPageAccessorImpl::GetMappedChunkIndex(int64_t primaryChannelChunkIndex) const
{
  auto volumeDataChannelMapping = m_layer->GetVolumeDataChannelMapping();
  if(volumeDataChannelMapping)
  {
    return volumeDataChannelMapping->GetMappedChunkIndex(m_layer->GetPrimaryChannelLayer(), primaryChannelChunkIndex);
  }
  else
  {
    return primaryChannelChunkIndex;
  }
}

int64_t VolumeDataPageAccessorImpl::GetPrimaryChannelChunkIndex(int64_t chunkIndex) const
{
  auto volumeDataChannelMapping = m_layer->GetVolumeDataChannelMapping();

  if(volumeDataChannelMapping)
  {
    return volumeDataChannelMapping->GetPrimaryChunkIndex(m_layer->GetPrimaryChannelLayer(), chunkIndex);
  }
  else
  {
    return chunkIndex;
  }
}

int64_t VolumeDataPageAccessorImpl::GetSuperChunkCount() const
{
  EnsureSuperPartitionCreated();
  return m_superPartition->GetTotalChunkCount();
}

int VolumeDataPageAccessorImpl::GetChunkCountInSuperChunk(int64_t superChunk) const
{
  EnsureSuperPartitionCreated();

  IndexArray chunkMin, chunkMax;
  m_layer->GetChunkIndexMinMaxInSuperChunk(*m_superPartition, superChunk, chunkMin, chunkMax);

  VolumeDataRegion superChunkRegion = VolumeDataRegion::VolumeDataRegionFromChunkMinMax(*m_layer, chunkMin, chunkMax);

  return int(superChunkRegion.GetNumChunksInRegion());
}

void VolumeDataPageAccessorImpl::GetChunkIndicesInSuperChunk(int64_t *chunkIndices, int64_t superChunk) const
{
  EnsureSuperPartitionCreated();

  IndexArray chunkMin, chunkMax;
  m_layer->GetChunkIndexMinMaxInSuperChunk(*m_superPartition, superChunk, chunkMin, chunkMax);

  VolumeDataRegion superChunkRegion = VolumeDataRegion::VolumeDataRegionFromChunkMinMax(*m_layer, chunkMin, chunkMax);

  int64_t numChunksInRegion = superChunkRegion.GetNumChunksInRegion();
  for(int64_t chunkInRegion = 0; chunkInRegion < numChunksInRegion; chunkInRegion++)
  {
    chunkIndices[chunkInRegion] = superChunkRegion.GetChunkIndexInRegion(chunkInRegion);
  }
}

int VolumeDataPageAccessorImpl::AddReference()
{
  return ++m_references;
}

int VolumeDataPageAccessorImpl::RemoveReference()
{
  return --m_references;
}

int VolumeDataPageAccessorImpl::GetMaxPages()
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);
  return m_maxPages;
}

void VolumeDataPageAccessorImpl::SetMaxPages(int maxPages)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);
  m_maxPages = maxPages;
  LimitPageListSize(m_maxPages, pageListMutexLock);
}

VolumeDataPage* VolumeDataPageAccessorImpl::CreatePage(int64_t chunk)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  if(!m_layer)
  {
    return nullptr;
  }

  // Wait for commit to finish before inserting a new page
  while(m_isCommitInProgress)
  {
    m_commitFinishedCondition.wait_for(pageListMutexLock, std::chrono::milliseconds(1000));
    if(!m_layer)
    {
      return nullptr;
    }
  }

  // This will throw if we can't acquire the write lock
  AcquireLayerWriteLock();

  auto page_it = m_pages.find(chunk);
  if(page_it != m_pages.end())
  {
    throw InvalidOperation("Cannot create a page that already exists");
  }

  VolumeDataPageImpl *parentPage = nullptr;
  if(m_parentVolumeDataPageAccessor)
  {
    int64_t parentChunk = m_layer->GetParentIndex(chunk, *m_parentVolumeDataPageAccessor->m_layer);
    parentPage = static_cast<VolumeDataPageImpl *>(m_parentVolumeDataPageAccessor->ReadPage(parentChunk));
  }

  // Create a new page
  VolumeDataPageImpl *page = new VolumeDataPageImpl(this, chunk, parentPage);

  m_pages[chunk] = page;
  m_pages_lru.InsertFirst(page);

  assert(page->IsPinned());

  if(parentPage && parentPage->GetErrorInternal().code != 0)
  {
    page->SetError(parentPage->GetErrorInternal());
    return page;
  }

  Error error;
  VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(chunk);

  pageListMutexLock.unlock();

  std::vector<uint8_t> page_data;
  DataBlock dataBlock;
  VolumeDataHash volumeDataHash = m_layer->IsUseNoValue() ? VolumeDataHash::NOVALUE : VolumeDataHash(0.0f);
  if (!VolumeDataStore::CreateConstantValueDataBlock(volumeDataChunk, m_layer->GetFormat(), m_layer->GetNoValue(), m_layer->GetComponents(), volumeDataHash, dataBlock, page_data, error))
  {
    pageListMutexLock.lock();
    page->UnPin();
    m_logger.LogError(fmt::format("Failed when creating chunk: {}", error.string.c_str()));
    return nullptr;
  }

  pageListMutexLock.lock();
  page->SetBufferData(dataBlock, m_layer->GetChunkDimensionGroup(), std::move(page_data), uint64_t(volumeDataHash));
  page->MakeDirty();

  m_pageReadCondition.notify_all();

  if(!m_layer)
  {
    page->UnPin();
    page = nullptr;
  }

  LimitPageListSize(m_maxPages, pageListMutexLock);

  return page;
}

VolumeDataPageImpl* VolumeDataPageAccessorImpl::PrepareReadPage(int64_t chunk, Error &error)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  if(!m_layer)
  {
    error.code = -1;
    error.string = "PrepareReadPage missing layer";
    return nullptr;
  }

  if (m_layer->GetProduceStatus() == VolumeDataLayer::ProduceStatus_Unavailable)
  {
    error.code = -1;
    error.string = "The accessed dimension group or channel is unavailable (check produce status on VDS before accessing data)";
    return nullptr;
  }

  auto page_it = m_pages.find(chunk);
  if (page_it != m_pages.end())
  {
    if (m_pages_lru.GetFirstItem() != page_it->second)
    {
      m_pages_lru.Remove(page_it->second);
      m_pages_lru.InsertFirst(page_it->second);
    }
    page_it->second->Pin();
    m_pagesFound++;
    return page_it->second;
  }

  // Wait for commit to finish before inserting a new page
  while(m_isCommitInProgress)
  {
    m_commitFinishedCondition.wait_for(pageListMutexLock, std::chrono::milliseconds(1000));
    if(!m_layer)
    {
      error.code = -1;
      error.string = "PrepareReadPage loosing layer while waiting for commit";
      return nullptr;
    }
  }

  VolumeDataPageImpl *parentPage = nullptr;
  if(m_parentVolumeDataPageAccessor)
  {
    int64_t parentChunk = m_layer->GetParentIndex(chunk, *m_parentVolumeDataPageAccessor->m_layer);
    parentPage = static_cast<VolumeDataPageImpl *>(m_parentVolumeDataPageAccessor->ReadPage(parentChunk));
  }

  // Not found, we need to create a new page
  VolumeDataPageImpl *page = new VolumeDataPageImpl(this, chunk, parentPage);

  m_pages[chunk] = page;
  m_pages_lru.InsertFirst(page);

  assert(page->IsPinned());

  if(parentPage && parentPage->GetErrorInternal().code != 0)
  {
    page->SetError(parentPage->GetErrorInternal());
    return page;
  }

  VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(chunk);

  VolumeDataLayer const *remapFromLayer = m_layer->GetLayerToRemapFrom();

  if(remapFromLayer != m_layer)
  {
    std::vector<VolumeDataChunk> volumeDataChunkRead;
    remapFromLayer->GetChunksOverlappingChunk(volumeDataChunk, &volumeDataChunkRead);
    page->SetJobID(m_accessManager->AddRemapJob(*page, volumeDataChunkRead));
  }
  else
  {
    if (!m_accessManager->GetVolumeDataStore()->PrepareReadChunk(volumeDataChunk, m_layer->GetEffectiveWaveletAdaptiveLoadLevel(), error))
    {
      page->SetError(error);
    }
  }

  return page;
}

bool VolumeDataPageAccessorImpl::ReadPreparedPage(VolumeDataPageImpl* pageImpl)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex, std::defer_lock);

  if (!pageImpl->RequestPrepared())
  {
    pageListMutexLock.lock();
    return pageImpl->GetErrorInternal().code == 0;
  }

  if (pageImpl->EnterSettingData())
  {
    if (!pageImpl->RequestPrepared())
    {
      pageImpl->LeaveSettingData();
      pageListMutexLock.lock();
      return pageImpl->GetErrorInternal().code == 0;
    }

    Error error;
    bool success = false;

    int64_t jobID = pageImpl->JobID();
    if(jobID != -1)
    {
      success = m_accessManager->WaitForCompletion(jobID);

      if(!success)
      {
        ReadErrorException readError("", 0);
        bool canceled = m_accessManager->IsCanceled(jobID, &readError);
        (void)canceled;
        assert(canceled);
        error.code = readError.GetErrorCode();
        error.string = readError.GetErrorMessage();

        pageListMutexLock.lock();
        pageImpl->SetError(error);
        pageImpl->SetRequestPrepared(false);
        pageImpl->LeaveSettingData();
        m_pageReadCondition.notify_all();
        m_logger.LogError(fmt::format("Failed when waiting for chunk: {}", error.string.c_str()));
        return false;
      }
      
      pageListMutexLock.lock();
    }
    else
    {
      VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(pageImpl->GetChunkIndex());
      std::vector<uint8_t> serialized_data;
      std::vector<uint8_t> metadata;
      CompressionInfo compressionInfo;

      if (!m_accessManager->GetVolumeDataStore()->ReadChunk(volumeDataChunk, m_layer->GetEffectiveWaveletAdaptiveLoadLevel(), serialized_data, metadata, compressionInfo, error))
      {
        pageListMutexLock.lock();
        pageImpl->SetError(error);
        pageImpl->SetRequestPrepared(false);
        pageImpl->LeaveSettingData();
        m_pageReadCondition.notify_all();
        m_logger.LogError(fmt::format("Failed when waiting for chunk: {}", error.string.c_str()));
        return false;
      }

      std::vector<uint8_t> page_data;
      uint64_t page_hash = VolumeDataHash::UNKNOWN;
      DataBlock dataBlock;
      bool sparse = false;

      if(metadata.size() >= sizeof(uint64_t))
      {
        uint64_t hash = VolumeDataHash::UNKNOWN;
        memcpy(&hash, metadata.data(), sizeof(uint64_t));

        // Check for sparse data
        if(hash == VolumeDataHash::UNKNOWN)
        {
          VolumeDataHash constantValueVolumeDataHash = m_layer->IsUseNoValue() ? VolumeDataHash(VolumeDataHash::NOVALUE) : VolumeDataHash(0.0f);
          m_accessManager->GetVolumeDataStore()->CreateConstantValueDataBlock(volumeDataChunk, m_layer->GetFormat(), pageImpl->GetNoValue(), m_layer->GetComponents(), constantValueVolumeDataHash, dataBlock, page_data, error);
          sparse = true;
        }
      }

      if (!sparse)
      {
        bool success = m_accessManager->GetVolumeDataStore()->DeserializeVolumeData(volumeDataChunk, serialized_data, metadata, compressionInfo.GetCompressionMethod(), compressionInfo.GetAdaptiveLevel(), pageImpl->GetFormat(), pageImpl->UseNoValue(), m_layer->GetNoValue(), pageImpl->GetNoValue(), dataBlock, page_data, page_hash, error);
        if(!success)
        {
          pageListMutexLock.lock();
          pageImpl->SetError(error);
          pageImpl->SetRequestPrepared(false);
          pageImpl->LeaveSettingData();
          m_pageReadCondition.notify_all();
          m_logger.LogError(fmt::format("Failed when deserializing chunk: {}", error.string.c_str()));
          return false;
        }
      }

      pageListMutexLock.lock();
      pageImpl->SetBufferData(dataBlock, m_layer->GetChunkDimensionGroup(), std::move(page_data), page_hash);
    }

    m_pagesRead++;
    pageImpl->SetRequestPrepared(false);
    pageImpl->LeaveSettingData();
    m_pageReadCondition.notify_all();
  }
  else
  {
    pageImpl->LeaveSettingData();
    pageListMutexLock.lock();
    m_pageReadCondition.wait(pageListMutexLock, [pageImpl]{return !pageImpl->SettingData();});
  }

  assert(pageListMutexLock.owns_lock());
  if (pageImpl->GetErrorInternal().code)
    return false;

  LimitPageListSize(m_maxPages, pageListMutexLock);
  return m_layer != nullptr;
}

void VolumeDataPageAccessorImpl::CancelPreparedReadPage(VolumeDataPageImpl* pageImpl)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  pageImpl->UnPin();
  if (pageImpl->IsPinned())
    return;

  auto page_it = m_pages.find(pageImpl->GetChunkIndex());
  if (page_it != m_pages.end())
  {
    m_pages_lru.Remove(pageImpl);
    m_pages.erase(page_it);
  }
  pageListMutexLock.unlock();

  if (pageImpl->RequestPrepared())
  {
    if(pageImpl->JobID() != -1)
    {
      m_accessManager->CancelAndWaitForCompletion(pageImpl->JobID());
    }
    else
    {
      VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(pageImpl->GetChunkIndex());
      Error error;
      m_accessManager->GetVolumeDataStore()->CancelReadChunk(volumeDataChunk, error);
      if (error.code)
      {
        pageImpl->SetError(error);
      }
    }
    pageImpl->SetRequestPrepared(false);
  }

  pageImpl->LeaveSettingData();
  delete pageImpl;
  m_pageReadCondition.notify_all();
}

bool
VolumeDataPageAccessorImpl::GetError(VolumeDataPageImpl *pageImpl, OpenVDS::Error &error)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  error = pageImpl->GetErrorInternal();
  return error.code != 0;
}

void VolumeDataPageAccessorImpl::CopyPage(int64_t chunkIndex, VolumeDataPageAccessor const &source)
{
  if (!m_layer)
  {
    return;
  }

  auto const &sourceVolumeDataPageAccessor = static_cast<VolumeDataPageAccessorImpl const &>(source);

  if(!sourceVolumeDataPageAccessor.m_layer || (sourceVolumeDataPageAccessor.m_layer->GetProduceStatus() == VolumeDataLayer::ProduceStatus_Unavailable))
  {
    throw InvalidOperation("The source volume data page accessor is not valid");
  }

  if(!(static_cast<VolumeDataPartition const &>(*sourceVolumeDataPageAccessor.m_layer) == static_cast<VolumeDataPartition const &>(*m_layer)))
  {
    throw InvalidOperation("The source volume data page accessor layout is not compatible with the layout of this volume data page accessor");
  }

  if(!IsReadWrite())
  {
    throw InvalidOperation("Trying to copy a page to a read-only volume data page accessor");
  }

  VolumeDataChunk chunk = m_layer->GetChunkFromIndex(chunkIndex);
  m_accessManager->AddCopyPageJob(chunk, *this, const_cast<VolumeDataPageAccessorImpl &>(sourceVolumeDataPageAccessor));
  m_pagesWritten++;
}

VolumeDataPage* VolumeDataPageAccessorImpl::ReadPage(int64_t chunk)
{
  if(!m_layer)
  {
    return nullptr;
  }

  if(!m_isLayerWriteLocked && m_layer->IsWriteLocked())
  {
    throw InvalidOperation("Invalid read from write locked layer, please make sure all previously created VolumeDataPageAccessors have committed their writes.");
  }

  Error error;

  VolumeDataPageImpl *page = PrepareReadPage(chunk, error);
  if (page)
  {
    (void)ReadPreparedPage(page);
  }
  return page;
}

VolumeDataPage* VolumeDataPageAccessorImpl::ReadPageAtPosition(const int (&position)[Dimensionality_Max])
{
  for (int i = 0; i < Dimensionality_Max; i++)
  {
    if(position[i] < 0 || position[i] >= m_layer->GetDimensionNumSamples(i))
    {
      return nullptr;
    }
  }
  return ReadPage(GetChunkIndex(position));
}

void VolumeDataPageAccessorImpl::LimitPageListSize(int maxPages, std::unique_lock<std::mutex>& pageListMutexLock)
{
  while(int(m_pages.size()) > m_maxPages)
  {
    // Wait for commit to finish before deleting a page
    while(m_isCommitInProgress)
    {
      m_commitFinishedCondition.wait_for(pageListMutexLock, std::chrono::milliseconds(1000));
    }

    // Find a page to evict
    VolumeDataPageImpl* page = nullptr;
    for (auto lru_node = m_pages_lru.GetLastItem(); lru_node; lru_node = lru_node->m_lru.m_prev)
    {
      if (!lru_node->IsPinned())
      {
        page = lru_node;
        break;
      }
    }
    if (!page)
      return;

    if(page->IsWritten() && m_layer)
    {
      // Finish reading all pages currently being read
      while(1)
      {
        bool isReadInProgress = false;
        for (auto targetPage = m_pages_lru.GetFirstItem(); targetPage; targetPage = targetPage->m_lru.m_next)
        {
          if (page->IsCopyMarginNeeded(targetPage))
          {
            if (targetPage->IsEmpty())
            {
              isReadInProgress = true;
              break;
            }
          }
        }

        if(!isReadInProgress)
        {
          break;
        }

        page->Pin(); // Make sure this page isn't deleted by LimitPageListSize!

        // Wait for the page getting read
        m_pageReadCondition.wait_for(pageListMutexLock, std::chrono::milliseconds(1000));

        page->UnPin();

        // Check if the page was pinned while we released the mutex, we have to abort evicting this page
        if(page->IsPinned())
        {
          break;
        }
      }

      // Check if the page was pinned while we released the mutex, we have to abort evicting this page
      if(page->IsPinned())
      {
        continue;
      }

      // Copy margins
      if(page->IsWritten())
      {
        for (auto targetPage = m_pages_lru.GetFirstItem(); targetPage; targetPage = targetPage->m_lru.m_next)
        {
          if(page->IsCopyMarginNeeded(targetPage))
          {
            page->CopyMargin(targetPage);
          }
        }
      }
    }

    m_pages.erase(page->GetChunkIndex());
    m_pages_lru.Remove(page);

    if(page->IsDirty() && m_layer)
    {
      page->WriteBack(m_layer, pageListMutexLock);
      m_pagesWritten++;
    }

    delete page;
  }
}

static int SerializeChunkMetadata(uint64_t hash, uint8_t (&adaptiveLevels)[WAVELET_ADAPTIVE_LEVELS], bool isWaveletAdaptive, void *buffer)
{
  assert(hash != VolumeDataHash::UNKNOWN);

  int chunkMetadataLength = (int)sizeof(uint64_t);

  memcpy(buffer, &hash, sizeof(hash));

  if (isWaveletAdaptive)
  {
    memcpy((char *)buffer + sizeof(uint64_t), adaptiveLevels, sizeof(uint8_t[WAVELET_ADAPTIVE_LEVELS]));
    chunkMetadataLength += (int)sizeof(uint8_t[WAVELET_ADAPTIVE_LEVELS]);
  }

  return chunkMetadataLength;
}

int64_t VolumeDataPageAccessorImpl::RequestWritePage(int64_t chunk, const DataBlock& dataBlock, const std::vector<uint8_t>& data, uint64_t hash)
{
  std::vector<uint8_t> serializedData;
  std::vector<uint8_t> metadata(64);
  uint8_t adaptiveLevels[WAVELET_ADAPTIVE_LEVELS];

  uint64_t serializedHash = VolumeDataStore::SerializeVolumeData({ m_layer, chunk }, dataBlock, data, m_layer->GetEffectiveCompressionMethod(), m_layer->GetEffectiveCompressionTolerance(), serializedData, adaptiveLevels);

  if(serializedHash != VolumeDataHash::UNKNOWN)
  {
    hash = serializedHash;
  }

  assert(hash != VolumeDataHash::UNKNOWN);

  auto metadata_size = SerializeChunkMetadata(hash, adaptiveLevels, CompressionMethod_IsWavelet(m_layer->GetEffectiveCompressionMethod()), metadata.data());
  metadata.resize(metadata_size);

  return m_accessManager->GetVolumeDataStore()->WriteChunk({ m_layer, chunk }, serializedData, metadata);
}

void VolumeDataPageAccessorImpl::AcquireLayerWriteLock()
{
  if(!m_isLayerWriteLocked)
  {
    if(!m_layer->AcquireWriteLock())
    {
      throw InvalidOperation("Couldn't acquire write lock, please make sure all previously created VolumeDataPageAccessors have committed their writes.");
    }
    m_isLayerWriteLocked = true;
  }
}

void VolumeDataPageAccessorImpl::CommitInternal(std::unique_lock<std::mutex>& pageListMutexLock)
{
  // Finish reading all pages currently being read
  for(auto page = m_pages_lru.GetFirstItem(); page; page = page->m_lru.m_next)
  {
    while(page->IsEmpty() && !page->IsCanceled())
    {
      // Wait for the page getting read
      m_pageReadCondition.wait_for(pageListMutexLock, std::chrono::milliseconds(1000));
    }
  }

  // Copy all margins
  for(auto page = m_pages_lru.GetFirstItem(); page; page = page->m_lru.m_next)
  {
    if(page->IsWritten())
    {
      for (auto targetPage = m_pages_lru.GetFirstItem(); targetPage; targetPage = targetPage->m_lru.m_next)
      {
        if(page->IsCopyMarginNeeded(targetPage))
        {
          page->CopyMargin(targetPage);
        }
      }
    }
  }

  for(auto page = m_pages_lru.GetFirstItem(); page; page = page->m_lru.m_next)
  {
    if(page->IsDirty() && m_layer)
    {
      page->WriteBack(m_layer, pageListMutexLock);
      m_pagesWritten++;
    }
  }

  if(m_isLayerWriteLocked)
  {
    if(m_layer)
    {
      m_layer->ReleaseWriteLock();
    }
    m_isLayerWriteLocked = false;
  }

  if(m_parentVolumeDataPageAccessor)
  {
    m_parentVolumeDataPageAccessor->CommitInternal(pageListMutexLock);
  }
}

void VolumeDataPageAccessorImpl::Commit()
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex);

  if(m_isCommitInProgress)
  {
    return;
  }

  // Make sure we don't start reading any new pages while we're finishing up the current waiting reads
  m_isCommitInProgress = true;
  CommitInternal(pageListMutexLock);
  m_isCommitInProgress = false;
  m_commitFinishedCondition.notify_all();
}

// ============================================================================
// Two-phase read support for parallel decompression
// ============================================================================

VolumeDataPageAccessorImpl::FetchedData VolumeDataPageAccessorImpl::FetchPageData(VolumeDataPageImpl* pageImpl)
{
  FetchedData result;

  if (!pageImpl->RequestPrepared())
  {
    // Page should already have valid data - validate this assumption
    const DataBlock& db = pageImpl->GetDataBlock();
    if (db.Dimensionality < 1 || db.Dimensionality > DataBlock::Dimensionality_Max)
    {
      m_logger.LogError(fmt::format("FetchPageData: RequestPrepared=false but DataBlock is invalid! "
        "Dimensionality={}, chunk={}", (int)db.Dimensionality, pageImpl->GetChunkIndex()));
      // This is a logic error - page claims to have data but doesn't
      assert(false && "FetchPageData: Page claims to have data (RequestPrepared=false) but DataBlock is invalid");
      result.success = false;
      result.error.code = -1;
      result.error.string = "Page has invalid DataBlock despite RequestPrepared=false";
      return result;
    }
    // Validate AllocatedSize values are reasonable
    for (int i = 0; i < db.Dimensionality; i++)
    {
      if (db.AllocatedSize[i] <= 0 || db.AllocatedSize[i] > 100000)
      {
        m_logger.LogError(fmt::format("FetchPageData: RequestPrepared=false but AllocatedSize[{}]={} is invalid! chunk={}",
          i, db.AllocatedSize[i], pageImpl->GetChunkIndex()));
        assert(false && "FetchPageData: Page has invalid AllocatedSize despite RequestPrepared=false");
        result.success = false;
        result.error.code = -1;
        result.error.string = "Page has invalid AllocatedSize despite RequestPrepared=false";
        return result;
      }
    }
    result.success = true;  // Page already has data (validated)
    return result;
  }

  // Handle jobID case (async prefetch already in progress)
  int64_t jobID = pageImpl->JobID();
  if (jobID != -1)
  {
    bool success = m_accessManager->WaitForCompletion(jobID);
    if (!success)
    {
      ReadErrorException readError("", 0);
      m_accessManager->IsCanceled(jobID, &readError);
      result.error.code = readError.GetErrorCode();
      result.error.string = readError.GetErrorMessage();
      return result;
    }
    result.success = true;
    return result;
  }

  // Perform I/O: fetch compressed data from storage
  VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(pageImpl->GetChunkIndex());
  CompressionInfo compressionInfo;

  if (!m_accessManager->GetVolumeDataStore()->ReadChunk(volumeDataChunk, m_layer->GetEffectiveWaveletAdaptiveLoadLevel(), result.serializedData, result.metadata, compressionInfo, result.error))
  {
    m_logger.LogError(fmt::format("FetchPageData: Failed when reading chunk: {}", result.error.string.c_str()));
    return result;
  }

  result.compressionMethod = compressionInfo.GetCompressionMethod();
  result.adaptiveLevel = compressionInfo.GetAdaptiveLevel();

  // Check for sparse data
  if (result.metadata.size() >= sizeof(uint64_t))
  {
    uint64_t hash = VolumeDataHash::UNKNOWN;
    memcpy(&hash, result.metadata.data(), sizeof(uint64_t));
    if (hash == VolumeDataHash::UNKNOWN)
    {
      result.sparse = true;
    }
  }

  result.success = true;
  return result;
}

bool VolumeDataPageAccessorImpl::DecompressPageData(VolumeDataPageImpl* pageImpl, FetchedData& fetchedData)
{
  std::unique_lock<std::mutex> pageListMutexLock(m_pagesMutex, std::defer_lock);

  if (!pageImpl->RequestPrepared())
  {
    pageListMutexLock.lock();
    return pageImpl->GetErrorInternal().code == 0;
  }

  if (!fetchedData.success)
  {
    pageListMutexLock.lock();
    pageImpl->SetError(fetchedData.error);
    pageImpl->SetRequestPrepared(false);
    pageImpl->LeaveSettingData();
    m_pageReadCondition.notify_all();
    return false;
  }

  // Note: Empty serializedData with !sparse can occur for constant value chunks.
  // Let DeserializeVolumeData handle all cases - it will fill data for constants
  // or fail with an appropriate error if the data is truly missing.

  Error error;
  std::vector<uint8_t> page_data;
  uint64_t page_hash = VolumeDataHash::UNKNOWN;
  DataBlock dataBlock;

  VolumeDataChunk volumeDataChunk = m_layer->GetChunkFromIndex(pageImpl->GetChunkIndex());

  if (fetchedData.sparse)
  {
    VolumeDataHash constantValueVolumeDataHash = m_layer->IsUseNoValue() ? VolumeDataHash(VolumeDataHash::NOVALUE) : VolumeDataHash(0.0f);
    m_accessManager->GetVolumeDataStore()->CreateConstantValueDataBlock(volumeDataChunk, m_layer->GetFormat(), pageImpl->GetNoValue(), m_layer->GetComponents(), constantValueVolumeDataHash, dataBlock, page_data, error);
  }
  else
  {
    bool success = m_accessManager->GetVolumeDataStore()->DeserializeVolumeData(
      volumeDataChunk,
      fetchedData.serializedData,
      fetchedData.metadata,
      fetchedData.compressionMethod,
      fetchedData.adaptiveLevel,
      pageImpl->GetFormat(),
      pageImpl->UseNoValue(),
      m_layer->GetNoValue(),
      pageImpl->GetNoValue(),
      dataBlock,
      page_data,
      page_hash,
      error);

    if (!success)
    {
      pageListMutexLock.lock();
      pageImpl->SetError(error);
      pageImpl->SetRequestPrepared(false);
      pageImpl->LeaveSettingData();
      m_pageReadCondition.notify_all();
      m_logger.LogError(fmt::format("DecompressPageData: Failed when deserializing chunk: {}", error.string.c_str()));
      return false;
    }
  }

  pageListMutexLock.lock();
  pageImpl->SetBufferData(dataBlock, m_layer->GetChunkDimensionGroup(), std::move(page_data), page_hash);

  m_pagesRead++;
  pageImpl->SetRequestPrepared(false);
  pageImpl->LeaveSettingData();
  m_pageReadCondition.notify_all();

  LimitPageListSize(m_maxPages, pageListMutexLock);
  return m_layer != nullptr;
}

}
