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

#ifndef VOLUMEDATAREQUESTPROCESSOR_H
#define VOLUMEDATAREQUESTPROCESSOR_H

#include <OpenVDS/VolumeData.h>
#include <OpenVDS/OpenVDS.h>

#include "DimensionGroup.h"
#include "VolumeDataPageAccessorImpl.h"
#include "VolumeDataChunk.h"
#include <ThreadPool/ThreadPool.h>

#include <stdint.h>
#include <map>
#include <functional>
#include <vector>

namespace OpenVDS
{

class VolumeDataPage;

struct PageAccessorKey
{
  DimensionsND dimensionsND;
  int32_t lod;
  int32_t channel;
  VolumeDataFormat format;
  float noValue;
  bool useNoValue;


  bool operator<(const PageAccessorKey &other) const
  {
    if (dimensionsND == other.dimensionsND)
    {
      if (lod == other.lod)
      {
        if (channel == other.channel)
        {
          if (format == other.format)
          {
            if (useNoValue && other.useNoValue)
            {
              return noValue < other.noValue;
            }
            return useNoValue < other.useNoValue;
          }
          return format < other.format;
        }
        return channel < other.channel;
      }
      return lod < other.lod;
    }
    return dimensionsND < other.dimensionsND;
  }
};

struct JobPage
{
  JobPage(VolumeDataPageImpl *page, const VolumeDataChunk &chunk)
    : page(page)
    , chunk(chunk)
  {}
  VolumeDataPageImpl *page;
  VolumeDataChunk chunk;
};

// Holds fetched (but not yet decompressed) chunk data
// Used for two-phase processing: sequential I/O followed by parallel decompression
struct FetchedPageData
{
  bool success = false;
  bool sparse = false;
  std::vector<uint8_t> serializedData;
  std::vector<uint8_t> metadata;
  CompressionMethod compressionMethod = CompressionMethod::None;
  int adaptiveLevel = 0;
  Error error;
};

struct PageAccessorNotifier
{
  PageAccessorNotifier(std::mutex &mutex)
    : dirty(false)
    , exit(false)
    , mutex(mutex)
  {}

  void setDirty()
  {
    std::unique_lock<std::mutex> lock(mutex);
    dirty = true;
    jobNotification.notify_all();
  }
  void setDirtyNoLock()
  {
    dirty = true;
    jobNotification.notify_all();
  }

  void setExit()
  {
    std::unique_lock<std::mutex> lock(mutex);
    exit = true;
    jobNotification.notify_all();
  }

  bool dirty;
  bool exit;
  std::mutex &mutex;
  std::condition_variable jobNotification;
};

struct Job
{
  Job(int64_t jobId, PageAccessorNotifier &pageAccessorNotifier, VolumeDataPageAccessorImpl &pageAccessor, int pagesCount)
    : jobId(jobId)
    , pageAccessorNotifier(pageAccessorNotifier)
    , pageAccessor(pageAccessor)
    , pagesProcessed(0)
    , done(false)
    , cancelled(false)
    , pagesCount(pagesCount)
  {}

  int64_t jobId;
  PageAccessorNotifier &pageAccessorNotifier;
  VolumeDataPageAccessorImpl &pageAccessor;
  std::vector<JobPage> pages;
  std::vector<std::future<Error>> future;
  std::atomic_int pagesProcessed;
  std::atomic_bool done;
  std::atomic_bool cancelled;
  int pagesCount;
  Error completedError;
};

class VolumeDataRequestProcessor
{
public:
  VolumeDataRequestProcessor(VolumeDataAccessManagerImpl &manager, int requestThreadCount, Logger &logger);
  ~VolumeDataRequestProcessor();

  int64_t AddJob(const std::vector<VolumeDataChunk> &chunks, VolumeDataFormat format, std::pair<bool, float> noValue, std::function<bool(VolumeDataPageImpl *page, const VolumeDataChunk &volumeDataChunk, Error &error)> processor, bool singleThread = false);
  bool  IsActive(int64_t requestID);
  bool  IsCompleted(int64_t requestID);
  bool  IsCanceled(int64_t requestID, Error &error);
  bool  WaitForCompletion(int64_t requestID, int millisecondsBeforeTimeout = 0);
  void  Cancel(int64_t requestID);
  float GetCompletionFactor(int64_t requestID);

