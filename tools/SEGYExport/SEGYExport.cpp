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

#include <SEGYUtils/SEGY.h>
#include "IO/File.h"

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataAccess.h>
#include <OpenVDS/VolumeDataLayout.h>

#include <cxxopts.hpp>

#include <cstdlib>
#include <climits>
#include <json/json.h>
#include <cassert>
#include <fmt/format.h>
#include <chrono>

#include <PrintHelpers.h>
#include <HelpConnection.h>

#ifdef _WIN32
#include <WindowsUnicodeMain.h>
#endif

#include "SEGYUtils/VDSSEGYInfo.h"

#ifndef WIN32
#include <signal.h>
#endif
template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamplesToSEGY(const float* prVDSData, unsigned char* puSEGYData, int iSampleMin, int iSampleMax)
{
  if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat)
  {
    SEGY::Ieee2ibm(puSEGYData + iSampleMin * 4, prVDSData, iSampleMax - iSampleMin);
    return;
  }

  int
    nValue = 0;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat)
    {
      nValue = SEGY::FloatToInt(*prVDSData++);
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int32)
    {
      nValue = (int32_t)*prVDSData++; // Intentionally lose precision
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt32)
    {
      nValue = (uint32_t)*prVDSData++; // Intentionally lose precision
    }
    else
    {
      assert(0 && "Bad data sample format");
    }

    if (ENDIANNESS == SEGY::Endianness::BigEndian)
    {
      puSEGYData[iSample * 4 + 0] = (nValue >> 24) & 0xff;
      puSEGYData[iSample * 4 + 1] = (nValue >> 16) & 0xff;
      puSEGYData[iSample * 4 + 2] = (nValue >> 8) & 0xff;
      puSEGYData[iSample * 4 + 3] = nValue & 0xff;
    }
    else
    {
      puSEGYData[iSample * 4 + 3] = (nValue >> 24) & 0xff;
      puSEGYData[iSample * 4 + 2] = (nValue >> 16) & 0xff;
      puSEGYData[iSample * 4 + 1] = (nValue >> 8) & 0xff;
      puSEGYData[iSample * 4 + 0] = nValue & 0xff;
    }
  }
}

template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamplesToSEGY(const uint8_t* puVDSData, unsigned char* puSEGYData, int iSampleMin, int iSampleMax)
{
  int8_t
    nValue;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    nValue = *puVDSData++;

    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int8)
    {
      puSEGYData[iSample * 1 + 0] = nValue - SEGY::GetVDSIntegerOffsetForDataSampleFormat<FORMAT>();
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt8)
    {
      puSEGYData[iSample * 1 + 0] = (uint8_t)nValue;
    }
    else
    {
      assert(0 && "Bad data sample format");
    }

  }
}

template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamplesToSEGY(const uint16_t* puVDSData, unsigned char* puSEGYData, int iSampleMin, int iSampleMax)
{
  int16_t
    nValue = 0;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int16)
    {
      nValue = *puVDSData++ - SEGY::GetVDSIntegerOffsetForDataSampleFormat<FORMAT>();
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt16)
    {
      nValue = *puVDSData++;
    }
    else
    {
      assert(0 && "Bad data sample format");
    }

    if (ENDIANNESS == SEGY::Endianness::BigEndian)
    {
      puSEGYData[iSample * 2 + 0] = (nValue >> 8) & 0xff;
      puSEGYData[iSample * 2 + 1] = nValue & 0xff;
    }
    else
    {
      nValue = (int16_t)(puSEGYData[iSample * 2 + 1] << 8 | puSEGYData[iSample * 2 + 0]);
      puSEGYData[iSample * 2 + 0] = nValue & 0xff;
      puSEGYData[iSample * 2 + 1] = (nValue >> 8) & 0xff;
    }
  }
}

