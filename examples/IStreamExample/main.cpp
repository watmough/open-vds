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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <objidl.h>

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

// Link with shlwapi.lib for SHCreateStreamOnFileEx
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

// Helper function to get compression method name
const char* GetCompressionMethodName(OpenVDS::CompressionMethod method)
{
  switch (method)
  {
  case OpenVDS::CompressionMethod::None: return "None";
  case OpenVDS::CompressionMethod::Wavelet: return "Wavelet";
  case OpenVDS::CompressionMethod::RLE: return "RLE";
  case OpenVDS::CompressionMethod::Zip: return "Zip";
  case OpenVDS::CompressionMethod::WaveletNormalizeBlock: return "WaveletNormalizeBlock";
  case OpenVDS::CompressionMethod::WaveletLossless: return "WaveletLossless";
  case OpenVDS::CompressionMethod::WaveletNormalizeBlockLossless: return "WaveletNormalizeBlockLossless";
  default: return "Unknown";
  }
}

// Helper function to get format name
const char* GetFormatName(OpenVDS::VolumeDataChannelDescriptor::Format format)
{
  switch (format)
  {
  case OpenVDS::VolumeDataChannelDescriptor::Format_U8: return "U8";
  case OpenVDS::VolumeDataChannelDescriptor::Format_U16: return "U16";
  case OpenVDS::VolumeDataChannelDescriptor::Format_R32: return "R32 (float)";
  case OpenVDS::VolumeDataChannelDescriptor::Format_U32: return "U32";
  case OpenVDS::VolumeDataChannelDescriptor::Format_R64: return "R64 (double)";
  case OpenVDS::VolumeDataChannelDescriptor::Format_U64: return "U64";
  case OpenVDS::VolumeDataChannelDescriptor::Format_1Bit: return "1Bit";
  default: return "Unknown";
  }
}

// Print VDS information
void PrintVDSInfo(const std::wstring& filePath)
{
  std::wcout << L"\n========================================\n";
  std::wcout << L"File: " << filePath << L"\n";
  std::wcout << L"========================================\n";

  // Create IStream from file
  IStream* pStream = nullptr;
  HRESULT hr = SHCreateStreamOnFileEx(
    filePath.c_str(),
    STGM_READ | STGM_SHARE_DENY_WRITE,
    0,
    FALSE,
    nullptr,
    &pStream
  );

  if (FAILED(hr))
  {
    std::wcerr << L"Failed to create IStream from file. HRESULT: 0x"
               << std::hex << hr << std::dec << L"\n";
    return;
  }

  // Open VDS using IStreamOpenOptions
  OpenVDS::IStreamOpenOptions options(pStream);
  OpenVDS::Error error;
  OpenVDS::VDSHandle handle = OpenVDS::Open(options, error);

  if (error.code != 0)
  {
    std::cerr << "Failed to open VDS: " << error.string << "\n";
    pStream->Release();
    return;
  }

  // Get layout
  OpenVDS::VolumeDataLayout* layout = OpenVDS::GetLayout(handle);

  // Print basic information
  std::cout << "\nVDS Information:\n";
  std::cout << "  Dimensionality: " << layout->GetDimensionality() << "\n";

  // Print dimensions
  std::cout << "\nDimensions:\n";
  for (int dim = 0; dim < layout->GetDimensionality(); dim++)
  {
    auto axis = layout->GetAxisDescriptor(dim);
    std::cout << "  [" << dim << "] " << axis.GetName() << ":\n";
    std::cout << "      Samples: " << layout->GetDimensionNumSamples(dim) << "\n";
    std::cout << "      Coordinate range: [" << axis.GetCoordinateMin()
              << ", " << axis.GetCoordinateMax() << "]\n";
    std::cout << "      Unit: " << axis.GetUnit() << "\n";
  }

  // Print channels
  std::cout << "\nChannels: " << layout->GetChannelCount() << "\n";
  for (int ch = 0; ch < layout->GetChannelCount(); ch++)
  {
    auto channel = layout->GetChannelDescriptor(ch);
    std::cout << "  [" << ch << "] " << channel.GetName() << ":\n";
    std::cout << "      Format: " << GetFormatName(channel.GetFormat()) << "\n";
    std::cout << "      Components: " << static_cast<int>(channel.GetComponents()) << "\n";
    std::cout << "      Value range: [" << channel.GetValueRangeMin()
              << ", " << channel.GetValueRangeMax() << "]\n";
  }

  // Print compression
  OpenVDS::CompressionMethod compressionMethod = OpenVDS::GetCompressionMethod(handle);
  std::cout << "\nCompression:\n";
  std::cout << "  Method: " << GetCompressionMethodName(compressionMethod) << "\n";

  if (compressionMethod == OpenVDS::CompressionMethod::Wavelet ||
      compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlock)
  {
    float tolerance = OpenVDS::GetCompressionTolerance(handle);
    std::cout << "  Tolerance: " << tolerance << "\n";

    // Print wavelet adaptive levels
    auto adaptiveLevels = OpenVDS::GetWaveletAdaptiveLevels(handle);
    if (!adaptiveLevels.empty())
    {
      std::cout << "  Adaptive levels: " << adaptiveLevels.size() << "\n";
      for (size_t i = 0; i < adaptiveLevels.size(); i++)
      {
        std::cout << "    [" << i << "] Tolerance: " << adaptiveLevels[i].compressionTolerance
                  << ", Ratio: " << adaptiveLevels[i].compressionRatio
                  << ", Size: " << adaptiveLevels[i].compressedSize << "\n";
      }
    }
  }

  // Print chunk information
  auto layoutDescriptor = layout->GetLayoutDescriptor();
  int brickSize = 1 << layoutDescriptor.GetBrickSize(); // BrickSize is log2 of actual size

  std::cout << "\nChunk Layout:\n";
  std::cout << "  Brick size: " << brickSize << "\n";

  int64_t totalChunks = 1;
  for (int dim = 0; dim < layout->GetDimensionality(); dim++)
  {
    int chunkCount = (layout->GetDimensionNumSamples(dim) + brickSize - 1) / brickSize;
    std::cout << "  Dimension " << dim << ": " << chunkCount << " chunks\n";
    totalChunks *= chunkCount;
  }
  std::cout << "  Total chunks (approx): " << totalChunks << "\n";

  // Close VDS
  OpenVDS::Close(handle);

  // Release IStream
  pStream->Release();

  std::cout << "\n";
}

