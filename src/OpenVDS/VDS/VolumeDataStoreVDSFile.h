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

#ifndef VOLUMEDATASTOREVDSFILE_H
#define VOLUMEDATASTOREVDSFILE_H

#include <BulkDataStore/HueBulkDataStore.h>
#include <BulkDataStore/HueBulkDataStoreFileTypes.h>

#include "VDS.h"
#include "MetadataManager.h"
#include "VolumeDataStore.h"

#include <mutex>

#ifdef _WIN32
struct IStream;  // Forward declaration for Windows COM IStream
#endif

namespace OpenVDS
{

class VolumeDataStoreVDSFile : public VolumeDataStore
{
  struct LayerFile
  {
    HueBulkDataStore::FileInterface *fileInterface;
    VDSLayerMetadataWaveletAdaptive layerMetadata;
    bool layerChunksWaveletAdaptive;
    bool dirty;

    LayerFile() = default;
    LayerFile(HueBulkDataStore::FileInterface *fileInterface, VDSLayerMetadataWaveletAdaptive const &layerMetadata, bool layerChunksWaveletAdaptive, bool dirty)
      : fileInterface(fileInterface), layerMetadata(layerMetadata), layerChunksWaveletAdaptive(layerChunksWaveletAdaptive), dirty(dirty)
    {}
  };

  VDS &m_vds;

  bool m_isVDSObjectFilePresent;
  bool m_isVolumeDataLayoutFilePresent;
  std::map<std::string, LayerFile> m_layerFiles;
  std::unique_ptr<HueBulkDataStore, void (*)(HueBulkDataStore *)> m_dataStore;

  std::vector<uint8_t> ParseVDSObject(std::string const &parseString);

  LayerFile *GetLayerFile(std::string const &layerName) const;
  LayerFile *GetLayerFile(const VolumeDataLayer &volumeDataLayer) const { return GetLayerFile(GetLayerName(volumeDataLayer)); }

public:
  enum Mode
  {
    ReadOnly,
    ReadWrite,
    Create,
  };

  CompressionInfo
                GetEffectiveAdaptiveLevel(VolumeDataLayer* volumeDataLayer, WaveletAdaptiveMode waveletAdaptiveMode, float tolerance, float ratio) override;
  bool          GetWaveletAdaptiveLevelSizes(VolumeDataLayer* volumeDataLayer, float &baseCompressionTolerance, int64_t &uncompressedSize, int64_t (&adaptiveLevelSizes)[WAVELET_ADAPTIVE_LEVELS]) override;
  bool          PrepareReadChunkImpl(const VolumeDataChunk &volumeDataChunk, int adaptiveLevel, Error &error) override;
  bool          ReadChunkImpl(const VolumeDataChunk& chunk, int adaptiveLevel, std::vector<uint8_t>& serializedData, std::vector<uint8_t>& metadata, CompressionInfo& compressionInfo, Error& error) override;
  bool          CancelReadChunkImpl(const VolumeDataChunk& chunk, Error& error) override;
  bool          ReadChunkDataHash(const VolumeDataChunk& chunk, uint64_t &chunkDataHash, Error& error) override;
  bool          WriteChunkImpl(const VolumeDataChunk& chunk, std::shared_ptr<std::vector<uint8_t>>& serializedData, const std::vector<uint8_t>& metadata, std::function<void(const Error &error)> completed) override;
  void          Flush(Error &error) override;
  bool          ReadSerializedVolumeDataLayout(std::vector<uint8_t>& serializedVolumeDataLayout, Error &error) override;
  bool          WriteSerializedVolumeDataLayout(const std::vector<uint8_t>& serializedVolumeDataLayout, Error &error) override;
  bool          SerializeAndWriteLayerStatus(Error& error) override;
  bool          AddLayer(VolumeDataLayer* volumeDataLayer, int chunkMetadataPageSize, bool overwriteExisting) override;
  bool          RemoveLayer(VolumeDataLayer* volumeDataLayer) override { return false; }
  bool          Close(Error &error) override;
  bool          EnableWriting(Error& error) override;

  std::function<bool(std::string const& channelName, bool isPrimary)>
                IsChannelZipped() const override;

  VolumeDataStoreVDSFile(VDS &vds, const std::string &fileName, Mode mode, Error &error);
#ifdef _WIN32
  VolumeDataStoreVDSFile(VDS &vds, ::IStream* pStream, Error &error);
#endif
 ~VolumeDataStoreVDSFile();
};

}

#endif //VOLUMEDATASTOREVDSFILE_H