template<typename T, SEGY::Endianness ENDIANNESS>
void
copySamplesToSEGY(SEGY::BinaryHeader::DataSampleFormatCode dataSampleFormatCode, const T* source, unsigned char* puTarget, int iSampleMin, int iSampleMax)
{
  switch (dataSampleFormatCode)
  {
  case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt32>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int32>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt16>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int16>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt8>(source, puTarget, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
    return copySamplesToSEGY<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int8>(source, puTarget, iSampleMin, iSampleMax);
  default:
    assert(false && "Unsupported data sample format");
  }
}

template<typename T>
void
copySamplesToSEGY(SEGY::Endianness endianess, SEGY::BinaryHeader::DataSampleFormatCode dataSampleFormatCode, const T* source, unsigned char* puTarget, int iSampleMin, int iSampleMax)
{
  switch (endianess)
  {
  case SEGY::Endianness::BigEndian:
    return copySamplesToSEGY<T, SEGY::Endianness::BigEndian>(dataSampleFormatCode, source, puTarget, iSampleMin, iSampleMax);
  case SEGY::Endianness::LittleEndian:
    return copySamplesToSEGY<T, SEGY::Endianness::LittleEndian>(dataSampleFormatCode, source, puTarget, iSampleMin, iSampleMax);
  }
}

#ifdef UNICODE_MAIN
int UNICODE_MAIN(int argc, char **argv)
#else
int
main(int argc, char *argv[])
#endif
{
#ifndef WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
  //auto start_time = std::chrono::high_resolution_clock::now();

  cxxopts::Options options("SEGYExport", "SEGYExport - A tool to export a volume data store (VDS) to a SEG-Y file\n\nUse -H or see online documentation for connection string paramters:\nhttps://osdu.pages.opengroup.org/platform/domain-data-mgmt-services/seismic/open-vds/connection.html\n");
  options.positional_help("<output file>");

  std::string url;
  std::string connection;
  std::string persistentID;
  std::string fileName;
  bool useJsonOutput = false;
  bool help = false;
  bool helpConnection = false;
  bool version = false;
  bool disableInfo = false;
  bool disableWarning = false;

  options.add_option("", "", "url", "Url with vendor specific protocol or VDS file name.", cxxopts::value<std::string>(url), "<string>");
  options.add_option("", "", "connection", "Vendor specific connection string.", cxxopts::value<std::string>(connection), "<string>");
  options.add_option("", "", "vdsfile", "Input VDS file name.", cxxopts::value<std::string>(url), "<string>");
  options.add_option("", "", "persistentID", "A globally unique ID for the VDS, usually an 8-digit hexadecimal number.", cxxopts::value<std::string>(persistentID), "<ID>");

  options.add_option("", "q", "quiet", "Disable info level output.", cxxopts::value<bool>(disableInfo), "");
  options.add_option("", "Q", "very-quiet", "Disable warning level output.", cxxopts::value<bool>(disableWarning), "");
  options.add_option("", "", "json-output", "Enable json output.", cxxopts::value<bool>(useJsonOutput), "");
  options.add_option("", "h", "help", "Print this help information", cxxopts::value<bool>(help), "");
  options.add_option("", "H", "help-connection", "Print help information about the connection string", cxxopts::value<bool>(helpConnection), "");
  options.add_option("", "", "version", "Print version information.", cxxopts::value<bool>(version), "");

  options.add_option("", "", "output", "", cxxopts::value<std::string>(fileName), "");

  options.parse_positional("output");

  OpenVDS::OutputPrinter outputPrinter(useJsonOutput, OpenVDS::OutputPrinter::getLogLevel(disableWarning, disableInfo));
  if(argc == 1)
  {
    outputPrinter.printError("Args", options.help());
    return EXIT_SUCCESS;
  }

  try
  {
    options.parse(argc, argv);
  }
  catch(cxxopts::OptionParseException &e)
  {
    outputPrinter.printError("Args", e.what());
    return EXIT_FAILURE;
  }

  if (help)
  {
    outputPrinter.printInfo("Args", options.help());
    return EXIT_SUCCESS;
  }
  if (helpConnection)
  {
    outputPrinter.printInfo("Args", GetConnectionHelpString());
    return EXIT_SUCCESS;
  }

  if (version)
  {
    outputPrinter.printVersion("SEGYExport");
    return EXIT_SUCCESS;
  }

  if(fileName.empty())
  {
    outputPrinter.printError("Args", "No output SEG-Y file specified");
    return EXIT_FAILURE;
  }

  // Open the VDS
  if (!persistentID.empty())
  {
    if (!url.empty() && url.back() != '/')
    {
      url.push_back('/');
    }
    url.insert(url.end(), persistentID.begin(), persistentID.end());
  }

  OpenVDS::Error openError;

  OpenVDS::ScopedVDSHandle handle;
  
  if(OpenVDS::IsSupportedProtocol(url))
  {
    handle = OpenVDS::Open(url, connection, openError);
  }
  else
  {
    handle = OpenVDS::Open(OpenVDS::VDSFileOpenOptions(url), openError);
  }

  if(openError.code != 0)
  {
    outputPrinter.printError("VDS", "Could not open VDS", openError.string);
    return EXIT_FAILURE;
  }

  outputPrinter.printVersion("SEGYExport");

  auto accessManager = OpenVDS::GetAccessManager(handle);
  auto volumeDataLayout = accessManager.GetVolumeDataLayout();

  int dimensionality = accessManager.GetVolumeDataLayout()->GetDimensionality();
  int outerDimension = std::max(2, dimensionality - 1);

  // Find a dimension group containing (at least) the inner dimensions and can be produced
  OpenVDS::DimensionsND dimensionGroup = OpenVDS::Dimensions_01;

  if(outerDimension == 1)
  {
    dimensionGroup = OpenVDS::Dimensions_02;
  }

  if(accessManager.GetVDSProduceStatus(dimensionGroup, 0, 0) != OpenVDS::VDSProduceStatus::Normal)
  {
    if(dimensionality == 4 && outerDimension == 2)
    {
      if(accessManager.GetVDSProduceStatus(OpenVDS::Dimensions_013, 0, 0) == OpenVDS::VDSProduceStatus::Normal || 
        (accessManager.GetVDSProduceStatus(OpenVDS::Dimensions_013, 0, 0) == OpenVDS::VDSProduceStatus::Remapped && accessManager.GetVDSProduceStatus(dimensionGroup, 0, 0) == OpenVDS::VDSProduceStatus::Unavailable))
      {
        dimensionGroup = OpenVDS::Dimensions_013;
      }
    }
    else
    {
      if(accessManager.GetVDSProduceStatus(OpenVDS::Dimensions_012, 0, 0) == OpenVDS::VDSProduceStatus::Normal || 
        (accessManager.GetVDSProduceStatus(OpenVDS::Dimensions_012, 0, 0) == OpenVDS::VDSProduceStatus::Remapped && accessManager.GetVDSProduceStatus(dimensionGroup, 0, 0) == OpenVDS::VDSProduceStatus::Unavailable))
      {
        dimensionGroup = OpenVDS::Dimensions_012;
      }
    }
  }

  if(accessManager.GetVDSProduceStatus(dimensionGroup, 0, 0) == OpenVDS::VDSProduceStatus::Unavailable)
  {
    outputPrinter.printError("VDS", "VDS cannot produce data");
    return EXIT_FAILURE;
  }

  if(!volumeDataLayout->IsChannelAvailable("Trace"))
  {
    outputPrinter.printError("VDS", "VDS has no \"Trace\" channel");
    return EXIT_FAILURE;
  }
  int traceFlagChannel = volumeDataLayout->GetChannelIndex("Trace");

  if(!volumeDataLayout->IsChannelAvailable("SEGYTraceHeader"))
  {
    outputPrinter.printError("VDS", "VDS has no \"SEGYTraceHeader\" channel");
    return EXIT_FAILURE;
  }
  int segyTraceHeaderChannel = volumeDataLayout->GetChannelIndex("SEGYTraceHeader");

  if((!volumeDataLayout->IsMetadataBLOBAvailable("SEGY", "TextHeader")   && !volumeDataLayout->IsMetadataBLOBAvailable("", "SEGYTextHeader")) ||
     (!volumeDataLayout->IsMetadataBLOBAvailable("SEGY", "BinaryHeader") && !volumeDataLayout->IsMetadataBLOBAvailable("", "SEGYBinaryHeader")))
  {
    outputPrinter.printError("VDS", "SEG-Y Text/Binary headers not found");
    return EXIT_FAILURE;
  }

  // Write headers
  std::vector<uint8_t> textHeader(SEGY::TextualFileHeaderSize);
  std::vector<uint8_t> binaryHeader(SEGY::BinaryFileHeaderSize);

  if(volumeDataLayout->IsMetadataBLOBAvailable("SEGY", "TextHeader"))
  {
    volumeDataLayout->GetMetadataBLOB("SEGY", "TextHeader", textHeader);
  }
  else if(volumeDataLayout->IsMetadataBLOBAvailable("", "SEGYTextHeader")) // This non-standard metadata is written by Bluware software that pre-dates the standardization of VDS
  {
    volumeDataLayout->GetMetadataBLOB("", "SEGYTextHeader", textHeader);

    int a2e[] = {  0,  1,  2,  3, 55, 45, 46, 47, 22,  5, 37, 11, 12, 13, 14, 15,
                  16, 17, 18, 19, 60, 61, 50, 38, 24, 25, 63, 39, 28, 29, 30, 31,
                  64, 79,127,123, 91,108, 80,125, 77, 93, 92, 78,107, 96, 75, 97,
                  240,241,242,243,244,245,246,247,248,249,122, 94, 76,126,110,111,
                  124,193,194,195,196,197,198,199,200,201,209,210,211,212,213,214,
                  215,216,217,226,227,228,229,230,231,232,233, 74,224, 90, 95,109,
                  121,129,130,131,132,133,134,135,136,137,145,146,147,148,149,150,
                  151,152,153,162,163,164,165,166,167,168,169,192,106,208,161,  7,
                   32, 33, 34, 35, 36, 21,  6, 23, 40, 41, 42, 43, 44,  9, 10, 27,
                   48, 49, 26, 51, 52, 53, 54,  8, 56, 57, 58, 59,  4, 20, 62,225,
                   65, 66, 67, 68, 69, 70, 71, 72, 73, 81, 82, 83, 84, 85, 86, 87,
                   88, 89, 98, 99,100,101,102,103,104,105,112,113,114,115,116,117,
                  118,119,120,128,138,139,140,141,142,143,144,154,155,156,157,158,
                  159,160,170,171,172,173,174,175,176,177,178,179,180,181,182,183,
                  184,185,186,187,188,189,190,191,202,203,204,205,206,207,218,219,
                  220,221,222,223,234,235,236,237,238,239,250,251,252,253,254,255 };

    // Convert to EBCDIC
    for(int i = 0; i < int(textHeader.size()); i++) textHeader[i] = a2e[textHeader[i]];
  }

  SEGY::Endianness headerEndianness = SEGY::Endianness::BigEndian;

  if(volumeDataLayout->IsMetadataIntAvailable("SEGY", "Endianness"))
  {
    switch(volumeDataLayout->GetMetadataInt("SEGY", "Endianness"))
    {
    case 0: headerEndianness = SEGY::Endianness::BigEndian;    break;
    case 1: headerEndianness = SEGY::Endianness::LittleEndian; break;
    }
  }

  SEGY::Endianness dataEndianness = headerEndianness;

  if(volumeDataLayout->IsMetadataIntAvailable("SEGY", "DataEndianness"))
  {
    switch(volumeDataLayout->GetMetadataInt("SEGY", "DataEndianness"))
    {
    case 0: dataEndianness = SEGY::Endianness::BigEndian;    break;
    case 1: dataEndianness = SEGY::Endianness::LittleEndian; break;
    }
  }

  if(volumeDataLayout->IsMetadataBLOBAvailable("SEGY", "BinaryHeader"))
  {
    volumeDataLayout->GetMetadataBLOB("SEGY", "BinaryHeader", binaryHeader);
  }
  else if(volumeDataLayout->IsMetadataBLOBAvailable("", "SEGYBinaryHeader")) // This non-standard metadata is written by Bluware software that pre-dates the standardization of VDS
  {
    std::vector<uint8_t> littleEndianBinaryHeader(SEGY::BinaryFileHeaderSize);

    volumeDataLayout->GetMetadataBLOB("", "SEGYBinaryHeader", littleEndianBinaryHeader);

    binaryHeader.resize(littleEndianBinaryHeader.size());

    // Convert to big-endian
    if(headerEndianness == SEGY::Endianness::BigEndian)
    {
      for(int i = 0; i < int(littleEndianBinaryHeader.size()); i++) binaryHeader[i] = littleEndianBinaryHeader[(i < 3 * 4) ? (i ^ 3) : (i ^ 1)];
    }
  }

  if(textHeader.size() != SEGY::TextualFileHeaderSize || binaryHeader.size() != SEGY::BinaryFileHeaderSize)
  {
    outputPrinter.printError("SEGY", "Invalid SEG-Y Text/Binary headers");
    return EXIT_FAILURE;
  }

  SEGY::BinaryHeader::DataSampleFormatCode dataSampleFormatCode;

  if(volumeDataLayout->IsMetadataIntAvailable("SEGY", "DataSampleFormatCode"))
  {
    dataSampleFormatCode = SEGY::BinaryHeader::DataSampleFormatCode(volumeDataLayout->GetMetadataInt("SEGY", "DataSampleFormatCode"));
  }
  else
  {
    dataSampleFormatCode = SEGY::BinaryHeader::DataSampleFormatCode(ReadFieldFromHeader(binaryHeader.data(), SEGY::BinaryHeader::DataSampleFormatCodeHeaderField, headerEndianness));
  }

  if(dataSampleFormatCode == SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat && dataEndianness == SEGY::Endianness::LittleEndian)
  {
    outputPrinter.printError("SEGY", "Little-endian IBM float is not supported");
    return EXIT_FAILURE;
  }

  if(dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::Int32 &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::UInt32 &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::Int16 &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::UInt16 &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::Int8 &&
     dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::UInt8)
  {
    outputPrinter.printError("SEGY", "Unsupported data sample format", fmt::format("{}", int(dataSampleFormatCode)));
    return EXIT_FAILURE;
  }

  OpenVDS::File
    file;

  OpenVDS::Error
    error;

  file.Open(fileName.c_str(), true, true, true, error);

  if(error.code != 0)
  {
    outputPrinter.printError("VDS", "Could not open file", fileName);
    return EXIT_FAILURE;
  }

  file.Write(textHeader.data(), 0, SEGY::TextualFileHeaderSize, error) && file.Write(binaryHeader.data(), SEGY::TextualFileHeaderSize, SEGY::BinaryFileHeaderSize, error);
  if(error.code != 0)
  {
    outputPrinter.printError("VDS", "Error writing SEG-Y headers to file", fileName);
    return EXIT_FAILURE;
  }

  int lineCount = volumeDataLayout->GetDimensionNumSamples(outerDimension);

  // Find which dimension to loop over in case the data has been transposed on import (i.e. crossline-sorted binned data)
  const char *primaryKey = volumeDataLayout->GetMetadataString("SEGY", "PrimaryKey");
  for(int dimension = 1; dimension < dimensionality; dimension++)
  {
    if(strcmp(primaryKey, volumeDataLayout->GetDimensionName(dimension)) == 0)
    {
      outerDimension = dimension;
    }
  }

  // Count the total number of traces for each request
  int64_t traceCount = 1;
  for(int dimension = 1; dimension < dimensionality; dimension++)
  {
    if(dimension != outerDimension)
    {
      traceCount *= volumeDataLayout->GetDimensionNumSamples(dimension);
    }
  }

  const int sampleFormatSize = SEGY::FormatSize(dataSampleFormatCode);
  const int sampleCount = volumeDataLayout->GetDimensionNumSamples(0);
  const int traceDataSize = sampleCount * sampleFormatSize;

  const auto requestDataFormat = SEGY::convertSegyFormat(dataSampleFormatCode, error);
  if (error.code != 0)
  {
    outputPrinter.printError("VDS", "Could not get VDS data format for SEGY data format", openError.string);
    return EXIT_FAILURE;
  }

  std::unique_ptr<char[]> data(new char[traceCount * traceDataSize]);
  std::unique_ptr<char[]> traceFlag(new char[traceCount]);
  std::unique_ptr<char[]> segyTraceHeader(new char[traceCount * SEGY::TraceHeaderSize]);

  int64_t
    offset = SEGY::TextualFileHeaderSize + SEGY::BinaryFileHeaderSize;

  double percentage = -1.0;
  for(int line = 0; line < lineCount; line++)
  {
    double new_percentage = line / double(lineCount) * 100.0;
    if (new_percentage - percentage > 0.333)
    {
      percentage = new_percentage;
      outputPrinter.printPercentage(percentage);
    }
    int min[OpenVDS::Dimensionality_Max] = {},
        max[OpenVDS::Dimensionality_Max] = {};

    for(int dimension = 0; dimension < outerDimension; dimension++)
    {
      max[dimension] = volumeDataLayout->GetDimensionNumSamples(dimension);
    }

    min[outerDimension] = line;
    max[outerDimension] = line + 1;

    auto dataRequest = accessManager.RequestVolumeSubset((void *)data.get(), traceCount * traceDataSize, dimensionGroup, 0, 0, min, max, requestDataFormat);

    max[0] = 1;
    auto traceFlagRequest = accessManager.RequestVolumeSubset((void *)traceFlag.get(), traceCount, dimensionGroup, 0, traceFlagChannel, min, max, OpenVDS::VolumeDataChannelDescriptor::Format_U8);

    max[0] = 240;
    auto segyTraceHaderRequest = accessManager.RequestVolumeSubset((void *)segyTraceHeader.get(), traceCount * SEGY::TraceHeaderSize, dimensionGroup, 0, segyTraceHeaderChannel, min, max, OpenVDS::VolumeDataChannelDescriptor::Format_U8);

    // Need to queue the writing on another thread to get max. performance
    if (!dataRequest->WaitForCompletion())
    {
      dataRequest->IsCanceled(); //get the error
      int errorCode = dataRequest->GetErrorCode();
      std::string errorString = dataRequest->GetErrorMessage();
      outputPrinter.printError("Error in data request", errorString, fmt::format("{}", errorCode));
      assert(dataRequest->IsCanceled());
      exit(1);
    }
    if (!traceFlagRequest->WaitForCompletion())
    {
      dataRequest->IsCanceled(); //get the error
      int errorCode = dataRequest->GetErrorCode();
      std::string errorString = dataRequest->GetErrorMessage();
      outputPrinter.printError("Error in traceFlag request", errorString, fmt::format("{}", errorCode));
      assert(traceFlagRequest->IsCanceled());
      exit(1);
    }
    if (!segyTraceHaderRequest->WaitForCompletion())
    {
      dataRequest->IsCanceled(); //get the error
      int errorCode = dataRequest->GetErrorCode();
      std::string errorString = dataRequest->GetErrorMessage();
      outputPrinter.printError("Error in segyTraceHeader request", errorString, fmt::format("{}", errorCode));
      assert(segyTraceHaderRequest->IsCanceled());
      exit(1);
    }

    std::unique_ptr<unsigned char[]> writeBuffer(new unsigned char[traceCount * (traceDataSize + SEGY::TraceHeaderSize)]);
    int activeTraceCount = 0;

    for(int trace = 0; trace < traceCount; trace++)
    {
      if(traceFlag[trace])
      {
        // Copy trace header
        memcpy(writeBuffer.get() + static_cast<size_t>(activeTraceCount) * (traceDataSize + SEGY::TraceHeaderSize), segyTraceHeader.get() + static_cast<size_t>(trace) * SEGY::TraceHeaderSize, SEGY::TraceHeaderSize);

        // Convert trace data
        switch (dataSampleFormatCode)
        {
        case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
        case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
        case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
        case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
          copySamplesToSEGY<float>(dataEndianness, dataSampleFormatCode, reinterpret_cast<float*>(data.get() + static_cast<size_t>(trace) * traceDataSize), writeBuffer.get() + static_cast<size_t>(activeTraceCount) * (traceDataSize + SEGY::TraceHeaderSize) + SEGY::TraceHeaderSize, 0, sampleCount);
          break;
        case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
        case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
          copySamplesToSEGY<uint16_t>(dataEndianness, dataSampleFormatCode, reinterpret_cast<uint16_t*>(data.get() + static_cast<size_t>(trace) * traceDataSize), writeBuffer.get() + static_cast<size_t>(activeTraceCount) * (traceDataSize + SEGY::TraceHeaderSize) + SEGY::TraceHeaderSize, 0, sampleCount);
          break;
        case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
        case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
          copySamplesToSEGY<uint8_t>(dataEndianness, dataSampleFormatCode, reinterpret_cast<uint8_t*>(data.get() + static_cast<size_t>(trace) * traceDataSize), writeBuffer.get() + static_cast<size_t>(activeTraceCount) * (traceDataSize + SEGY::TraceHeaderSize) + SEGY::TraceHeaderSize, 0, sampleCount);
          break;
        default:
          error.code = -1;
          error.string = fmt::format("Unknown input format {}.", SEGY::DataSampleFormatCodeToString(dataSampleFormatCode));
          continue;
        }

        activeTraceCount++;
      }
    }

    auto totalLength = static_cast<int64_t>(activeTraceCount) * (traceDataSize + SEGY::TraceHeaderSize);
    int64_t bufferOffset = 0;
    while (totalLength > 0)
    {
      constexpr int64_t maxWrite = 128LL * 1024LL * 1024LL;
      auto thisLength = std::min(totalLength, maxWrite);
      file.Write(writeBuffer.get() + bufferOffset, offset, static_cast<int32_t>(thisLength), error);
      if (error.code != 0)
      {
        outputPrinter.printError("Error writing SEG-Y traces to file", fileName);
        return EXIT_FAILURE;
      }
      offset += thisLength;
      totalLength -= thisLength;
      bufferOffset += thisLength;
    }
  }
  outputPrinter.printPercentage(100.0f);
  outputPrinter.printInfo("Done", fmt::format("Successfully exported {} to {}", url, fileName));
  //double elapsed = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start_time).count();
  //fmt::print("Elapsed time is {}.\n", elapsed / 1000);

  return EXIT_SUCCESS;
}