int main(int argc, char* argv[])
{
  std::cout << "OpenVDS IStream Example\n";
  std::cout << "=======================\n\n";
  std::cout << "This example demonstrates opening VDS files using Windows IStream.\n\n";

  // Directory to scan for VDS files
  std::wstring vdsDirectory = L"C:\\Shared\\example-vds-data";

  // Allow override from command line
  if (argc > 1)
  {
    // Convert from char* to wstring
    int wideCharCount = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    if (wideCharCount > 0)
    {
      std::vector<wchar_t> buffer(wideCharCount);
      MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, buffer.data(), wideCharCount);
      vdsDirectory = buffer.data();
    }
  }

  std::wcout << L"Scanning directory: " << vdsDirectory << L"\n";

  // Check if directory exists
  if (!fs::exists(vdsDirectory))
  {
    std::wcerr << L"Directory does not exist: " << vdsDirectory << L"\n";
    std::wcerr << L"Usage: " << argv[0] << L" [directory]\n";
    std::wcerr << L"  If no directory is specified, defaults to C:\\Shared\\example-vds-data\n";
    return 1;
  }

  // Find all .vds files
  std::vector<std::wstring> vdsFiles;
  try
  {
    for (const auto& entry : fs::directory_iterator(vdsDirectory))
    {
      if (entry.is_regular_file() && entry.path().extension() == L".vds")
      {
        vdsFiles.push_back(entry.path().wstring());
      }
    }
  }
  catch (const fs::filesystem_error& e)
  {
    std::cerr << "Error scanning directory: " << e.what() << "\n";
    return 1;
  }

  if (vdsFiles.empty())
  {
    std::wcout << L"No .vds files found in " << vdsDirectory << L"\n";
    return 0;
  }

  std::cout << "Found " << vdsFiles.size() << " VDS file(s)\n";

  // Process each VDS file
  for (const auto& filePath : vdsFiles)
  {
    try
    {
      PrintVDSInfo(filePath);
    }
    catch (const std::exception& e)
    {
      std::cerr << "Exception processing file: " << e.what() << "\n";
    }
  }

  std::cout << "\nProcessed " << vdsFiles.size() << " file(s) successfully.\n";
  return 0;
}

#else
// Non-Windows platforms
#include <iostream>
int main()
{
  std::cerr << "This example is only available on Windows (requires IStream support)\n";
  return 1;
}
#endif