  int CountActivePages();

  int64_t RequestVolumeSubset(void *buffer, VolumeDataLayer const *volumeDataLayer, const int32_t(&minRequested)[Dimensionality_Max], const int32_t (&maxRequested)[Dimensionality_Max], int32_t LOD, VolumeDataChannelDescriptor::Format format, bool isReplaceNoValue, float replacementNoValue);
  int64_t RequestProjectedVolumeSubset(void *buffer, VolumeDataLayer const *volumeDataLayer, const int32_t (&minRequested)[Dimensionality_Max], const int32_t (&maxRequested)[Dimensionality_Max], FloatVector4 const &voxelPlane, DimensionGroup projectedDimensions, int32_t LOD, VolumeDataChannelDescriptor::Format format, InterpolationMethod interpolationMethod, bool isReplaceNoValue, float replacementNoValue);
  int64_t RequestVolumeSamples(void *buffer, VolumeDataLayer const *volumeDataLayer, const float(*samplePositions)[Dimensionality_Max], int32_t samplePosCount, InterpolationMethod interpolationMethod, bool isReplaceNoValue, float replacementNoValue);
  int64_t RequestVolumeTraces(void *buffer, VolumeDataLayer const *volumeDataLayer, const float(*tracePositions)[Dimensionality_Max], int32_t tracePositionsCount, int32_t LOD, InterpolationMethod interpolationMethod, int32_t traceDimension, bool isReplaceNoValue, float replacementNoValue);
  int64_t PrefetchVolumeChunk(VolumeDataLayer const *volumeDataLayer, int64_t chunkIndex);
  int64_t RequestRemap(VolumeDataPageImpl& targetPage, std::vector<VolumeDataChunk> const &sourceChunks);

  static int64_t StaticGetVolumeSubsetBufferSize(VolumeDataLayout const *volumeDataLayout, const int (&minVoxelCoordinates)[Dimensionality_Max], const int (&maxVoxelCoordinates)[Dimensionality_Max], VolumeDataChannelDescriptor::Format format, int LOD, int channel);
  static int64_t StaticGetProjectedVolumeSubsetBufferSize(VolumeDataLayout const *volumeDataLayout, const int (&minVoxelCoordinates)[Dimensionality_Max], const int (&maxVoxelCoordinates)[Dimensionality_Max], DimensionGroup projectedDimensions, VolumeDataChannelDescriptor::Format format, int LOD, int channel);
  static int64_t StaticGetVolumeSamplesBufferSize(VolumeDataLayout const *volumeDataLayout, int sampleCount, int channel);
  static int64_t StaticGetVolumeTracesBufferSize(VolumeDataLayout const *volumeDataLayout, int traceCount, int traceDimension, int LOD, int channel);

  ThreadPool& GetThreadPool() { return m_threadPool; }

#ifdef OPENVDS_SINGLE_THREADED
  // Get the decompression thread pool (always multi-threaded for parallel decompression)
  AsyncThreadPool& GetDecompressionThreadPool() { return *m_decompressionThreadPool; }
#endif

private:
  VolumeDataAccessManagerImpl &m_manager;
  std::map<PageAccessorKey, VolumeDataPageAccessorImpl *> m_pageAccessors;
  std::vector<std::unique_ptr<Job>> m_jobs;
  std::mutex m_mutex;
  PageAccessorNotifier m_pageAccessorNotifier;
  ThreadPool m_threadPool;
  std::thread m_cleanupThread;
  Logger &m_logger;

#ifdef OPENVDS_SINGLE_THREADED
  // Separate thread pool for decompression - always multi-threaded even when
  // OPENVDS_SINGLE_THREADED is defined. This allows parallel decompression
  // while keeping I/O single-threaded for COM/IStream compatibility.
  std::unique_ptr<AsyncThreadPool> m_decompressionThreadPool;
#endif
};

}

#endif
