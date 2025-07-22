/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**   http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
****************************************************************************/

#define _CRT_SECURE_NO_WARNINGS 1
#include "IO/File.h"
#include "VDS/Hash.h"
#include <SEGYUtils/DataProvider.h>
#include <SEGYUtils/SEGYFileInfo.h>
#include <SEGYUtils/TraceDataManager.h>
#include "TraceInfo2DManager.h"

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/MetadataContainer.h>
#include <OpenVDS/VolumeDataLayoutDescriptor.h>
#include <OpenVDS/VolumeDataAxisDescriptor.h>
#include <OpenVDS/VolumeDataChannelDescriptor.h>
#include <OpenVDS/VolumeDataAccess.h>
#include <OpenVDS/Range.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <OpenVDS/KnownMetadata.h>
#include <OpenVDS/GlobalMetadataCommon.h>
#include <OpenVDS/ValueRangeEstimator.h>

#include "IO/IOManager.h"

#include <mutex>
#include <cstdlib>
#include <climits>
#include <cassert>
#include <algorithm>

#define _USE_MATH_DEFINES
#include <math.h>

#include <cxxopts.hpp>
#include <json/json.h>
#include <fmt/format.h>

#include "SplitUrl.h"
#include <PrintHelpers.h>
#include <HelpConnection.h>

#ifdef _WIN32
#include <WindowsUnicodeMain.h>
#endif

#include <chrono>
#include <numeric>
#include <set>
#include <array>

#include "SEGYUtils/VDSSEGYInfo.h"

#if defined(WIN32)
#undef WIN32_LEAN_AND_MEAN // avoid warnings if defined on command line
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#include <io.h>
#include <windows.h>

int64_t GetTotalSystemMemory()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return int64_t(status.ullTotalPhys);
}

#else
#include <unistd.h>

int64_t GetTotalSystemMemory()
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return int64_t(pages) * int64_t(page_size);
}

#include <cfloat>
#include <signal.h>
#endif

constexpr double METERS_PER_FOOT = 0.3048;

namespace OpenVDS
{
  extern Json::Value SerializeVolumeDataLayoutDescriptor(VolumeDataLayout const &volumeDataLayout);
  extern Json::Value SerializeAxisDescriptors(VolumeDataLayout const &volumeDataLayout);
  extern Json::Value SerializeChannelDescriptors(VolumeDataLayout const &volumeDataLayout);
  extern Json::Value SerializeVolumeDataLayoutDescriptor(VolumeDataLayoutDescriptor const& layoutDescriptor);
  extern Json::Value SerializeAxisDescriptor(VolumeDataAxisDescriptor const& axisDescriptor);
  extern Json::Value SerializeChannelDescriptor(VolumeDataChannelDescriptor const& channelDescriptor);
}

static std::string convertToString(const Json::Value &value)
{
  std::stringstream stream;
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "   ";  // or whatever you like
  std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
  writer->write(value, &stream);
  return stream.str();
}


Json::Value SerializeAxisDescriptors(std::vector<OpenVDS::VolumeDataAxisDescriptor> const& axisDescriptors)
{
  Json::Value axisDescriptorsJson(Json::arrayValue);

  for(auto &axisDescriptor : axisDescriptors)
  {
    axisDescriptorsJson.append(OpenVDS::SerializeAxisDescriptor(axisDescriptor));
  }
  return axisDescriptorsJson;
}

Json::Value SerializeChannelDescriptors(std::vector<OpenVDS::VolumeDataChannelDescriptor> const &channelDescriptors)
{
  Json::Value channelDescriptorsJson(Json::arrayValue);

  for(auto &channelDescriptor : channelDescriptors)
  {
    channelDescriptorsJson.append(OpenVDS::SerializeChannelDescriptor(channelDescriptor));
  }
  return channelDescriptorsJson;
}


std::string GetUrlWithPersistentID(const std::string& url, const std::string& persistentID, bool isUrlHttpSupported)
{
  if (persistentID.empty() || !isUrlHttpSupported)
    return url;
  
  std::string baseUrl;
  std::string parameters;
  splitUrlOnParameters(url, baseUrl, parameters);
  if (baseUrl.back() != '/')
  {
    baseUrl.push_back('/');
  }
  baseUrl.insert(baseUrl.end(), persistentID.begin(), persistentID.end());
  return baseUrl;
}
enum class TraceSpacingByOffset
{
  Off = 0,
  On = 1,
  Auto = 2
};

enum class PrimaryKeyValue
{
  Other = 0,
  InlineNumber = 1,
  CrosslineNumber = 2
};

enum class VDSFormatOverride
{
  None, Override16Bit, Override8Bit
};

inline char asciitolower(char in) {
  if (in <= 'Z' && in >= 'A')
    return in - ('Z' - 'z');
  return in;
}

// Splits a string into two parts separated by a colon and discards the colon.
// If no colon is found then the return value is two empty strings.
//
static std::pair<std::string, std::string> SplitColonPair(const std::string& pairString)
{
  const auto colonIndex = pairString.find(':');
  if (colonIndex == std::string::npos)
  {
    return {};
  }

  std::string
    left = pairString.substr(0, colonIndex),
    right = pairString.substr(colonIndex + 1);
  return { left, right };
}

// Converts a string to float. Sets the Error object if problems are encountered.
//
static float ToFloat(const std::string& string, OpenVDS::Error& error)
{
  error = {};
  float value = 0.0f;
  try
  {
    size_t endPos;
    value = std::stof(string, &endPos);
    endPos += 1;
    if (endPos < string.size() && !isspace(string[endPos]))
    {
      throw std::invalid_argument("Non-numeric characters in string");
    }
  }
  catch (const std::invalid_argument& )
  {
    error.code = 1;
    error.string = fmt::format("Could not convert to a numeric value: {}", string);
  }
  catch (const std::out_of_range& )
  {
    error.code = 2;
    error.string = fmt::format("Value is too large to store in a float: {}", string);
  }

  return value;
}

// Converts a string like "45.45:88.88" to a FloatRange. Various validation checks are performed.
// Any conversion errors will be noted in the Error object.
//
static OpenVDS::FloatRange ColonPairToFloatRange(const std::string& pairString, OpenVDS::Error& error)
{
   error = {};
  const auto strings = SplitColonPair(pairString);
  if (strings.first.empty() || strings.second.empty())
  {
    error.code = 1;
    error.string = "Could not find a pair of values. The parameter must be in this form:  value1:value2 (no spaces)";
    return {};
  }

  OpenVDS::FloatRange result;
  result.Min = ToFloat(strings.first, error);
  if (error.code != 0)
  {
    return {};
  }
  result.Max = ToFloat(strings.second, error);
  if (error.code != 0)
  {
    return {};
  }

  return result;
}

struct SurveyCoordinateSystemTransformer
{
  enum MeasurementSystem
  {
    unknown,
    meters,
    feet,
    original,
    empty,
    invalid
  };

  static MeasurementSystem getMeasurementSystem(const std::string& value)
  {
    const char* const allowedValues[] = {
      "unknown",
      "meters",
      "feet",
      "original",
      "empty"
    };
    if (value.empty())
      return empty;
    for (int i = 0; i < int(sizeof(allowedValues) / sizeof(*allowedValues)); i++)
    {
      if (value == allowedValues[i])
        return MeasurementSystem(i);
    }
    return invalid;
  }

  static bool assertSegyUnitName(const std::string& segyUnitName)
  {
    auto unit = getMeasurementSystem(segyUnitName);
    switch (unit)
    {
    case unknown:
    case meters:
    case feet:
    case empty:
      return true;
    default:
      break;
    }
    return false;
  }

  static bool assertVDSUnitName(const std::string& vdsUnitName)
  {
    auto unit = getMeasurementSystem(vdsUnitName);
    switch (unit)
    {
    case meters:
    case feet:
    case original:
    case empty:
      return true;
    default:
      break;
    }
    return false;
  }

  void resolveUnits(const std::string& segyUnitName, SEGY::BinaryHeader::MeasurementSystem foundMeasurementSystem, const std::string& vdsUnitName)
  {
    assert(assertSegyUnitName(segyUnitName));
    assert(assertVDSUnitName(vdsUnitName));
    auto segyUnit = getMeasurementSystem(segyUnitName);
    auto vdsUnit = getMeasurementSystem(vdsUnitName);
    if (segyUnit == empty)
      segyMeasurementSystem = foundMeasurementSystem;
    else
      segyMeasurementSystem = (SEGY::BinaryHeader::MeasurementSystem)segyUnit;

    if (vdsUnit == empty)
      vdsMeasurementSystem = SEGY::BinaryHeader::MeasurementSystem::Meters;
    else if (vdsUnit == original)
      vdsMeasurementSystem = segyMeasurementSystem;
    else
      vdsMeasurementSystem = (SEGY::BinaryHeader::MeasurementSystem)vdsUnit;
  }

  template<typename T>
  T convertToVdsDistance(T distance) const
  {
    if (segyMeasurementSystem == vdsMeasurementSystem)
      return distance;
    if (segyMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Unknown)
      return distance;
    if (segyMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Meters)
      return distance / T(METERS_PER_FOOT);

    assert(segyMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Feet);
    assert(vdsMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Meters);
    return distance * T(METERS_PER_FOOT);
  }

  SEGY::BinaryHeader::MeasurementSystem
    segyMeasurementSystem = SEGY::BinaryHeader::MeasurementSystem::Unknown;
  SEGY::BinaryHeader::MeasurementSystem
    vdsMeasurementSystem = SEGY::BinaryHeader::MeasurementSystem::Unknown;
};

static DataProvider CreateDataProviderFromFile(const std::string &filename, OpenVDS::Error &error)
{
  std::unique_ptr<OpenVDS::File> file(new OpenVDS::File());
  if (!file->Open(filename, false, false, false, error))
    return DataProvider((OpenVDS::File *)nullptr);
  return DataProvider(file.release());
}

static DataProvider CreateDataProviderFromOpenOptions(const std::string &url, const std::string &connectionString, OpenVDS::Error &error)
{
  std::unique_ptr<OpenVDS::IOManager> ioManager(OpenVDS::IOManager::CreateIOManager(url, connectionString, OpenVDS::IOManager::AccessPattern::ReadOnly, error));
  if (error.code)
    return DataProvider("", (OpenVDS::IOManager*)nullptr, error);
  return DataProvider(url, ioManager.release(), error);
}

DataProvider CreateDataProvider(const std::string& url, const std::string& connection, OpenVDS::Error& error)
{
  if (OpenVDS::IsSupportedProtocol(url))
  {
    return CreateDataProviderFromOpenOptions(url, connection, error);
  }
  else
  {
    return CreateDataProviderFromFile(url, error);
  }
  return DataProvider(nullptr);
}

static std::vector<DataProvider> CreateDataProviders(const std::vector<std::string> &fileNames, const std::string &connection, OpenVDS::Error &error, std::string &errorFileName)
{
  std::vector<DataProvider>
    dataProviders;

  for (const auto& fileName : fileNames)
  {
    dataProviders.push_back(CreateDataProvider(fileName, connection, error));

    if (error.code != 0)
    {
      errorFileName = fileName;
      dataProviders.clear();
      break;
    }
  }
  return dataProviders;
}

Json::Value
SerializeSEGYBinInfo(SEGYBinInfo const& binInfo)
{
  Json::Value
    jsonBinInfo;

  jsonBinInfo["inlineNumber"] = binInfo.m_inlineNumber;
  jsonBinInfo["crosslineNumber"] = binInfo.m_crosslineNumber;

  jsonBinInfo["ensembleXCoordinate"] = binInfo.m_ensembleXCoordinate;
  jsonBinInfo["ensembleYCoordinate"] = binInfo.m_ensembleYCoordinate;

  return jsonBinInfo;
}

Json::Value
SerializeSEGYSegmentInfo(SEGYSegmentInfo const& segmentInfo)
{
  Json::Value
    jsonSegmentInfo;

  jsonSegmentInfo["primaryKey"] = segmentInfo.m_primaryKey;
  jsonSegmentInfo["traceStart"] = segmentInfo.m_traceStart;
  jsonSegmentInfo["traceStop"] = segmentInfo.m_traceStop;

  jsonSegmentInfo["binInfoStart"] = SerializeSEGYBinInfo(segmentInfo.m_binInfoStart);
  jsonSegmentInfo["binInfoStop"] = SerializeSEGYBinInfo(segmentInfo.m_binInfoStop);

  return jsonSegmentInfo;
}

std::string
ToString(SEGY::Endianness endiannness)
{
  switch (endiannness)
  {
  case SEGY::Endianness::BigEndian:  return "BigEndian";
  case SEGY::Endianness::LittleEndian: return "LittleEndian";
  default:
    assert(0); return "";
  }
}

std::string
ToString(SEGY::FieldWidth fieldWidth)
{
  switch (fieldWidth)
  {
  case SEGY::FieldWidth::TwoByte:  return "TwoByte";
  case SEGY::FieldWidth::FourByte: return "FourByte";
  default:
    assert(0); return "";
  }
}

Json::Value
SerializeSEGYHeaderField(SEGY::HeaderField const& headerField)
{
  Json::Value
    jsonHeaderField(Json::ValueType::arrayValue);

  jsonHeaderField.append(headerField.byteLocation);
  jsonHeaderField.append(ToString(headerField.fieldWidth));

  return jsonHeaderField;
}

Json::Value
SerializeSEGYFileInfo(SEGYFileInfo const& fileInfo, const size_t fileIndex)
{
  Json::Value
    jsonFileInfo;

  jsonFileInfo["persistentID"] = fmt::format("{:X}", fileInfo.m_persistentID);
  jsonFileInfo["headerEndianness"] = ToString(fileInfo.m_headerEndianness);
  jsonFileInfo["dataSampleFormatCode"] = (int)fileInfo.m_dataSampleFormatCode;
  jsonFileInfo["sampleCount"] = fileInfo.m_sampleCount;
  jsonFileInfo["startTime"] = fileInfo.m_startTimeMilliseconds;
  jsonFileInfo["sampleInterval"] = fileInfo.m_sampleIntervalMilliseconds;
  jsonFileInfo["traceCount"] = fileInfo.m_traceCounts[fileIndex];
  jsonFileInfo["primaryKey"] = SerializeSEGYHeaderField(fileInfo.m_primaryKey);
  jsonFileInfo["secondaryKey"] = SerializeSEGYHeaderField(fileInfo.m_secondaryKey);

  if (fileInfo.m_segyType == SEGY::SEGYType::PrestackOffsetSorted)
  {
    Json::Value
      jsonOffsetMap;

    for (const auto& entry : fileInfo.m_segmentInfoListsByOffset[fileIndex])
    {
      Json::Value
        jsonSegmentInfoArray(Json::ValueType::arrayValue);

      for (auto const& segmentInfo : entry.second)
      {
        jsonSegmentInfoArray.append(SerializeSEGYSegmentInfo(segmentInfo));
      }

      jsonOffsetMap[std::to_string(entry.first)] = jsonSegmentInfoArray;
    }

    jsonFileInfo["segmentInfo"] = jsonOffsetMap;
  }
  else
  {
    Json::Value
      jsonSegmentInfoArray(Json::ValueType::arrayValue);

    for (auto const& segmentInfo : fileInfo.m_segmentInfoLists[fileIndex])
    {
      jsonSegmentInfoArray.append(SerializeSEGYSegmentInfo(segmentInfo));
    }

    jsonFileInfo["segmentInfo"] = jsonSegmentInfoArray;
  }

  return jsonFileInfo;
}

std::map<std::string, SEGY::HeaderField>
g_traceHeaderFields =
{
 { "tracesequencenumber",           SEGY::TraceHeader::TraceSequenceNumberHeaderField },
 { "tracesequencenumberwithinfile", SEGY::TraceHeader::TraceSequenceNumberWithinFileHeaderField },
 { "energysourcepointnumber",       SEGY::TraceHeader::EnergySourcePointNumberHeaderField },
 { "ensemblenumber",                SEGY::TraceHeader::EnsembleNumberHeaderField },
 { "tracenumberwithinensemble",     SEGY::TraceHeader::TraceNumberWithinEnsembleHeaderField },
 { "traceidentificationcode",       SEGY::TraceHeader::TraceIdentificationCodeHeaderField },
 { "coordinatescale",               SEGY::TraceHeader::CoordinateScaleHeaderField },
 { "sourcexcoordinate",             SEGY::TraceHeader::SourceXCoordinateHeaderField },
 { "sourceycoordinate",             SEGY::TraceHeader::SourceYCoordinateHeaderField },
 { "groupxcoordinate",              SEGY::TraceHeader::GroupXCoordinateHeaderField },
 { "groupycoordinate",              SEGY::TraceHeader::GroupYCoordinateHeaderField },
 { "coordinateunits",               SEGY::TraceHeader::CoordinateUnitsHeaderField },
 { "starttime",                     SEGY::TraceHeader::StartTimeHeaderField },
 { "numsamples",                    SEGY::TraceHeader::NumSamplesHeaderField },
 { "sampleinterval",                SEGY::TraceHeader::SampleIntervalHeaderField },
 { "ensemblexcoordinate",           SEGY::TraceHeader::EnsembleXCoordinateHeaderField },
 { "ensembleycoordinate",           SEGY::TraceHeader::EnsembleYCoordinateHeaderField },
 { "inlinenumber",                  SEGY::TraceHeader::InlineNumberHeaderField },
 { "crosslinenumber",               SEGY::TraceHeader::CrosslineNumberHeaderField },
 { "receiver",                      SEGY::TraceHeader::ReceiverHeaderField },
 { "offset",                        SEGY::TraceHeader::OffsetHeaderField },
 { "offsetx",                       SEGY::TraceHeader::OffsetXHeaderField },
 { "offsety",                       SEGY::TraceHeader::OffsetYHeaderField },
 { "azimuth",                       SEGY::TraceHeader::Azimuth },
 { "mutestarttime",                 SEGY::TraceHeader::MuteStartTime },
 { "muteendtime",                   SEGY::TraceHeader::MuteEndTime },
};

std::map<std::string, std::string>
g_aliases =
{
 { "inline",              "inlinenumber" },
 { "crossline",           "crosslinenumber" },
 { "shot",                "energysourcepointnumber" },
 { "sp",                  "energysourcepointnumber" },
 { "cdp",                 "ensemblenumber" },
 { "cmp",                 "ensemblenumber" },
 { "easting",             "ensemblexcoordinate" },
 { "northing",            "ensembleycoordinate" },
 { "cdpxcoordinate",      "ensemblexcoordinate" },
 { "cdpycoordinate",      "ensembleycoordinate" },
 { "cdp-x",               "ensemblexcoordinate" },
 { "cdp-y",               "ensembleycoordinate" },
 { "source-x",            "sourcexcoordinate" },
 { "source-y",            "sourceycoordinate" },
 { "group-x",             "groupxcoordinate" },
 { "group-y",             "groupycoordinate" },
 { "receiverxcoordinate", "groupxcoordinate" },
 { "receiverycoordinate", "groupycoordinate" },
 { "receiver-x",          "groupxcoordinate" },
 { "receiver-y",          "groupycoordinate" },
 { "scalar",              "coordinatescale" }
};

void
ResolveAlias(std::string& fieldName)
{

  std::transform(fieldName.begin(), fieldName.end(), fieldName.begin(), asciitolower);
  if (g_aliases.find(fieldName) != g_aliases.end())
  {
    fieldName = g_aliases[fieldName];
  }
}

SEGY::Endianness
EndiannessFromJson(Json::Value const& jsonEndianness)
{
  std::string
    endiannessString = jsonEndianness.asString();

  std::transform(endiannessString.begin(), endiannessString.end(), endiannessString.begin(), asciitolower);
  if (endiannessString == "bigendian")
  {
    return SEGY::Endianness::BigEndian;
  }
  else if (endiannessString == "littleendian")
  {
    return SEGY::Endianness::LittleEndian;
  }

  throw Json::Exception("Illegal endianness");
}

SEGY::FieldWidth
FieldWidthFromJson(Json::Value const& jsonFieldWidth)
{
  std::string
    fieldWidthString = jsonFieldWidth.asString();
  std::transform(fieldWidthString.begin(), fieldWidthString.end(), fieldWidthString.begin(), asciitolower);

  if (fieldWidthString == "twobyte")
  {
    return SEGY::FieldWidth::TwoByte;
  }
  else if (fieldWidthString == "fourbyte")
  {
    return SEGY::FieldWidth::FourByte;
  }

  throw Json::Exception("Illegal field width");
}

SEGY::HeaderField
HeaderFieldFromJson(Json::Value const& jsonHeaderField)
{
  int
    byteLocation = jsonHeaderField[0].asInt();

  SEGY::FieldWidth
    fieldWidth = FieldWidthFromJson(jsonHeaderField[1]);

  if (byteLocation < 1 || byteLocation > SEGY::TraceHeaderSize + 1 - ((fieldWidth == SEGY::FieldWidth::TwoByte) ? 2 : 4))
  {
    throw Json::Exception(std::string("Illegal field definition: ") + jsonHeaderField.toStyledString());
  }

  return SEGY::HeaderField(byteLocation, fieldWidth);
}

bool
ParseHeaderFormatFile(DataProvider &dataProvider, std::map<std::string, SEGY::HeaderField>& traceHeaderFields, SEGY::Endianness& headerEndianness, OpenVDS::Error &error)
{
  int64_t dataSize = dataProvider.Size(error);

  if (error.code != 0)
  {
    return false;
  }

  if (dataSize > INT_MAX)
  {
    return false;
  }

  std::unique_ptr<char[]>
    buffer(new char[dataSize]);

  dataProvider.Read(buffer.get(), 0, (int32_t)dataSize, error);

  if (error.code != 0)
  {
    return false;
  }

  try
  {
    Json::CharReaderBuilder
      rbuilder;

    rbuilder["collectComments"] = false;

    std::string
      errs;

    std::unique_ptr<Json::CharReader>
      reader(rbuilder.newCharReader());

    Json::Value
      root;

    bool
      success = reader->parse(buffer.get(), buffer.get() + dataSize, &root, &errs);

    if (!success)
    {
      throw Json::Exception(errs);
    }

    for (std::string const& fieldName : root.getMemberNames())
    {
      std::string canonicalFieldName = fieldName;
      ResolveAlias(canonicalFieldName);

      if (canonicalFieldName == "endianness")
      {
        headerEndianness = EndiannessFromJson(root[fieldName]);
      }
      else
      {
        traceHeaderFields[canonicalFieldName] = HeaderFieldFromJson(root[fieldName]);
      }
    }
  }
  catch (Json::Exception &e)
  {
    error.code = -1;
    error.string = e.what();
    return false;
  }

  return true;
}

bool OnlyDigits(const std::string& str)
{
  for (auto a : str)
  {
    if (a < '0' || a > '9')
      return false;
  }
  return true;
}

bool
ParseHeaderFieldArgs(const std::vector<std::string> &header_fields_args, std::map<std::string, SEGY::HeaderField>& traceHeaderFields, SEGY::Endianness& headerEndianness, OpenVDS::Error& error)
{
  for (auto& header_field : header_fields_args)
  {
    if (header_field.empty())
    {
      error.code = -1;
      error.string = "Cannot parse empty header-field";
      return false;
    }
    auto it = std::find(header_field.begin(), header_field.end(), '=');
    if (it == header_field.end())
    {
      error.code = -1;
      error.string = fmt::format("Failed to parse header-field {}.", header_field);
      return false;
    }
    std::string headerName(header_field.begin(), it);
    if (it + 1 == header_field.end())
    {
      error.code = -1;
      error.string = fmt::format("Can not find value for header-field {}.", headerName);
      return false;
    }
    std::string header_value(it + 1, header_field.end());
    auto min_delimiter = std::find(header_value.begin(), header_value.end(), '-');
    int fieldWidth = -1;
    int byteLocation = -1;
    if (min_delimiter != header_value.end())
    {
      if (min_delimiter + 1 == header_value.end())
      {
        error.code = -1;
        error.string = fmt::format("unable to parse value for header-field {} with value {}.", headerName, header_value);
        return false;
      }
      std::string value_start(header_value.begin(), min_delimiter);
      std::string value_end(min_delimiter + 1, header_value.end());
      if (!OnlyDigits(value_start) || !OnlyDigits(value_end))
      {
        error.code = -1;
        error.string = fmt::format("unable to parse header-field {} value range {}.", headerName, header_value);
        return false;
      }
      int value_start_value = atoi(value_start.c_str());
      int value_end_value = atoi(value_end.c_str());
      if(value_end_value < value_start_value)
      {
        error.code = -1;
        error.string = fmt::format("header-field {} has illegal value range {}.", headerName, header_value);
        return false;
      }
      byteLocation = value_start_value;
      fieldWidth = value_end_value - value_start_value + 1;
    }
    else
    {
      auto colon_delimiter = std::find(header_value.begin(), header_value.end(), ':');
      std::string offset_str(header_value.begin(), colon_delimiter);
      if (!OnlyDigits(offset_str))
      {
        error.code = -1;
        error.string = fmt::format("unable to parse byte location for header-field {}: {}.", headerName, header_value);
        return false;
      }
      byteLocation = atoi(offset_str.c_str());
      if (colon_delimiter < header_value.end() && colon_delimiter + 1 < header_value.end())
      {
        std::string width_str(colon_delimiter + 1, header_value.end());
        if (!OnlyDigits(width_str))
        {
          error.code = -1;
          error.string = fmt::format("unable to parse width specifier for header-field {}: {}.", headerName, width_str);
          return false;
        }
        fieldWidth = atoi(width_str.c_str());
      }
    }
    if (byteLocation < 0)
    {
      error.code = -1;
      error.string = fmt::format("unable to find byte location for header-field {}: {}.", headerName, header_value);
      return false;
    }

    auto originalHeaderName = headerName;
    ResolveAlias(headerName);
    auto& traceHeaderField = traceHeaderFields[headerName];

    if(fieldWidth == -1)
    {
      if (!traceHeaderField.Defined())
      {
        error.code = -1;
        error.string = fmt::format("header-field {} has no pre-defined field width, field width must be specified.", originalHeaderName);
        return false;
      }

      fieldWidth = (traceHeaderField.fieldWidth == SEGY::FieldWidth::TwoByte) ? 2 : 4;
    }
    else if (fieldWidth != 2 && fieldWidth != 4)
    {
      error.code = -1;
      error.string = fmt::format("header-field {} has illegal field width of {}. Only widths of 2 or 4 are accepted.", originalHeaderName, fieldWidth);
      return false;
    }

    if (byteLocation < 1 || byteLocation > SEGY::TraceHeaderSize + 1 - fieldWidth)
    {
      error.code = -1;
      error.string = fmt::format("header-field {} with byte location {} and field width of {} is outside of the trace header.", originalHeaderName, byteLocation, fieldWidth);
      return false;
    }

    traceHeaderField.byteLocation = byteLocation;
    traceHeaderField.fieldWidth = (fieldWidth == 2) ? SEGY::FieldWidth::TwoByte : SEGY::FieldWidth::FourByte;
  }
  return true;
}

SEGYBinInfo
binInfoFromJson(Json::Value const& jsonBinInfo)
{
  int inlineNumber = jsonBinInfo["inlineNumber"].asInt();
  int crosslineNumber = jsonBinInfo["crosslineNumber"].asInt();
  double ensembleXCoordinate = jsonBinInfo["ensembleXCoordinate"].asDouble();
  double ensembleYCoordinate = jsonBinInfo["ensembleYCoordinate"].asDouble();

  return SEGYBinInfo(inlineNumber, crosslineNumber, ensembleXCoordinate, ensembleYCoordinate);
}

SEGYSegmentInfo
segmentInfoFromJson(Json::Value const& jsonSegmentInfo)
{
  int primaryKey = jsonSegmentInfo["primaryKey"].asInt();
  int traceStart = jsonSegmentInfo["traceStart"].asInt();
  int traceStop = jsonSegmentInfo["traceStop"].asInt();
  SEGYBinInfo binInfoStart = binInfoFromJson(jsonSegmentInfo["binInfoStart"]);
  SEGYBinInfo binInfoStop = binInfoFromJson(jsonSegmentInfo["binInfoStop"]);

  return SEGYSegmentInfo(primaryKey, traceStart, traceStop, binInfoStart, binInfoStop);
}

std::vector<int>
getOrderedSegmentListIndices(SEGYFileInfo const& fileInfo, size_t& globalTotalSegments)
{
  // Generate a list of indices that will traverse m_segyFileInfo.m_segmentInfoLists in primary key order, which
  // may be different from the order that the files were given.

  std::vector<int>
    orderedListIndices;
  globalTotalSegments = 0;

  if (fileInfo.IsOffsetSorted())
  {
    for (int i = 0; i < static_cast<int>(fileInfo.m_segmentInfoListsByOffset.size()); ++i)
    {
      orderedListIndices.push_back(i);
      for (const auto& entry : fileInfo.m_segmentInfoListsByOffset[i])
      {
        globalTotalSegments += entry.second.size();
      }
    }

    // don't need to check for ascending/descending primary key because the map will have offsets (keys) sorted in ascending order

    auto
      comparator = [&](int i1, int i2)
    {
      const auto
        & v1 = fileInfo.m_segmentInfoListsByOffset[i1],
        & v2 = fileInfo.m_segmentInfoListsByOffset[i2];
      return (*v1.begin()).first < (*v2.begin()).first;
    };
    std::sort(orderedListIndices.begin(), orderedListIndices.end(), comparator);
  }
  else
  {
    size_t
      longestList = 0;
    for (size_t i = 0; i < fileInfo.m_segmentInfoLists.size(); ++i)
    {
      orderedListIndices.push_back(static_cast<int>(i));
      globalTotalSegments += fileInfo.m_segmentInfoLists[i].size();
      if (fileInfo.m_segmentInfoLists[i].size() > fileInfo.m_segmentInfoLists[longestList].size())
      {
        longestList = i;
      }
    }
    const bool
      isAscending = fileInfo.m_segmentInfoLists[longestList].front().m_binInfoStart.m_inlineNumber <= fileInfo.m_segmentInfoLists[longestList].back().m_binInfoStart.m_inlineNumber;
    auto
      comparator = [&](int i1, int i2)
    {
      const auto
        & v1 = fileInfo.m_segmentInfoLists[i1],
        & v2 = fileInfo.m_segmentInfoLists[i2];
      return isAscending ? v1.front().m_binInfoStart.m_inlineNumber < v2.front().m_binInfoStart.m_inlineNumber : v2.front().m_binInfoStart.m_inlineNumber < v1.front().m_binInfoStart.m_inlineNumber;
    };
    std::sort(orderedListIndices.begin(), orderedListIndices.end(), comparator);
  }

  return orderedListIndices;
}

SEGYSegmentInfo const&
findRepresentativeSegment(SEGYFileInfo const& fileInfo, int& primaryStep, int& bestListIndex)
{
  assert(!fileInfo.IsOffsetSorted());

  // Since we give more weight to segments near the center of the data we need a sorted index of segment lists so that we can
  // traverse the lists in data order, instead of the arbitrary order given by the filename ordering.
  size_t
    globalTotalSegments = 0;
  auto
    orderedListIndices = getOrderedSegmentListIndices(fileInfo, globalTotalSegments);

  primaryStep = 0;
  bestListIndex = 0;

  float bestScore = 0.0f;
  size_t bestIndex = 0;

  int segmentPrimaryStep = 0;

  size_t
    globalOffset = 0;

  for (const auto listIndex : orderedListIndices)
  {
    const auto&
      segmentInfoList = fileInfo.m_segmentInfoLists[listIndex];

    for (size_t i = 0; i < segmentInfoList.size(); i++)
    {
      int64_t
        numTraces = (segmentInfoList[i].m_traceStop - segmentInfoList[i].m_traceStart + 1);

      // index of this segment within the entirety of segments from all input files
      const auto
        globalIndex = globalOffset + i;

      float
        multiplier = 1.5f - abs(globalIndex - (float)globalTotalSegments / 2) / (float)globalTotalSegments; // give 50% more importance to a segment in the middle of the dataset

      float
        score = float(numTraces) * multiplier;

      if (score > bestScore)
      {
        bestScore = score;
        bestListIndex = listIndex;
        bestIndex = i;
      }

      // Updating the primary step with the step for the previous segment intentionally ignores the step of the last segment since it can be anomalous
      if (segmentPrimaryStep && (!primaryStep || std::abs(segmentPrimaryStep) < std::abs(primaryStep)))
      {
        primaryStep = segmentPrimaryStep;
      }

      if (i > 0)
      {
        segmentPrimaryStep = segmentInfoList[i].m_primaryKey - segmentInfoList[i - 1].m_primaryKey;
      }
    }

    globalOffset += segmentInfoList.size();
  }

  // If the primary step couldn't be determined, set it to the last step or 1
  primaryStep = primaryStep ? primaryStep : std::max(segmentPrimaryStep, 1);

  return fileInfo.m_segmentInfoLists[bestListIndex][bestIndex];
}

// Analog of findRepresentativeSegment for offset-sorted prestack
int
findRepresentativePrimaryKey(SEGYFileInfo const& fileInfo, int& primaryStep)
{
  // If primary keys are reverse-sorted in the file the structures we build below will hide that.
  // For the computations we're doing in this method it's OK that we're not preserving the original order.

  // scan segment infos to get primary key values and total trace count per primary key
  std::map<int, int64_t>
    primaryKeyLengths;
  for (const auto& offsetMap : fileInfo.m_segmentInfoListsByOffset)
  {
    for (const auto& entry : offsetMap)
    {
      for (const auto& segmentInfo : entry.second)
      {
        const auto
          segmentLength = segmentInfo.m_traceStop - segmentInfo.m_traceStart + 1;
        const auto
          newSum = segmentLength + primaryKeyLengths[segmentInfo.m_primaryKey];
        primaryKeyLengths[segmentInfo.m_primaryKey] = newSum;
      }
    }
  }

  // determine primary step
  primaryStep = 0;
  int
    segmentPrimaryStep = 0,
    previousPrimaryKey = 0,
    firstPrimaryKey = 0,
    lastPrimaryKey = 0;
  bool
    firstSegment = true;
  for (const auto& entry : primaryKeyLengths)
  {
    if (firstSegment)
    {
      firstSegment = false;
      previousPrimaryKey = entry.first;
      firstPrimaryKey = entry.first;
      lastPrimaryKey = entry.first;
    }
    else
    {
      lastPrimaryKey = entry.first;

      // Updating the primary step with the step for the previous segment intentionally ignores the step of the last segment since it can be anomalous
      if (segmentPrimaryStep && (!primaryStep || std::abs(segmentPrimaryStep) < std::abs(primaryStep)))
      {
        primaryStep = segmentPrimaryStep;
      }

      segmentPrimaryStep = entry.first - previousPrimaryKey;
      previousPrimaryKey = entry.first;
    }
  }

  // If the primary step couldn't be determined, set it to the last step or 1
  primaryStep = primaryStep ? primaryStep : std::max(segmentPrimaryStep, 1);

  float
    bestScore = 0.0f;
  int
    bestPrimaryKey = 0;

  for (const auto& entry : primaryKeyLengths)
  {
    const auto&
      numTraces = entry.second;

    // index of this segment within the entirety of segments from all input files
    const auto
      factor = abs(static_cast<float>(entry.first - firstPrimaryKey) / static_cast<float>(lastPrimaryKey - firstPrimaryKey));

    const auto
      multiplier = 1.5f - abs(factor - 0.5f); // give 50% more importance to a segment in the middle of the dataset

    const auto
      score = static_cast<float>(numTraces) * multiplier;

    if (score > bestScore)
    {
      bestScore = score;
      bestPrimaryKey = entry.first;
    }
  }

  return bestPrimaryKey;
}

template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamples(float* prTarget, const unsigned char* puSource, int iSampleMin, int iSampleMax)
{
  if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat)
  {
    SEGY::Ibm2ieee(prTarget, puSource + iSampleMin * 4, iSampleMax - iSampleMin);
    return;
  }

  int
    nValue;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    if (ENDIANNESS == SEGY::Endianness::BigEndian)
    {
      nValue = (int)(puSource[iSample * 4 + 0] << 24 | puSource[iSample * 4 + 1] << 16 | puSource[iSample * 4 + 2] << 8 | puSource[iSample * 4 + 3]);
    }
    else
    {
      nValue = (int)(puSource[iSample * 4 + 3] << 24 | puSource[iSample * 4 + 2] << 16 | puSource[iSample * 4 + 1] << 8 | puSource[iSample * 4 + 0]);
    }

    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat)
    {
      *prTarget++ = SEGY::IntToFloat(nValue);
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int32)
    {
      *prTarget++ = (float)nValue; // Intentionally lose precision
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt32)
    {
      *prTarget++ = (float)(unsigned int)nValue; // Intentionally lose precision
    }
    else
    {
      assert(0 && "Bad data sample format");
    }

  }
}

template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamples(uint8_t* puTarget, const unsigned char* puSource, int iSampleMin, int iSampleMax)
{
  int8_t
    nValue;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    nValue = (int8_t)(puSource[iSample * 1 + 0]);

    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int8)
    {
      *puTarget++ = nValue + SEGY::GetVDSIntegerOffsetForDataSampleFormat<FORMAT>();
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt8)
    {
      *puTarget++ = (uint8_t)nValue;
    }
    else
    {
      assert(0 && "Bad data sample format");
    }
  }
}

template <SEGY::Endianness ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode FORMAT>
void
copySamples(uint16_t* puTarget, const unsigned char* puSource, int iSampleMin, int iSampleMax)
{
  int16_t
    nValue;

  for (int iSample = iSampleMin; iSample < iSampleMax; iSample++)
  {
    if (ENDIANNESS == SEGY::Endianness::BigEndian)
    {
      nValue = (int16_t)(puSource[iSample * 2 + 0] << 8 | puSource[iSample * 2 + 1]);
    }
    else
    {
      nValue = (int16_t)(puSource[iSample * 2 + 1] << 8 | puSource[iSample * 2 + 0]);
    }

    if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::Int16)
    {
      *puTarget++ = nValue + SEGY::GetVDSIntegerOffsetForDataSampleFormat<FORMAT>();
    }
    else if (FORMAT == SEGY::BinaryHeader::DataSampleFormatCode::UInt16)
    {
      *puTarget++ = (uint16_t)nValue;
    }
    else
    {
      assert(0 && "Bad data sample format");
    }
  }
}

template<typename T, SEGY::Endianness ENDIANNESS>
void
copySamples(SEGY::BinaryHeader::DataSampleFormatCode dataSampleFormatCode, T* target, const unsigned char* puSource, int iSampleMin, int iSampleMax)
{
  switch (dataSampleFormatCode)
  {
  case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt32>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int32>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt16>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int16>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::UInt8>(target, puSource, iSampleMin, iSampleMax);
  case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
    return copySamples<ENDIANNESS, SEGY::BinaryHeader::DataSampleFormatCode::Int8>(target, puSource, iSampleMin, iSampleMax);
  default:
    assert(false && "Unsupported data sample format");
  }
}

template<typename T>
void
copySamples(SEGY::Endianness endianess, SEGY::BinaryHeader::DataSampleFormatCode dataSampleFormatCode, T* target, const unsigned char* puSource, int iSampleMin, int iSampleMax)
{
  switch (endianess)
  {
  case SEGY::Endianness::BigEndian:
    return copySamples<T, SEGY::Endianness::BigEndian>(dataSampleFormatCode, target, puSource, iSampleMin, iSampleMax);
  case SEGY::Endianness::LittleEndian:
    return copySamples<T, SEGY::Endianness::LittleEndian>(dataSampleFormatCode, target, puSource, iSampleMin, iSampleMax);
  }
}


template<typename T>
bool
analyzeSegment(DataProvider &dataProvider, SEGYFileInfo const& fileInfo, SEGYSegmentInfo const& segmentInfo, float valueRangePercentile, OpenVDS::FloatRange& valueRange, int& fold, int& secondaryStep, const SEGY::SEGYType segyType, TraceInfo2DManager * pTraceInfo2DManager, TraceSpacingByOffset& traceSpacingByOffset, std::vector<int>& offsetValues, OpenVDS::OutputPrinter &outputPrinter, OpenVDS::Error& error)
{
  assert(segmentInfo.m_traceStop >= segmentInfo.m_traceStart && "A valid segment info should always have a stop trace greater or equal to the start trace");
  assert(pTraceInfo2DManager != nullptr);

  pTraceInfo2DManager->Clear();

  bool success = true;

  valueRange = OpenVDS::FloatRange(0.0f, 1.0f);
  secondaryStep = 0;
  fold = 1;
  offsetValues.clear();

  const int traceByteSize = fileInfo.TraceByteSize();

  int64_t traceBufferStart = 0;
  int traceBufferSize = 0;
  std::unique_ptr<char[]> buffer;

  OpenVDS::HeapBasedValueRangeEstimator<T> valueRangeEstimator(valueRangePercentile, (segmentInfo.m_traceStop - segmentInfo.m_traceStart + 1) * fileInfo.m_sampleCount);

  // Allocate sample buffer for converting samples to float
  std::unique_ptr<T[]> sampleBuffer(new T[fileInfo.m_sampleCount]);

  // Determine fold and secondary step
  int gatherSecondaryKey = 0, gatherFold = 0, gatherSecondaryStep = 0;

  std::map<int, std::vector<int>> gatherOffsetValues;

  for (int64_t trace = segmentInfo.m_traceStart; trace <= segmentInfo.m_traceStop; trace++)
  {
    if(trace - traceBufferStart >= traceBufferSize)
    {
      traceBufferStart = trace;
      traceBufferSize = (segmentInfo.m_traceStop - trace + 1) < 1000 ? int(segmentInfo.m_traceStop - trace + 1) : 1000;

      buffer.reset(new char[static_cast<size_t>(traceByteSize) * traceBufferSize]);
      int64_t offset = SEGY::TextualFileHeaderSize + SEGY::BinaryFileHeaderSize + traceByteSize * traceBufferStart;
      success = dataProvider.Read(buffer.get(), offset, traceByteSize * traceBufferSize, error);

      if (!success)
      {
        break;
      }
    }

    const void *header = buffer.get() + traceByteSize * (trace - traceBufferStart);
    const void *data   = buffer.get() + traceByteSize * (trace - traceBufferStart) + SEGY::TraceHeaderSize;

    int tracePrimaryKey = fileInfo.Is2D() ? 0 : SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness);
    int traceSecondaryKey = fileInfo.IsUnbinned() ? static_cast<int>(trace - segmentInfo.m_traceStart + 1) : SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);

    if(tracePrimaryKey != segmentInfo.m_primaryKey)
    {
      outputPrinter.printWarning("SEGY", fmt::format("trace {} has a primary key that doesn't match with the segment. This trace will be ignored.", segmentInfo.m_traceStart + trace));
      continue;
    }

    pTraceInfo2DManager->AddTraceInfo(static_cast<const char *>(header), trace);

    if(gatherFold > 0 && traceSecondaryKey == gatherSecondaryKey)
    {
      gatherFold++;
      fold = std::max(fold, gatherFold);
    }
    else
    {
      // Updating the secondary step with the step for the previous gather intentionally ignores the step of the last gather since it can be anomalous
      if(gatherSecondaryStep && (!secondaryStep || std::abs(gatherSecondaryStep) < std::abs(secondaryStep)))
      {
        secondaryStep = gatherSecondaryStep;
      }

      if(gatherFold > 0)
      {
        gatherSecondaryStep = traceSecondaryKey - gatherSecondaryKey;
      }
      gatherSecondaryKey = traceSecondaryKey;
      gatherFold = 1;
    }

    if (fileInfo.HasGatherOffset())
    {
      const auto
        thisOffset = SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offset"], fileInfo.m_headerEndianness);
      gatherOffsetValues[gatherSecondaryKey].push_back(thisOffset);
    }

    copySamples<T>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, sampleBuffer.get(), (const unsigned char*)data, 0, fileInfo.m_sampleCount);
    valueRangeEstimator.UpdateValueRange(sampleBuffer.get(), sampleBuffer.get() + fileInfo.m_sampleCount);
  }

  if (fileInfo.HasGatherOffset() && !fileInfo.IsUnbinned() && !gatherOffsetValues.empty())
  {
    // use the longest gather in the segment to populate the list of offset values

    // find the longest gather
    int
      longestSecondaryKey = (*gatherOffsetValues.begin()).first;

    for (const auto& entry : gatherOffsetValues)
    {
      if (entry.second.size() > gatherOffsetValues[longestSecondaryKey].size())
      {
        longestSecondaryKey = entry.first;
      }
    }

    // copy the longest gather's offset values to the method arg
    const auto
      & longestGatherValues = gatherOffsetValues[longestSecondaryKey];
    offsetValues.assign(longestGatherValues.begin(), longestGatherValues.end());

    if (traceSpacingByOffset != TraceSpacingByOffset::Off)
    {
      // Determine if offset values are suitable for computed respacing:  Values are monotonically increasing
      size_t
        numSorted = 0;

      // check each gather to see if offsets are ordered within every gather
      for (const auto& entry : gatherOffsetValues)
      {
        const auto
          & offsets = entry.second;
        if (std::is_sorted(offsets.begin(), offsets.end()))
        {
          ++numSorted;
        }
        else
        {
          // if a gather is unordered then we're done
          break;
        }
      }

      if (numSorted == gatherOffsetValues.size())
      {
        // we can do respacing
        if (traceSpacingByOffset == TraceSpacingByOffset::Auto)
        {
          // change the parameter to On to indicate that respacing is in effect
          traceSpacingByOffset = TraceSpacingByOffset::On;
        }
      }
      else if (traceSpacingByOffset == TraceSpacingByOffset::Auto)
      {
        // the offset spacing doesn't conform to our needs for respacing, so force the setting from Auto to Off
        traceSpacingByOffset = TraceSpacingByOffset::Off;
      }
      else
      {
        // traceSpacingByOffset is set to On, but detected offset values aren't compatible with the respacing scheme

        auto msg = "The detected gather offset values are not monotonically increasing. This may indicate an incorrect header format for the trace header Offset field and/or an incorrect option value for --respace-gathers.";
        outputPrinter.printError("analyzeSegment", msg);
        return false;

        return false;
      }
    }

  }

  // If the secondary step couldn't be determined, set it to the last step or 1
  secondaryStep = secondaryStep ? secondaryStep : std::max(gatherSecondaryStep, 1);

  // Set value range
  valueRangeEstimator.GetValueRange(valueRange.Min, valueRange.Max);

  return success;
}

// Analog of analyzeSegment for offset-sorted SEGY
template<typename T>
bool
analyzePrimaryKey(const std::vector<DataProvider>& dataProviders, SEGYFileInfo const& fileInfo, const int primaryKey, float valueRangePercentile, OpenVDS::FloatRange& valueRange, int& fold, int& secondaryStep, std::vector<int>& offsetValues, OpenVDS::OutputPrinter &outputPrinter, OpenVDS::Error& error)
{
  assert(fileInfo.IsOffsetSorted());

  // TODO what kind of sanity checking should we do on offset-sorted segments?
  //if (segmentInfo.m_traceStop < segmentInfo.m_traceStart)
  //{
  //  m_errorString = "Invalid parameters, a valid segment info should always have a stop trace greater than or equal to the start trace";
  //  VDSImportSEGY::pLoggingInterface->LogError("VDSImportSEGY::analyzeSegment", m_errorString);
  //  return false;
  //}

  bool
    success = true;

  valueRange = OpenVDS::FloatRange(0.0f, 1.0f);
  secondaryStep = 0;
  fold = 1;
  offsetValues.clear();

  if (fileInfo.m_segmentInfoListsByOffset.empty() || fileInfo.m_segmentInfoListsByOffset.front().empty())
  {
    // no data to analyze
    return true;
  }

  // get all offset values from all files
  std::set<int>
    globalOffsets;
  for (const auto& offsetSegments : fileInfo.m_segmentInfoListsByOffset)
  {
    for (const auto& entry : offsetSegments)
    {
      globalOffsets.insert(entry.first);
    }
  }

  fold = static_cast<int>(globalOffsets.size());

  offsetValues.assign(globalOffsets.begin(), globalOffsets.end());

  const int32_t
    traceByteSize = fileInfo.TraceByteSize();

  int64_t
    traceBufferStart = 0,
    traceBufferSize = 0;

  std::vector<char>
    buffer;

  // 1. Find all the segments in all files that match the primary key
  // 2. Get a total trace count of all the segments; pass trace count to value range estimator
  std::map<size_t, std::vector<std::reference_wrapper<const SEGYSegmentInfo>>>
    providerSegments;
  int64_t
    primaryKeyTraceCount = 0;
  for (size_t fileIndex = 0; fileIndex < fileInfo.m_segmentInfoListsByOffset.size(); ++fileIndex)
  {
    const auto&
      offsetSegments = fileInfo.m_segmentInfoListsByOffset[fileIndex];
    for (const auto& entry : offsetSegments)
    {
      for (const auto& segmentInfo : entry.second)
      {
        if (segmentInfo.m_primaryKey == primaryKey)
        {
          providerSegments[fileIndex].push_back(std::cref(segmentInfo));
          primaryKeyTraceCount += segmentInfo.m_traceStop - segmentInfo.m_traceStart + 1;
        }
      }
    }
  }

  // Create min/max heaps for determining value range
  OpenVDS::HeapBasedValueRangeEstimator<T> valueRangeEstimator(valueRangePercentile, primaryKeyTraceCount * fileInfo.m_sampleCount);

  std::unique_ptr<T[]> sampleBuffer(new T[fileInfo.m_sampleCount]);

  // For secondary step need to scan all traces in all segments to get a complete (hopefully) list of secondary keys, then figure out start/end/step.
  // We need to get all secondary keys before doing any analysis because we may not encounter them in least-to-greatest order in the offset-sorted segments.
  std::set<int>
    allSecondaryKeys;

  for (const auto& entry : providerSegments)
  {
    auto&
      dataProvider = dataProviders[entry.first];
    auto&
      primaryKeySegments = entry.second;

    for (const auto &segmentInfoRef : primaryKeySegments)
    {
      const auto&
        segmentInfo = segmentInfoRef.get();
      for (int64_t trace = segmentInfo.m_traceStart; trace <= segmentInfo.m_traceStop; trace++)
      {
        if (trace - traceBufferStart >= traceBufferSize)
        {
          traceBufferStart = trace;
          traceBufferSize = (segmentInfo.m_traceStop - trace + 1) < 1000 ? int(segmentInfo.m_traceStop - trace + 1) : 1000;

          buffer.resize(traceByteSize * traceBufferSize);

          const int64_t
            offset = SEGY::TextualFileHeaderSize + SEGY::BinaryFileHeaderSize + traceByteSize * traceBufferStart;

          OpenVDS::Error
            readError;

          success = dataProvider.Read(buffer.data(), offset, (int32_t)(traceByteSize * traceBufferSize), readError);

          if (!success)
          {
            outputPrinter.printError("analyzePrimaryKey", error.string);
            break;
          }
        }

        const void
          * header = buffer.data() + traceByteSize * (trace - traceBufferStart),
          * data = buffer.data() + traceByteSize * (trace - traceBufferStart) + SEGY::TraceHeaderSize;

        const int
          tracePrimaryKey = SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness),
          traceSecondaryKey = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);

        allSecondaryKeys.insert(traceSecondaryKey);

        if (tracePrimaryKey != segmentInfo.m_primaryKey)
        {
          auto
            warnString = fmt::format("Warning: trace {} has a primary key that doesn't match with the segment. This trace will be ignored.", segmentInfo.m_traceStart + trace);
          outputPrinter.printWarning("analyzePrimaryKey", warnString);
          continue;
        }

        // Update value range
        copySamples<T>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, sampleBuffer.get(), (const unsigned char*)data, 0, fileInfo.m_sampleCount);
        valueRangeEstimator.UpdateValueRange(sampleBuffer.get(), sampleBuffer.get() + fileInfo.m_sampleCount);
      }
    }
  }

  // Determine the secondary step
  if (allSecondaryKeys.size() <= 1)
  {
    secondaryStep = 1;
  }
  else if (allSecondaryKeys.size() == 2)
  {
    secondaryStep = *allSecondaryKeys.rbegin() - *allSecondaryKeys.begin();
  }
  else
  {
    auto
      secondaryIter = allSecondaryKeys.begin();
    auto
      previousSecondary = *secondaryIter;
    auto
      workingStep = 0;
    secondaryStep = 0;
    while (++secondaryIter != allSecondaryKeys.end())
    {
      // Updating the secondary step with the previously computed step intentionally ignores the step of the final secondary key since it can be anomalous
      if (workingStep && (!secondaryStep || std::abs(workingStep) < std::abs(secondaryStep)))
      {
        secondaryStep = workingStep;
      }

      workingStep = *secondaryIter - previousSecondary;
      previousSecondary = *secondaryIter;
    }

    // If the secondary step couldn't be determined, set it to the last step or 1
    secondaryStep = secondaryStep ? secondaryStep : std::max(workingStep, 1);
  }

  // Set value range
  valueRangeEstimator.GetValueRange(valueRange.Min, valueRange.Max);

  return success;
}

std::pair<SEGYSegmentInfo, SEGYSegmentInfo>
findFirstLastInlineSegmentInfos(const SEGYFileInfo& fileInfo, const std::vector<int>& orderedListIndices)
{
  if (fileInfo.IsOffsetSorted())
  {
    // Find least/greatest inline numbers
    // Create synthetic segment infos for least/greatest inline using least/greatest crossline numbers on those inlines
    // If inline is ascending:  return (least-inline segment info, greatest-inline segment info)
    // else:  return (greatest-inline segment info, least-inline segment info)

    int
      minInline = INT32_MAX,
      maxInline = -INT32_MAX;
    const std::vector<SEGYSegmentInfo>
      * pLongestList = nullptr;
    for (const auto& offsetMap : fileInfo.m_segmentInfoListsByOffset)
    {
      for (const auto& entry : offsetMap)
      {
        if (pLongestList == nullptr || (entry.second.size() > pLongestList->size()))
        {
          pLongestList = &entry.second;
        }
        for (const auto& segmentInfo : entry.second)
        {
          minInline = std::min(minInline, segmentInfo.m_primaryKey);
          maxInline = std::max(maxInline, segmentInfo.m_primaryKey);
        }
      }
    }

    SEGYSegmentInfo
      minSegmentInfo{},
      maxSegmentInfo{};
    minSegmentInfo.m_primaryKey = minInline;
    minSegmentInfo.m_binInfoStart = { minInline, INT32_MAX, 0.0, 0.0 };
    minSegmentInfo.m_binInfoStop = { minInline, -INT32_MAX, 0.0, 0.0 };
    maxSegmentInfo.m_primaryKey = maxInline;
    maxSegmentInfo.m_binInfoStart = { maxInline, INT32_MAX, 0.0, 0.0 };
    maxSegmentInfo.m_binInfoStop = { maxInline, -INT32_MAX, 0.0, 0.0 };

    for (const auto& offsetMap : fileInfo.m_segmentInfoListsByOffset)
    {
      for (const auto& entry : offsetMap)
      {
        for (const auto& segmentInfo : entry.second)
        {
          if (segmentInfo.m_primaryKey == minInline)
          {
            if (segmentInfo.m_binInfoStart.m_crosslineNumber < minSegmentInfo.m_binInfoStart.m_crosslineNumber)
            {
              minSegmentInfo.m_binInfoStart = segmentInfo.m_binInfoStart;
            }
            if (segmentInfo.m_binInfoStop.m_crosslineNumber > minSegmentInfo.m_binInfoStop.m_crosslineNumber)
            {
              minSegmentInfo.m_binInfoStop = segmentInfo.m_binInfoStop;
            }
          }
          if (segmentInfo.m_primaryKey == maxInline)
          {
            if (segmentInfo.m_binInfoStart.m_crosslineNumber < maxSegmentInfo.m_binInfoStart.m_crosslineNumber)
            {
              maxSegmentInfo.m_binInfoStart = segmentInfo.m_binInfoStart;
            }
            if (segmentInfo.m_binInfoStop.m_crosslineNumber > maxSegmentInfo.m_binInfoStop.m_crosslineNumber)
            {
              maxSegmentInfo.m_binInfoStop = segmentInfo.m_binInfoStop;
            }
          }
        }
      }
    }

    const bool
      isAscending = pLongestList->front().m_primaryKey <= pLongestList->back().m_primaryKey;

    if (isAscending)
    {
      return { minSegmentInfo, maxSegmentInfo };
    }
    return { maxSegmentInfo, minSegmentInfo };
  }

  // else it's not offset-sorted
  return { std::cref(fileInfo.m_segmentInfoLists[orderedListIndices.front()].front()), std::cref(fileInfo.m_segmentInfoLists[orderedListIndices.back()].back()) };
}

int
BinInfoPrimaryKeyValue(PrimaryKeyValue primaryKey, const SEGYBinInfo& binInfo)
{
  if (primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    return binInfo.m_crosslineNumber;
  }
  return binInfo.m_inlineNumber;
}

int
BinInfoSecondaryKeyValue(PrimaryKeyValue primaryKey, const SEGYBinInfo& binInfo)
{
  if (primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    return binInfo.m_inlineNumber;
  }
  return binInfo.m_crosslineNumber;
}

bool
createSEGYMetadata(DataProvider &dataProvider, SEGYFileInfo const &fileInfo, OpenVDS::MetadataContainer& metadataContainer, SEGY::BinaryHeader::MeasurementSystem &measurementSystem, OpenVDS::Error& error)
{
  std::vector<uint8_t> textHeader(SEGY::TextualFileHeaderSize);
  std::vector<uint8_t> binaryHeader(SEGY::BinaryFileHeaderSize);

  // Read headers
  bool success = dataProvider.Read(textHeader.data(), 0, SEGY::TextualFileHeaderSize, error) &&
    dataProvider.Read(binaryHeader.data(), SEGY::TextualFileHeaderSize, SEGY::BinaryFileHeaderSize, error);

  if (!success) return false;

  // Create metadata
  metadataContainer.SetMetadataBLOB("SEGY", "TextHeader", textHeader);

  metadataContainer.SetMetadataBLOB("SEGY", "BinaryHeader", binaryHeader);

  metadataContainer.SetMetadataInt("SEGY", "Endianness", int(fileInfo.m_headerEndianness));
  metadataContainer.SetMetadataInt("SEGY", "DataSampleFormatCode", int(fileInfo.m_dataSampleFormatCode));

  measurementSystem = SEGY::BinaryHeader::MeasurementSystem(SEGY::ReadFieldFromHeader(binaryHeader.data(), SEGY::BinaryHeader::MeasurementSystemHeaderField, fileInfo.m_headerEndianness));
  return success;
}

void
createSurveyCoordinateSystemWKTAndUnitMetadata(SEGYFileInfo const& fileInfo, SEGY::BinaryHeader::MeasurementSystem segyMeasurementSystem, std::string const& crsWkt, OpenVDS::MetadataContainer& metadataContainer)
{
  if (fileInfo.m_segmentInfoLists.empty() && fileInfo.m_segmentInfoListsByOffset.empty()) return;

  if (!crsWkt.empty())
  {
    metadataContainer.SetMetadataString(LATTICE_CATEGORY, CRS_WKT, crsWkt);
  }
  if (segyMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Meters)
  {
    metadataContainer.SetMetadataString(LATTICE_CATEGORY, LATTICE_UNIT, KNOWNMETADATA_UNIT_METER);
  }
  else if (segyMeasurementSystem == SEGY::BinaryHeader::MeasurementSystem::Feet)
  {
    metadataContainer.SetMetadataString(LATTICE_CATEGORY, LATTICE_UNIT, KNOWNMETADATA_UNIT_FOOT);
  }
}

void
createSurveyCoordinateSystemMetadata(SEGYFileInfo const& fileInfo, const SurveyCoordinateSystemTransformer &surveyCoordinateSystemTransformer, OpenVDS::MetadataContainer& metadataContainer, PrimaryKeyValue primaryKey)
{
  if (fileInfo.m_segmentInfoLists.empty() && fileInfo.m_segmentInfoListsByOffset.empty()) return;

  if (fileInfo.Is2D()) return;

  double secondarySpacing[2] = { 0, 0 };

  size_t
    globalTotalSegments;
  auto
    orderedListIndices = getOrderedSegmentListIndices(fileInfo, globalTotalSegments);

  // Determine secondary spacing
  int countedSecondarySpacings = 0;

  auto
    secondaryUpdater = [&primaryKey, &countedSecondarySpacings, &secondarySpacing](const std::vector<SEGYSegmentInfo>& segmentInfoList)
  {
    for (auto const& segmentInfo : segmentInfoList)
    {
      const auto
        secondaryCount = BinInfoSecondaryKeyValue(primaryKey, segmentInfo.m_binInfoStop) - BinInfoSecondaryKeyValue(primaryKey, segmentInfo.m_binInfoStart);

      if (secondaryCount == 0 || BinInfoPrimaryKeyValue(primaryKey, segmentInfo.m_binInfoStart) != BinInfoPrimaryKeyValue(primaryKey, segmentInfo.m_binInfoStop)) continue;

      double segmentSecondarySpacing[3];

      segmentSecondarySpacing[0] = (segmentInfo.m_binInfoStop.m_ensembleXCoordinate - segmentInfo.m_binInfoStart.m_ensembleXCoordinate) / secondaryCount;
      segmentSecondarySpacing[1] = (segmentInfo.m_binInfoStop.m_ensembleYCoordinate - segmentInfo.m_binInfoStart.m_ensembleYCoordinate) / secondaryCount;

      secondarySpacing[0] += segmentSecondarySpacing[0];
      secondarySpacing[1] += segmentSecondarySpacing[1];

      countedSecondarySpacings++;
    }
  };

  for (size_t listIndex : orderedListIndices)
  {
    if (fileInfo.IsOffsetSorted())
    {
      for (const auto& offsetSegmentMap : fileInfo.m_segmentInfoListsByOffset)
      {
        for (const auto& entry : offsetSegmentMap)
        {
          secondaryUpdater(entry.second);
        }
      }
    }
    else
    {
      secondaryUpdater(fileInfo.m_segmentInfoLists[listIndex]);
    }
  }

  if (countedSecondarySpacings > 0)
  {
    secondarySpacing[0] /= countedSecondarySpacings;
    secondarySpacing[1] /= countedSecondarySpacings;
  }
  else
  {
    secondarySpacing[0] = 0;
    secondarySpacing[1] = 1;
  }

  // Determine primary spacing

  double primarySpacing[2] = { 0, 0 };

  const auto
    firstLastSegmentInfos = findFirstLastInlineSegmentInfos(fileInfo, orderedListIndices);
  const SEGYSegmentInfo
    & firstSegmentInfo = firstLastSegmentInfos.first,
    & lastSegmentInfo = firstLastSegmentInfos.second;

  if (BinInfoPrimaryKeyValue(primaryKey, firstSegmentInfo.m_binInfoStart) != BinInfoPrimaryKeyValue(primaryKey, lastSegmentInfo.m_binInfoStart))
  {
    int primaryNunberDelta = BinInfoPrimaryKeyValue(primaryKey, lastSegmentInfo.m_binInfoStart) - BinInfoPrimaryKeyValue(primaryKey, firstSegmentInfo.m_binInfoStart);
    int secondaryNunberDelta = BinInfoSecondaryKeyValue(primaryKey, lastSegmentInfo.m_binInfoStart) - BinInfoSecondaryKeyValue(primaryKey, firstSegmentInfo.m_binInfoStart);

    double offset[2] = { secondarySpacing[0] * secondaryNunberDelta,
               secondarySpacing[1] * secondaryNunberDelta };

    primarySpacing[0] = (lastSegmentInfo.m_binInfoStart.m_ensembleXCoordinate - firstSegmentInfo.m_binInfoStart.m_ensembleXCoordinate - offset[0]) / primaryNunberDelta;
    primarySpacing[1] = (lastSegmentInfo.m_binInfoStart.m_ensembleYCoordinate - firstSegmentInfo.m_binInfoStart.m_ensembleYCoordinate - offset[1]) / primaryNunberDelta;
  }
  else
  {
    // make square voxels
    primarySpacing[0] = secondarySpacing[1];
    primarySpacing[1] = -secondarySpacing[0];
  }

  double
    inlineSpacing[2] = {},
    crosslineSpacing[2] = {};

  if (primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    // map inline to secondary, crossline to primary
    inlineSpacing[0] = secondarySpacing[0];
    inlineSpacing[1] = secondarySpacing[1];
    crosslineSpacing[0] = primarySpacing[0];
    crosslineSpacing[1] = primarySpacing[1];
  }
  else
  {
    // map inline to primary, crossline to secondary
    inlineSpacing[0] = primarySpacing[0];
    inlineSpacing[1] = primarySpacing[1];
    crosslineSpacing[0] = secondarySpacing[0];
    crosslineSpacing[1] = secondarySpacing[1];
  }

  if (crosslineSpacing[0] == 0.0 && crosslineSpacing[1] == 1.0 && inlineSpacing[0] == 0.0)
  {
    // Deal with Headwave quirk:  If crossline spacing is (0, 1) (i.e. only one crossline) and the inline spacing is (0, anything) then Headwave
    // won't display sample data. In this case we'll change the crossline spacing to (-1, 0) (which seems to be how the Headwave SEGY importer
    // handles this case).
    crosslineSpacing[0] = -1;
    crosslineSpacing[1] = 0;
  }

  // Determine origin
  double origin[2];

  origin[0] = firstSegmentInfo.m_binInfoStart.m_ensembleXCoordinate;
  origin[1] = firstSegmentInfo.m_binInfoStart.m_ensembleYCoordinate;

  origin[0] -= inlineSpacing[0] * firstSegmentInfo.m_binInfoStart.m_inlineNumber;
  origin[1] -= inlineSpacing[1] * firstSegmentInfo.m_binInfoStart.m_inlineNumber;

  origin[0] -= crosslineSpacing[0] * firstSegmentInfo.m_binInfoStart.m_crosslineNumber;
  origin[1] -= crosslineSpacing[1] * firstSegmentInfo.m_binInfoStart.m_crosslineNumber;

  // Set coordinate system
  metadataContainer.SetMetadataDoubleVector2(LATTICE_CATEGORY, LATTICE_ORIGIN, OpenVDS::DoubleVector2(surveyCoordinateSystemTransformer.convertToVdsDistance(origin[0]), surveyCoordinateSystemTransformer.convertToVdsDistance(origin[1])));
  metadataContainer.SetMetadataDoubleVector2(LATTICE_CATEGORY, LATTICE_INLINE_SPACING, OpenVDS::DoubleVector2(surveyCoordinateSystemTransformer.convertToVdsDistance(inlineSpacing[0]), surveyCoordinateSystemTransformer.convertToVdsDistance(inlineSpacing[1])));
  metadataContainer.SetMetadataDoubleVector2(LATTICE_CATEGORY, LATTICE_CROSSLINE_SPACING, OpenVDS::DoubleVector2(surveyCoordinateSystemTransformer.convertToVdsDistance(crosslineSpacing[0]), surveyCoordinateSystemTransformer.convertToVdsDistance(crosslineSpacing[1])));
}

/////////////////////////////////////////////////////////////////////////////
bool
createImportInformationMetadata(const std::vector<DataProvider> &dataProviders, OpenVDS::MetadataContainer& metadataContainer, OpenVDS::Error &error)
{
  auto now = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm = *std::gmtime(&tt);
  auto duration = now.time_since_epoch();
  int millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;

  std::string importTimeStamp = fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);

  // Join all the input names into a single comma delimited string
  std::stringstream allNames;
  for (auto it = dataProviders.begin(); it != dataProviders.end(); ++it)
  {
    if(it != dataProviders.begin())
    {
      allNames << ",";
    }
    allNames << it->GetFileOrObjectName();
  }

  std::string inputFileName = allNames.str();

  // In lack of a better displayName we use the file name
  std::string displayName = inputFileName;

  // Use the timestamp from the first input
  std::string inputTimeStamp = dataProviders[0].LastWriteTime(error);
  if (error.code != 0)
  {
    return false;
  }

  // Sum the sizes of all the inputs
  int64_t
    inputFileSize = 0;
  for (const auto& provider : dataProviders)
  {
    inputFileSize += provider.Size(error);
    if (error.code != 0)
    {
      return false;
    }
  }

  // Set import information
  metadataContainer.SetMetadataString(KNOWNMETADATA_CATEGORY_IMPORTINFORMATION, KNOWNMETADATA_IMPORTINFORMATION_DISPLAYNAME, displayName);
  metadataContainer.SetMetadataString(KNOWNMETADATA_CATEGORY_IMPORTINFORMATION, KNOWNMETADATA_IMPORTINFORMATION_INPUTFILENAME, inputFileName);
  metadataContainer.SetMetadataDouble(KNOWNMETADATA_CATEGORY_IMPORTINFORMATION, KNOWNMETADATA_IMPORTINFORMATION_INPUTFILESIZE, (double)inputFileSize);
  metadataContainer.SetMetadataString(KNOWNMETADATA_CATEGORY_IMPORTINFORMATION, KNOWNMETADATA_IMPORTINFORMATION_INPUTTIMESTAMP, inputTimeStamp);
  metadataContainer.SetMetadataString(KNOWNMETADATA_CATEGORY_IMPORTINFORMATION, KNOWNMETADATA_IMPORTINFORMATION_IMPORTTIMESTAMP, importTimeStamp);
  return true;
}

void
buildUnbinned2DTraceInfo(TraceInfo2DManager* pTraceInfo2DManager, const SEGYFileInfo& fileInfo)
{
  for (const auto& segmentInfoList : fileInfo.m_segmentInfoLists)
  {
    for (const auto& segmentInfo : segmentInfoList)
    {
      TraceInfo2D info{};

      // Primary key is either ensemble number or energy source point number or receiver number. We'll
      // populate both ensembleNumber and energySourcePointNumber with the primary key value and sort
      // out which one (or neither) to write to metadata later on.
      info.ensembleNumber = segmentInfo.m_primaryKey;
      info.energySourcePointNumber = segmentInfo.m_primaryKey;

      info.traceNumber = segmentInfo.m_traceStart;
      info.x = segmentInfo.m_binInfoStart.m_ensembleXCoordinate;
      info.y = segmentInfo.m_binInfoStart.m_ensembleYCoordinate;
      pTraceInfo2DManager->AddTraceInfo(info);
    }
  }
}

struct TraceInfo2DOptions
{
  bool
    hasTracePositions,
    hasVerticalOffsets,
    hasESPs,
    hasEnsembleNumbers;
};

void
create2DTraceInformationMetadata(const TraceInfo2DOptions& options, TraceInfo2DManager * pTraceInfo2DManager, OpenVDS::MetadataContainer& metadataContainer, const SurveyCoordinateSystemTransformer& surveyCoordinateSystemTransformer, const std::vector<int>& keys, OpenVDS::Error& error)
{
  error = {};

  metadataContainer.ClearMetadata(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_TRACEPOSITIONS);
  metadataContainer.ClearMetadata(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_TRACEVERTICALOFFSETS);
  metadataContainer.ClearMetadata(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_ENERGYSOURCEPOINTNUMBERS);
  metadataContainer.ClearMetadata(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_ENSEMBLENUMBERS);

  if (pTraceInfo2DManager == nullptr || pTraceInfo2DManager->Count() == 0)
  {
    return;
  }

  // create KNOWNMETADATA_TRACECOORDINATES metadata items:  KNOWNMETADATA_TRACEPOSITIONS, KNOWNMETADATA_TRACEVERTICALOFFSETS, KNOWNMETADATA_ENERGYSOURCEPOINTNUMBERS, KNOWNMETADATA_ENSEMBLENUMBERS

  const auto
    nItems = keys.size();

  std::vector<double>
    tracePositions,
    verticalOffsets;
  std::vector<int32_t>
    espNumbers,
    ensembleNumbers;

  tracePositions.reserve(nItems * 2);
  verticalOffsets.reserve(nItems);
  espNumbers.reserve(nItems);
  ensembleNumbers.reserve(nItems);

  for (size_t i = 0; i < nItems; ++i)
  {
    const auto
      ensembleNumber = keys[i];
    const auto
      traceInfoIndex = pTraceInfo2DManager->GetIndexOfEnsembleNumber(ensembleNumber);

    if (traceInfoIndex < 0)
    {
      // No trace at this ensemble number
      tracePositions.push_back(0.0);
      tracePositions.push_back(0.0);
      verticalOffsets.push_back(0);
      espNumbers.push_back(0);
      ensembleNumbers.push_back(ensembleNumber);
    }
    else
    {
      const auto
       & item = pTraceInfo2DManager->Get(traceInfoIndex);
      assert(ensembleNumber == item.ensembleNumber);
      tracePositions.push_back(surveyCoordinateSystemTransformer.convertToVdsDistance(item.x));
      tracePositions.push_back(surveyCoordinateSystemTransformer.convertToVdsDistance(item.y));
      verticalOffsets.push_back(item.startTime);
      espNumbers.push_back(item.energySourcePointNumber);
      ensembleNumbers.push_back(item.ensembleNumber);
    }
  }

  if (options.hasTracePositions)
  {
    metadataContainer.SetMetadataBLOB(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_TRACEPOSITIONS, tracePositions);
  }
  if (options.hasVerticalOffsets)
  {
    metadataContainer.SetMetadataBLOB(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_TRACEVERTICALOFFSETS, verticalOffsets);
  }
  if (options.hasESPs)
  {
    metadataContainer.SetMetadataBLOB(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_ENERGYSOURCEPOINTNUMBERS, espNumbers);
  }
  if (options.hasEnsembleNumbers)
  {
    metadataContainer.SetMetadataBLOB(KNOWNMETADATA_TRACECOORDINATES, KNOWNMETADATA_ENSEMBLENUMBERS, ensembleNumbers);
  }

}

bool
parseSEGYFileInfoFile(const DataProvider& dataProvider, SEGYFileInfo& fileInfo, const size_t fileIndex, const bool isValidateHeader, OpenVDS::Error& error)
{
  const int64_t fileSize = dataProvider.Size(error);

  if (error.code != 0)
  {
    return false;
  }

  if (fileSize > INT_MAX)
  {
    return false;
  }

  std::unique_ptr<char[]>
    buffer(new char[fileSize]);

  dataProvider.Read(buffer.get(), 0, (int32_t)fileSize, error);

  if (error.code != 0)
  {
    return false;
  }

  try
  {
    Json::CharReaderBuilder
      rbuilder;

    rbuilder["collectComments"] = false;

    std::string
      errs;

    std::unique_ptr<Json::CharReader>
      reader(rbuilder.newCharReader());

    Json::Value
      jsonFileInfo;

    bool
      success = reader->parse(buffer.get(), buffer.get() + fileSize, &jsonFileInfo, &errs);

    if (!success)
    {
      error.string = errs;
      error.code = -1;
      return false;
    }

    if (isValidateHeader)
    {
      const bool
        isValid =
        fileInfo.m_persistentID == strtoull(jsonFileInfo["persistentID"].asCString(), nullptr, 16) &&
        fileInfo.m_headerEndianness == EndiannessFromJson(jsonFileInfo["headerEndianness"]) &&
        fileInfo.m_dataSampleFormatCode == SEGY::BinaryHeader::DataSampleFormatCode(jsonFileInfo["dataSampleFormatCode"].asInt()) &&
        fileInfo.m_sampleCount == jsonFileInfo["sampleCount"].asInt() &&
        fileInfo.m_sampleIntervalMilliseconds == jsonFileInfo["sampleInterval"].asDouble() &&
        fileInfo.m_primaryKey == HeaderFieldFromJson(jsonFileInfo["primaryKey"]) &&
        fileInfo.m_secondaryKey == HeaderFieldFromJson(jsonFileInfo["secondaryKey"]);
      if (!isValid)
      {
        error.string = "SEGY header data in JSON scan file does not match existing SEGY header data";
        error.code = -1;
        return false;
      }
    }
    else
    {
      fileInfo.m_persistentID = strtoull(jsonFileInfo["persistentID"].asCString(), nullptr, 16);
      fileInfo.m_headerEndianness = EndiannessFromJson(jsonFileInfo["headerEndianness"]);
      fileInfo.m_dataSampleFormatCode = SEGY::BinaryHeader::DataSampleFormatCode(jsonFileInfo["dataSampleFormatCode"].asInt());
      fileInfo.m_sampleCount = jsonFileInfo["sampleCount"].asInt();
      fileInfo.m_startTimeMilliseconds = jsonFileInfo["startTime"].asDouble();
      fileInfo.m_sampleIntervalMilliseconds = jsonFileInfo["sampleInterval"].asDouble();
      fileInfo.m_traceCounts.clear();
      fileInfo.m_traceCounts.push_back(jsonFileInfo["traceCount"].asInt64());
      fileInfo.m_primaryKey = HeaderFieldFromJson(jsonFileInfo["primaryKey"]);
      fileInfo.m_secondaryKey = HeaderFieldFromJson(jsonFileInfo["secondaryKey"]);
    }

    if (fileInfo.m_traceCounts.size() <= fileIndex)
    {
      fileInfo.m_traceCounts.resize(fileIndex + 1);
    }
    fileInfo.m_traceCounts[fileIndex] = jsonFileInfo["traceCount"].asInt64();

    if (fileInfo.m_segyType == SEGY::SEGYType::PrestackOffsetSorted)
    {
      if (fileInfo.m_segmentInfoListsByOffset.size() <= fileIndex)
      {
        fileInfo.m_segmentInfoListsByOffset.resize(fileIndex + 1);
      }
      auto&
        offsetMap = fileInfo.m_segmentInfoListsByOffset[fileIndex];
      offsetMap.clear();

      const auto&
        jsonSegmentInfo = jsonFileInfo["segmentInfo"];
      for (Json::ValueConstIterator iter = jsonSegmentInfo.begin(); iter != jsonSegmentInfo.end(); ++iter)
      {
        const auto
          offsetValue = std::stoi(iter.key().asString());
        auto&
          segmentInfoList = offsetMap[offsetValue];
        for (const auto& jsonSegmentInfo : *iter)
        {
          segmentInfoList.push_back(segmentInfoFromJson(jsonSegmentInfo));
        }
      }
    }
    else
    {
      if (fileInfo.m_segmentInfoLists.size() <= fileIndex)
      {
        fileInfo.m_segmentInfoLists.resize(fileIndex + 1);
      }
      auto&
        segmentInfo = fileInfo.m_segmentInfoLists[fileIndex];
      for (Json::Value jsonSegmentInfo : jsonFileInfo["segmentInfo"])
      {
        segmentInfo.push_back(segmentInfoFromJson(jsonSegmentInfo));
      }
    }
  }
  catch (Json::Exception &e)
  {
    error.string = fmt::format("Failed to parse JSON SEG-Y file info file: {}", e.what());
    error.code = -1;
    return false;
  }

  return true;
}

std::vector<OpenVDS::VolumeDataAxisDescriptor>
createAxisDescriptors(SEGYFileInfo const& fileInfo, SEGY::SampleUnits sampleUnits, int fold, int inlineStep, int crosslineStep, bool keepOriginalOrder, bool is2DCDPAxis, const int ensembleCount)
{
  std::vector<OpenVDS::VolumeDataAxisDescriptor>
    axisDescriptors;

  const char *sampleUnit = "";

  switch(sampleUnits)
  {
  case SEGY::SampleUnits::Milliseconds: sampleUnit = KNOWNMETADATA_UNIT_MILLISECOND; break;
  case SEGY::SampleUnits::Feet:         sampleUnit = KNOWNMETADATA_UNIT_FOOT;        break;
  case SEGY::SampleUnits::Meters:       sampleUnit = KNOWNMETADATA_UNIT_METER;       break;
  default:
    assert(0 && "Unknown sample unit");
  }

  axisDescriptors.emplace_back(fileInfo.m_sampleCount, KNOWNMETADATA_SURVEYCOORDINATE_INLINECROSSLINE_AXISNAME_SAMPLE, sampleUnit, (float)fileInfo.m_startTimeMilliseconds, (float)fileInfo.m_startTimeMilliseconds + (fileInfo.m_sampleCount - 1) * (float)fileInfo.m_sampleIntervalMilliseconds);

  if (fileInfo.Is2D())
  {
    assert(fileInfo.m_segmentInfoLists.size() == 1 && fileInfo.m_segmentInfoLists[0].size() == 1);

    if (fold > 1)
    {
      axisDescriptors.emplace_back(fold, VDS_DIMENSION_TRACE_NAME(VDS_DIMENSION_TRACE_SORT_OFFSET), KNOWNMETADATA_UNIT_UNITLESS, 1.0f, static_cast<float>(fold));
    }

    const auto
      & segmentInfo = fileInfo.m_segmentInfoLists[0][0];

    int
      axisSampleCount;
    float
      coordinateStart,
      coordinateEnd;
    const char
      * axisName;
    if (is2DCDPAxis)
    {
      axisSampleCount = (segmentInfo.m_binInfoStop.m_crosslineNumber - segmentInfo.m_binInfoStart.m_crosslineNumber) / crosslineStep + 1;
      axisName = VDS_DIMENSION_CDP_NAME;

      // the secondary key axis value range is CDP number range
      coordinateStart = static_cast<float>(segmentInfo.m_binInfoStart.m_crosslineNumber);
      coordinateEnd = static_cast<float>(segmentInfo.m_binInfoStop.m_crosslineNumber);
    }
    else
    {
      axisSampleCount = ensembleCount;
      axisName = VDS_DIMENSION_BARE_TRACE_NAME;

      // the secondary key axis will be 1..N
      coordinateStart = 1.0f;
      coordinateEnd = static_cast<float>(axisSampleCount);

    }
    axisDescriptors.emplace_back(axisSampleCount, axisName, KNOWNMETADATA_UNIT_UNITLESS, coordinateStart, coordinateEnd);
  }
  else if (fileInfo.IsUnbinned())
  {
    int
      maxTraceNumber = 0;
    size_t
      totalSegmentsCount = 0;

    for (const auto& segmentInfoList : fileInfo.m_segmentInfoLists)
    {
      totalSegmentsCount += segmentInfoList.size();

      for (const auto& segmentInfo : segmentInfoList)
      {
        maxTraceNumber = std::max(maxTraceNumber, segmentInfo.m_binInfoStop.m_crosslineNumber);
      }
    }

    axisDescriptors.emplace_back(maxTraceNumber, VDS_DIMENSION_TRACE_NAME(VDS_DIMENSION_TRACE_SORT_OFFSET), KNOWNMETADATA_UNIT_UNITLESS, 1.0f, (float)maxTraceNumber);

    const char
      * axisName;

    // figure out the unbinned primary axis name
    //switch (fileInfo.m_segyType)
    //{
    //case SEGY::SEGYType::ReceiverGathers:
    //  axisName = VDS_DIMENSION_RECEIVER_NAME;
    //  break;

    //case SEGY::SEGYType::ShotGathers:
    //  axisName = VDS_DIMENSION_SHOT_NAME;
    //  break;

    //case SEGY::SEGYType::CDPGathers:
    //default:
    //  axisName = VDS_DIMENSION_CDP_NAME;
    //  break;
    //}

    // Headwave uses Gather for all types?
    axisName = VDS_DIMENSION_GATHER_NAME;

    axisDescriptors.emplace_back(static_cast<int>(totalSegmentsCount), axisName, KNOWNMETADATA_UNIT_UNITLESS, 1.0f, static_cast<float>(totalSegmentsCount));
  }
  else
  {
    if (fold > 1)
    {
      axisDescriptors.emplace_back(fold, VDS_DIMENSION_TRACE_NAME(VDS_DIMENSION_TRACE_SORT_OFFSET), KNOWNMETADATA_UNIT_UNITLESS, 1.0f, static_cast<float>(fold));
    }

    int
      minInline,
      minCrossline,
      maxInline,
      maxCrossline;

    auto updateMinMaxes = [&minInline, &minCrossline, &maxInline, &maxCrossline](const SEGYSegmentInfo& segmentInfo)
    {
      minInline = std::min(minInline, segmentInfo.m_binInfoStart.m_inlineNumber);
      minInline = std::min(minInline, segmentInfo.m_binInfoStop.m_inlineNumber);
      maxInline = std::max(maxInline, segmentInfo.m_binInfoStart.m_inlineNumber);
      maxInline = std::max(maxInline, segmentInfo.m_binInfoStop.m_inlineNumber);

      minCrossline = std::min(minCrossline, segmentInfo.m_binInfoStart.m_crosslineNumber);
      minCrossline = std::min(minCrossline, segmentInfo.m_binInfoStop.m_crosslineNumber);
      maxCrossline = std::max(maxCrossline, segmentInfo.m_binInfoStart.m_crosslineNumber);
      maxCrossline = std::max(maxCrossline, segmentInfo.m_binInfoStop.m_crosslineNumber);
    };

    if (fileInfo.IsOffsetSorted())
    {
      const auto&
        firstSegment = fileInfo.m_segmentInfoListsByOffset.front().begin()->second.front();
      minInline = firstSegment.m_binInfoStart.m_inlineNumber;
      minCrossline = firstSegment.m_binInfoStart.m_crosslineNumber;
      maxInline = minInline;
      maxCrossline = minCrossline;

      for (const auto& offsetSegments : fileInfo.m_segmentInfoListsByOffset)
      {
        for (const auto& entry : offsetSegments)
        {
          for (const auto& segmentInfo : entry.second)
          {
            updateMinMaxes(segmentInfo);
          }
        }
      }
    }
    else
    {
      minInline = fileInfo.m_segmentInfoLists[0][0].m_binInfoStart.m_inlineNumber;
      minCrossline = fileInfo.m_segmentInfoLists[0][0].m_binInfoStart.m_crosslineNumber;
      maxInline = minInline;
      maxCrossline = minCrossline;

      for (const auto& segmentInfoList : fileInfo.m_segmentInfoLists)
      {
        for (const auto& segmentInfo : segmentInfoList)
        {
          updateMinMaxes(segmentInfo);
        }
      }
    }

    if(inlineStep < 0)
    {
      if(keepOriginalOrder)
      {
        std::swap(minInline, maxInline);
      }
      else
      {
        inlineStep = std::abs(inlineStep);
      }
    }

    if(crosslineStep < 0)
    {
      if(keepOriginalOrder)
      {
        std::swap(minCrossline, maxCrossline);
      }
      else
      {
        crosslineStep = std::abs(crosslineStep);
      }
    }

    // Ensure the max inline/crossline is a multiple of the step size from the min
    maxCrossline += (maxCrossline - minCrossline) % crosslineStep;
    maxInline += (maxInline - minInline) % std::abs(inlineStep);

    const int
      inlineCount = 1 + (maxInline - minInline) / inlineStep,
      crosslineCount = 1 + (maxCrossline - minCrossline) / crosslineStep;

    axisDescriptors.emplace_back(crosslineCount, KNOWNMETADATA_SURVEYCOORDINATE_INLINECROSSLINE_AXISNAME_CROSSLINE, KNOWNMETADATA_UNIT_UNITLESS, (float)minCrossline, (float)maxCrossline);
    axisDescriptors.emplace_back(inlineCount, KNOWNMETADATA_SURVEYCOORDINATE_INLINECROSSLINE_AXISNAME_INLINE, KNOWNMETADATA_UNIT_UNITLESS, (float)minInline, (float)maxInline);
  }

  return axisDescriptors;
}

struct OffsetChannelInfo
{
  float offsetStart;
  float offsetEnd;
  float offsetStep;
  bool  hasOffset;

  OffsetChannelInfo(bool has, float start, float end, float step) : offsetStart(start), offsetEnd(end), offsetStep(step), hasOffset(has) {}
};

static std::vector<OpenVDS::VolumeDataChannelDescriptor>
createChannelDescriptors(SEGYFileInfo const& fileInfo, OpenVDS::FloatRange const& valueRange, const OffsetChannelInfo& offsetInfo, const std::string& attributeName, const std::string& attributeUnit,
  bool isAzimuthEnabled, bool isMuteEnabled, VDSFormatOverride formatOverride, double overrideScale, bool isOverrideIntegerScale, const OpenVDS::FloatRange overrideScaleAndOffset, OpenVDS::Error &error)
{
  std::vector<OpenVDS::VolumeDataChannelDescriptor>
    channelDescriptors;

  // Primary channel
  auto format = SEGY::convertSegyFormat(fileInfo.m_dataSampleFormatCode, error);
  if (error.code)
    return channelDescriptors;

  float
    integerOffset = -(float)SEGY::GetVDSIntegerOffsetForDataSampleFormat(fileInfo.m_dataSampleFormatCode), // Observe this is a negative value.
    integerScale = 1.0f; // SEGY does not support integer scaling, so always 1

  // Adjust the value-range with the integerOffset. 
  // For float formats, integerOffset will be zero and this will not modify the valueRange.
  OpenVDS::FloatRange
    effectiveValueRange(valueRange.Min + integerOffset, valueRange.Max + integerOffset);

  if (formatOverride == VDSFormatOverride::Override16Bit)
  {
    format = OpenVDS::VolumeDataFormat::Format_U16;
    integerOffset = valueRange.Min;
    integerScale = static_cast<float>(overrideScale);
  }
  else if (formatOverride == VDSFormatOverride::Override8Bit)
  {
    format = OpenVDS::VolumeDataFormat::Format_U8;
    integerOffset = valueRange.Min;
    integerScale = static_cast<float>(overrideScale);
  }

  if (isOverrideIntegerScale)
  {
    integerScale = overrideScaleAndOffset.Min;
    integerOffset = overrideScaleAndOffset.Max;
  }

  channelDescriptors.emplace_back(format, OpenVDS::VolumeDataComponents::Components_1, attributeName.c_str(), attributeUnit.c_str(), effectiveValueRange.Min, effectiveValueRange.Max, OpenVDS::VolumeDataMapping::Direct, 1, OpenVDS::VolumeDataChannelDescriptor::Default, integerScale, integerOffset);

  // Trace defined flag
  channelDescriptors.emplace_back(OpenVDS::VolumeDataFormat::Format_U8, OpenVDS::VolumeDataComponents::Components_1, "Trace", "", 0.0f, 1.0f, OpenVDS::VolumeDataMapping::PerTrace, OpenVDS::VolumeDataChannelDescriptor::DiscreteData);

  // SEG-Y trace headers
  channelDescriptors.emplace_back(OpenVDS::VolumeDataFormat::Format_U8, OpenVDS::VolumeDataComponents::Components_1, "SEGYTraceHeader", "", 0.0f, 255.0f, OpenVDS::VolumeDataMapping::PerTrace, SEGY::TraceHeaderSize, OpenVDS::VolumeDataChannelDescriptor::DiscreteData | OpenVDS::VolumeDataChannelDescriptor::NoLossyCompressionUseZip | OpenVDS::VolumeDataChannelDescriptor::NotRenderable, 1.0f, 0.0f);

  if (offsetInfo.hasOffset)
  {
    // offset channel
    channelDescriptors.emplace_back(OpenVDS::VolumeDataFormat::Format_R32, OpenVDS::VolumeDataComponents::Components_1, "Offset", KNOWNMETADATA_UNIT_METER, float(offsetInfo.offsetStart), float(offsetInfo.offsetEnd), OpenVDS::VolumeDataMapping::PerTrace, OpenVDS::VolumeDataChannelDescriptor::NoLossyCompression);

    // TODO channels for other gather types - "Angle", "Vrms", "Frequency"
  }

  if (isAzimuthEnabled)
  {
    channelDescriptors.emplace_back(OpenVDS::VolumeDataFormat::Format_R32, OpenVDS::VolumeDataComponents::Components_1, "Azimuth", "deg", 0.0f, 360.0f, OpenVDS::VolumeDataMapping::PerTrace, OpenVDS::VolumeDataChannelDescriptor::NoLossyCompression);
  }

  if (isMuteEnabled)
  {
    channelDescriptors.emplace_back(OpenVDS::VolumeDataFormat::Format_U16, OpenVDS::VolumeDataComponents::Components_2, "Mute", KNOWNMETADATA_UNIT_MILLISECOND, 0.0f, 65535.0f, OpenVDS::VolumeDataMapping::PerTrace, OpenVDS::VolumeDataChannelDescriptor::NoLossyCompression);
  }

  return channelDescriptors;
}

int64_t
findFirstTrace(TraceDataManager& traceDataManager, int primaryKey, int secondaryKey, SEGYFileInfo const& fileInfo, int64_t traceStart, int64_t traceStop, int secondaryStart, int secondaryStop, OpenVDS::Error& error)
{
  const bool isSecondaryIncreasing = (secondaryStop >= secondaryStart);

  // Check if trace is at the start of the range or completely outside the range (this also handles cases where secondaryStart == secondaryStop which would fail to make a guess below)
  if (isSecondaryIncreasing ? (secondaryKey <= secondaryStart) :
    (secondaryKey >= secondaryStart))
  {
    return traceStart;
  }
  else if (isSecondaryIncreasing ? (secondaryKey > secondaryStop) :
    (secondaryKey < secondaryStop))
  {
    return traceStop + 1;
  }

  // Make an initial guess at which trace we start on based on linear interpolation
  int64_t trace = traceStart + (secondaryKey - secondaryStart) * (traceStop - traceStart) / (secondaryStop - secondaryStart);

  while (traceStart < traceStop - 1)
  {
    const char* header = traceDataManager.getTraceData(trace, error);
    if (error.code != 0)
    {
      return traceStart;
    }

    int primaryTest = fileInfo.Is2D() ? 0 : SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness),
      secondaryTest = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);
    int64_t traceDelta;

    if (primaryTest == primaryKey)
    {
      if ((secondaryTest >= secondaryKey) == isSecondaryIncreasing)
      {
        traceStop = trace;
        secondaryStop = secondaryTest;
      }
      else
      {
        traceStart = trace;
        secondaryStart = secondaryTest;
      }

      if (secondaryStop == secondaryStart)
      {
        traceDelta = 0;
      }
      else
      {
        traceDelta = (secondaryKey - secondaryTest) * (traceStop - traceStart) / (secondaryStop - secondaryStart);
      }
    }
    else
    {
      // We need to handle corrupted traces without hanging, so we scan backwards until we find a valid trace and then we update the interval to not include the corrupted traces
      for (int64_t scan = trace - 1; scan > traceStart; scan--)
      {
        header = traceDataManager.getTraceData(scan, error);
        if (error.code != 0)
        {
          return traceStart;
        }

        primaryTest = SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness);
        if (primaryTest == primaryKey)
        {
          secondaryTest = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);

          if ((secondaryTest >= secondaryKey) == isSecondaryIncreasing)
          {
            traceStop = scan;
            secondaryStop = secondaryTest;
            break;
          }
          else
          {
            // Start with the invalid trace and pretend it has the same secondary key as the previous valid trace
            traceStart = trace;
            secondaryStart = secondaryTest;
            break;
          }
        }
      }

      // If no valid trace was found before, start with the invalid trace and pretend it has the same secondary key as the previous valid trace
      if (primaryTest != primaryKey)
      {
        traceStart = trace;
      }

      traceDelta = 0;
    }

    // If the guessed trace is outside the range we already established, we do binary search instead -- this should ensure the range is always shrinking so we are guaranteed to terminate
    if (traceDelta == 0 || trace + traceDelta <= traceStart || trace + traceDelta >= traceStop)
    {
      trace = (traceStart + traceStop) / 2;
    }
    else
    {
      trace += traceDelta;
    }
  }

  if (isSecondaryIncreasing)
    return traceStop;
  return traceStart;
}

int64_t
findFirstTrace(TraceDataManager& traceDataManager, const SEGYSegmentInfo& segment, int secondaryKey, SEGYFileInfo const& fileInfo, PrimaryKeyValue primaryKey, OpenVDS::Error error)
{
  const int secondaryStart = BinInfoSecondaryKeyValue(primaryKey, segment.m_binInfoStart);
  const int secondaryStop = BinInfoSecondaryKeyValue(primaryKey, segment.m_binInfoStop);

  return findFirstTrace(traceDataManager, segment.m_primaryKey, secondaryKey, fileInfo, segment.m_traceStart, segment.m_traceStop, secondaryStart, secondaryStop, error);
}

int
VoxelIndexToDataIndex(const SEGYFileInfo& fileInfo, const int primaryIndex, const int secondaryIndex, const int tertiaryIndex, const int chunkMin[6], const int pitch[6])
{
  // The given voxel position consists of the indices for sample, trace (offset), secondary key, primary key. Which of those indices is actually used will depend on the SEGY type.

  // In all cases we assume the sample index is 0, and we won't use it when computing the data index

  if (fileInfo.Is2D())
  {
    if (fileInfo.HasGatherOffset())
    {
      // use indices for trace, secondary key
      return (secondaryIndex - chunkMin[2]) * pitch[2] + (tertiaryIndex - chunkMin[1]) * pitch[1];
    }
    // else use index for secondary key
    return (secondaryIndex - chunkMin[1]) * pitch[1];
  }

  if (fileInfo.Is4D())
  {
    // use all the indices
    return (primaryIndex - chunkMin[3]) * pitch[3] + (secondaryIndex - chunkMin[2]) * pitch[2] + (tertiaryIndex - chunkMin[1]) * pitch[1];
  }

  // else it's 3D poststack; use the indices for secondary key, primary key
  return (primaryIndex - chunkMin[2]) * pitch[2] + (secondaryIndex - chunkMin[1]) * pitch[1];
}

int
TraceIndex2DToEnsembleNumber(TraceInfo2DManager * pTraceInfo2DManager, int traceIndex, OpenVDS::Error & error)
{
  assert(pTraceInfo2DManager != nullptr);

  error = {};

  if (pTraceInfo2DManager == nullptr)
  {
    error.code = -1;
    error.string = "TraceIndex2DToEnsembleNumber:  2D trace information is missing";
    return 0;
  }

  assert(traceIndex >= 0);
  assert(traceIndex < pTraceInfo2DManager->Count());
  if (traceIndex < 0 || traceIndex >= pTraceInfo2DManager->Count())
  {
    error.code = -1;
    error.string = "TraceIndex2DToEnsembleNumber:  Requested trace index is missing from 2D trace info";
    return 0;
  }

  return pTraceInfo2DManager->Get(traceIndex).ensembleNumber;
}

int64_t
EnsembleIndex2DToTraceNumber(TraceInfo2DManager* pTraceInfo2DManager, int ensembleIndex, OpenVDS::Error& error)
{
  assert(pTraceInfo2DManager != nullptr);

  error = {};

  if (pTraceInfo2DManager == nullptr)
  {
    error.code = -1;
    error.string = "EnsembleNumber2DToTraceNumber:  2D trace information is missing";
    return 0;
  }

  assert(ensembleIndex >= 0);
  assert(ensembleIndex < pTraceInfo2DManager->Count());
  if (ensembleIndex < 0 || ensembleIndex >= pTraceInfo2DManager->Count())
  {
    error.code = -1;
    error.string = "EnsembleIndex2DToTraceNumber:  Requested trace index is missing from 2D trace info";
    return 0;
  }

  const auto& info = pTraceInfo2DManager->Get(ensembleIndex);
  return info.traceNumber;
}

int
SecondaryKeyDimension(const SEGYFileInfo& fileInfo, PrimaryKeyValue primaryKey)
{
  if ((fileInfo.m_segyType == SEGY::SEGYType::Prestack && primaryKey != PrimaryKeyValue::CrosslineNumber) || fileInfo.m_segyType == SEGY::SEGYType::Prestack2D || fileInfo.m_segyType == SEGY::SEGYType::PrestackOffsetSorted)
  {
    return 2;
  }
  if (fileInfo.m_segyType == SEGY::SEGYType::Prestack && primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    return 3;
  }
  if (fileInfo.m_segyType == SEGY::SEGYType::Poststack && primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    return 2;
  }
  return 1;
}

int
PrimaryKeyDimension(const SEGYFileInfo& fileInfo, PrimaryKeyValue primaryKey)
{
  if (fileInfo.Is4D() || fileInfo.m_segyType == SEGY::SEGYType::Prestack2D)
  {
    // Prestack2D doesn't really have a primary key dimension, but we'll use the same one as for Prestack as sort of a placeholder.
    if (primaryKey == PrimaryKeyValue::CrosslineNumber)
    {
      return 2;
    }
    return 3;
  }
  if (primaryKey == PrimaryKeyValue::CrosslineNumber)
  {
    return 1;
  }
  return 2;
}

int
findChannelDescriptorIndex(const std::string& channelName, const std::vector<OpenVDS::VolumeDataChannelDescriptor>& channelDescriptors)
{
  for (int index = 0; index < static_cast<int>(channelDescriptors.size()); ++index)
  {
    if (channelName == channelDescriptors[index].GetName())
    {
      return index;
    }
  }

  // not found
  return -1;
}

class GatherSpacing
{
public:
  GatherSpacing(int64_t firstTrace) : m_isRespace(false), m_firstTrace(firstTrace) {}

  GatherSpacing(int64_t firstTrace, std::unordered_map<int64_t, int> traceIndices) : m_isRespace(true), m_firstTrace(firstTrace), m_traceIndices(std::move(traceIndices)) {}

  int GetTertiaryIndex(int64_t traceNumber)
  {
    if (m_isRespace)
    {
      assert(traceNumber >= m_firstTrace && traceNumber < m_firstTrace + static_cast<int64_t>(m_traceIndices.size()));
      return m_traceIndices[traceNumber];
    }

    assert(traceNumber >= m_firstTrace);
    return static_cast<int>(traceNumber - m_firstTrace);
  }

private:
  bool       m_isRespace;
  int64_t    m_firstTrace;
  std::unordered_map<int64_t, int>
    m_traceIndices;
};

std::unique_ptr<GatherSpacing>
CalculateGatherSpacing(const SEGYFileInfo& fileInfo, const int fold, const std::vector<int>& globalOffsetValues, TraceDataManager& traceDataManager, TraceSpacingByOffset traceSpacingByOffset, int64_t firstTrace, OpenVDS::OutputPrinter &outputPrinter)
{
  if (fileInfo.Is4D() && traceSpacingByOffset != TraceSpacingByOffset::Off)
  {
    OpenVDS::Error
      error;
    const char
      * header = traceDataManager.getTraceData(firstTrace, error);
    if (error.code != 0)
    {
      return std::unique_ptr<GatherSpacing>(new GatherSpacing(firstTrace));
    }

    const auto
      primaryKey = SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness),
      secondaryKey = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);

    std::vector<int>
      gatherOffsets;
    gatherOffsets.reserve(fold);
    gatherOffsets.push_back(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offset"], fileInfo.m_headerEndianness));

    // read traces and stuff offset into a vector while primaryKey/secondaryKey match
    for (int64_t trace = firstTrace + 1; trace < traceDataManager.fileTraceCount() && trace < firstTrace + fold; ++trace)
    {
      header = traceDataManager.getTraceData(trace, error);
      if (error.code != 0)
      {
        break;
      }
      const auto
        testPrimary = SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness),
        testSecondary = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);
      if (testPrimary != primaryKey || testSecondary != secondaryKey)
      {
        break;
      }

      gatherOffsets.push_back(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offset"], fileInfo.m_headerEndianness));
    }

    // apply respace algorithm
    int
      gatherIndex = 0,
      offsetIndex = 0,
      tertiaryIndex = 0;
    std::unordered_map<int64_t, int>
      traceIndices;
    auto
      hasRoom = [&fold, &tertiaryIndex, &gatherOffsets, &gatherIndex]() { return (fold - tertiaryIndex) - (static_cast<int>(gatherOffsets.size()) - gatherIndex) > 0; };
    auto
      offsetAtIndex = [&globalOffsetValues](int index) { return globalOffsetValues[index]; };

    // skip tertiary indices before the first trace's offset
    while (hasRoom() && offsetIndex < fold && offsetAtIndex(offsetIndex) != gatherOffsets[0])
    {
      ++offsetIndex;
      ++tertiaryIndex;
    }

    // map traces to tertiary indices while we have room for dead traces and we have more input traces
    while (hasRoom() && gatherIndex < static_cast<int>(gatherOffsets.size()))
    {
      // process all the traces whose offset matches the current offset
      const auto
        currentOffset = offsetAtIndex(offsetIndex);
      while (hasRoom() && gatherIndex < static_cast<int>(gatherOffsets.size()) && gatherOffsets[gatherIndex] == currentOffset)
      {
        traceIndices[firstTrace + gatherIndex] = tertiaryIndex;
        ++gatherIndex;
        ++tertiaryIndex;
      }

      // advance to the next offset
      ++offsetIndex;

      // try to advance to tertiary index so that the next trace's offset is placed near-ish to where it might be expected
      if (gatherIndex < static_cast<int>(gatherOffsets.size()) && tertiaryIndex <= offsetIndex)
      {
        while (hasRoom() && gatherOffsets[gatherIndex] != offsetAtIndex(offsetIndex))
        {
          ++tertiaryIndex;
          ++offsetIndex;

          // hasRoom() will effectively take care of bounds-checking tertiaryIndex, but we need to explicitly check offsetIndex
          if (offsetIndex >= fold)
          {
            // if the trace's offset doesn't match any expected offset values then this is unexpected and may indicate a data problem
            const auto
              warnString = fmt::format("Warning:  Unknown trace header Offset value {} found at primary key {}, secondary key {}. This may indicate corrupt data or an incorrect header format.", gatherOffsets[gatherIndex], primaryKey, secondaryKey);
            outputPrinter.printWarning("Data", warnString);

            // return a no-respacing GatherSpacing
            return std::unique_ptr<GatherSpacing>(new GatherSpacing(firstTrace));
          }
        }
      }
    }

    // map remaining traces to remaining indices
    while (gatherIndex < static_cast<int>(gatherOffsets.size()))
    {
      traceIndices[firstTrace + gatherIndex] = tertiaryIndex;
      ++gatherIndex;
      ++tertiaryIndex;
    }

    return std::unique_ptr<GatherSpacing>(new GatherSpacing(firstTrace, traceIndices));
  }

  // else return a GatherSpacing that doesn't alter the spacing
  return std::unique_ptr<GatherSpacing>(new GatherSpacing(firstTrace));
}

// Converts a trace of floating point samples to quantized 8/16-bit values.
//
template<typename T>
void
ConvertOverrideTrace(std::vector<T>& intTrace, const std::vector<float>& floatTrace, const OpenVDS::FloatRange& valueRange, double integerScale)
{
  for (size_t i = 0; i < floatTrace.size(); ++i)
  {
    // ensure current value is within the value range
    auto sampleValue = floatTrace[i];
    sampleValue = std::max(sampleValue, valueRange.Min);
    sampleValue = std::min(sampleValue, valueRange.Max);

    intTrace[i] = static_cast<T>(round((sampleValue - valueRange.Min) / integerScale));
  }
}

// Searches for an ensemble within the given key range. Returns true if found.
//
bool
Find2DEnsembleInChunkRange(TraceInfo2DManager* pTraceInfo2DManager, int keyStart, int keyStop, int& ensembleIndex)
{
  // This ensemble number isn't present in the SEGY; try to find an ensemble number that's within the chunk's secondary key range
  const int step = keyStart <= keyStop ? 1 : -1;
  bool result = false;
  for (int searchKey = keyStart; searchKey != keyStop + step; searchKey += step)
  {
    ensembleIndex = pTraceInfo2DManager->GetIndexOfEnsembleNumber(searchKey);
    if (ensembleIndex >= 0)
    {
      result = true;
      break;
    }
  }

  return result;
}

std::string GetLegalTag(const std::vector<DataProvider>& dataProviders) {
  for (const auto& dataProvider : dataProviders) {
    if (dataProvider.m_ioManager && !dataProvider.m_ioManager->GetLegalTag().empty()) {
      return dataProvider.m_ioManager->GetLegalTag();
    }
  }
  return "";
}

#ifdef UNICODE_MAIN
int UNICODE_MAIN(int argc, char **argv)
#else
int
main(int argc, char* argv[])
#endif
{
#ifndef WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
  //auto start_time = std::chrono::high_resolution_clock::now();
  cxxopts::Options options("SEGYImport", "SEGYImport - A tool to scan and import a SEG-Y file to a volume data store (VDS)\n\nUse -H or see online documentation for connection string parameters:\nhttps://osdu.pages.opengroup.org/platform/domain-data-mgmt-services/seismic/open-vds/connection.html\n");
  options.positional_help("<input file>");

  std::string headerFormatFileName;
  std::vector<std::string> headerFields;
  std::string primaryKey;
  std::string secondaryKey;
  bool keepOriginalOrder = false;
  std::string sampleUnit;
  std::string crsWkt;
  double scale = 0;
  std::string overrideSampleFormatString;
  std::string overrideVDSFormatString;
  SEGY::BinaryHeader::DataSampleFormatCode overrideSampleFormat;
  bool overrideSampleStart = false;
  bool overrideBrickSize = false;
  bool overrideLODLevels = false;
  bool overrideCreate2DLODs = false;
  bool overrideMargin = false;
  double sampleStart = 0;
  bool littleEndian = false;
  bool scan = false;
  std::vector<std::string> fileInfoFileNames;
  int brickSize = 64;
  int LODLevels = 2;
  bool create2DLODs = false;
  int margin = 0;
  bool force = false;
  bool ignoreWarnings = false;
  std::string compressionMethodString;
  float compressionTolerance = 0;
  std::string url;
  std::string urlConnection;
  std::string inputConnection;
  std::string persistentID;
  bool singleConnection = false;
  bool uniqueID = false;
  bool disablePersistentID = false;
  bool prestack = false;
  bool is2D = false;
  bool is2DIndexAxis = false;
  bool isOffsetSorted = false;
  bool isOffsetSortedDupeKeyWarned = false;
  bool useJsonOutput = false;
  bool disablePrintSegyTextHeader = false;
  bool help = false;
  bool helpConnection = false;
  bool version = false;
  bool disableInfo = false;
  bool disableWarning = false;
  bool isMutes = false;
  bool isAzimuth = false;
  bool resumeMode = false;
  int flushFrequency = 60;
  std::string attributeName = AMPLITUDE_ATTRIBUTE_NAME;
  std::string attributeUnit;
  SEGY::AzimuthType azimuthType = SEGY::AzimuthType::Azimuth;
  SEGY::AzimuthUnits azimuthUnit = SEGY::AzimuthUnits::Degrees;
  float azimuthScaleFactor = 1.0f;
  std::string azimuthTypeString;
  std::string azimuthUnitString;

  TraceSpacingByOffset traceSpacingByOffset = TraceSpacingByOffset::Auto;
  std::string traceSpacingByOffsetString;
  bool isDisableCrosslineReordering = false;

  PrimaryKeyValue primaryKeyValue = PrimaryKeyValue::InlineNumber;

  const std::string supportedAzimuthTypes("Azimuth (from trace header field) (default), OffsetXY (computed from OffsetX and OffsetY header fields)");
  const std::string supportedAzimuthUnits("Radians, Degrees (default)");
  const std::string supportedTraceSpacingTypes("Off, On, Auto (default)");

  // default key names used if not supplied by user
  std::string defaultPrimaryKey = "InlineNumber";
  std::string defaultSecondaryKey = "CrosslineNumber";

  std::string segySurveyCoordinateSystemUnitString;
  std::string surveyCoordinateSystemUnitString;
  SurveyCoordinateSystemTransformer surveyCoordinateSystemTransformer;

  std::string supportedCompressionMethods = "None";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::Wavelet)) supportedCompressionMethods += ", Wavelet";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::RLE)) supportedCompressionMethods += ", RLE";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::Zip)) supportedCompressionMethods += ", Zip";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::WaveletNormalizeBlock)) supportedCompressionMethods += ", WaveletNormalizeBlock";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::WaveletLossless)) supportedCompressionMethods += ", WaveletLossless";
  if (OpenVDS::IsCompressionMethodSupported(OpenVDS::CompressionMethod::WaveletNormalizeBlockLossless)) supportedCompressionMethods += ", WaveletNormalizeBlockLossless";

  std::vector<std::string> fileNames;

  VDSFormatOverride vdsFormatOverride = VDSFormatOverride::None;

  std::string valueRangeString;
  OpenVDS::FloatRange userValueRange;

  std::string integerScaleString;
  OpenVDS::FloatRange userIntegerScaleOffset{};

  options.add_option("", "", "header-format", "A JSON file defining the header format for the input SEG-Y file. The expected format is a dictonary of strings (field names) to pairs (byte position, field width) where field width can be \"TwoByte\" or \"FourByte\". Additionally, an \"Endianness\" key can be specified as \"BigEndian\" or \"LittleEndian\".", cxxopts::value<std::string>(headerFormatFileName), "<file>");
  options.add_option("", "", "header-field", "A single definition of a header field. The expected format is a \"fieldname=offset:width\" where the \":width\" is optional. Its also possible to specify range: \"fieldname=begin-end\". Multiple header-fields is specified by providing multiple --header-field arguments.", cxxopts::value<std::vector<std::string>>(headerFields), "headerName=offset:width");
  options.add_option("", "p", "primary-key", "The name of the trace header field to use as the primary key.", cxxopts::value<std::string>(primaryKey), "<field>");
  options.add_option("", "s", "secondary-key", "The name of the trace header field to use as the secondary key.", cxxopts::value<std::string>(secondaryKey), "<field>");
  options.add_option("", "", "keep-original-order", "Do not reorder the data in the VDS if the primary key is sorted in descending order.", cxxopts::value<bool>(keepOriginalOrder), "");
  options.add_option("", "", "prestack", "Import binned prestack data (PSTM/PSDM gathers).", cxxopts::value<bool>(prestack), "");
  options.add_option("", "", "scale", "If a scale override (floating point) is given, it is used to scale the coordinates in the header instead of determining the scale factor from the coordinate scale trace header field.", cxxopts::value<double>(scale), "<value>");
  options.add_option("", "", "sample-unit", "A sample unit of 'ms' is used for datasets in the time domain (default), while a sample unit of 'm' or 'ft' is used for datasets in the depth domain", cxxopts::value<std::string>(sampleUnit), "<string>");
  options.add_option("", "", "sample-start", "The start time/depth/frequency (depending on the domain) of the sampling", cxxopts::value<double>(sampleStart), "<value>");
  options.add_option("", "", "sample-format", "Override the data format used when reading sample data from SEGY file. Possible values are: IBMFloat, IEEEFloat, UInt32, Int32, UInt16, Int16, UInt8, Int8.", cxxopts::value<std::string>(overrideSampleFormatString), "<string>");
  options.add_option("", "", "vds-format", "Override the data format used when writing sample data to the VDS. Possible values are: UInt16, UInt8. This option is only allowed when the SEGY sample format is IBMFloat or IEEEFloat.", cxxopts::value<std::string>(overrideVDSFormatString), "<string>");
  options.add_option("", "", "crs-wkt", "A coordinate reference system in well-known text format can optionally be provided", cxxopts::value<std::string>(crsWkt), "<string>");
  options.add_option("", "l", "little-endian", "Force little-endian trace headers.", cxxopts::value<bool>(littleEndian), "");
  options.add_option("", "", "scan", "Generate a JSON file containing information about the input SEG-Y file.", cxxopts::value<bool>(scan), "");
  options.add_option("", "i", "file-info", "A JSON file (generated by the --scan option) containing information about an input SEG-Y file.", cxxopts::value<std::vector<std::string>>(fileInfoFileNames), "<file>");
  options.add_option("", "b", "brick-size", "The brick size for the volume data store.", cxxopts::value<int>(brickSize), "<value>");
  options.add_option("", "", "lod-levels", "The number of LODs to generate.", cxxopts::value<int>(LODLevels), "<value>");
  options.add_option("", "", "create-2d-lods", "Create 2D LODs.", cxxopts::value<bool>(create2DLODs), "");
  options.add_option("", "", "margin", "The margin size (overlap) of the bricks.", cxxopts::value<int>(margin), "<value>");
  options.add_option("", "f", "force", "Continue on upload error.", cxxopts::value<bool>(force), "");
  options.add_option("", "", "ignore-warnings", "Ignore warnings about import parameters.", cxxopts::value<bool>(ignoreWarnings), "");
  options.add_option("", "", "compression-method", std::string("Compression method. Supported compression methods are: ") + supportedCompressionMethods + ".", cxxopts::value<std::string>(compressionMethodString), "<string>");
  options.add_option("", "", "tolerance", "This parameter specifies the compression tolerance when using the wavelet compression method. This value is the maximum deviation from the original data value when the data is converted to 8-bit using the value range. A value of 1 means the maximum allowable loss is the same as quantizing to 8-bit (but the average loss will be much much lower than quantizing to 8-bit). It is not a good idea to directly relate the tolerance to the quality of the compressed data, as the average loss will in general be an order of magnitude lower than the allowable loss.", cxxopts::value<float>(compressionTolerance), "<value>");
  options.add_option("", "", "url", "Url with cloud vendor scheme used for target location or file name of output VDS file.", cxxopts::value<std::string>(url), "<string>");
  options.add_option("", "", "url-connection", "Connection string used for additional parameters to the url connection", cxxopts::value<std::string>(urlConnection), "<string>");
  options.add_option("", "", "vdsfile", "File name of output VDS file.", cxxopts::value<std::string>(url), "<string>");
  options.add_option("", "", "single-connection", "Use single connection string. When specified url-connection will be used for input-connection as well", cxxopts::value<bool>(singleConnection), "");
  options.add_option("", "", "input-connection", "Connection string used for additional parameters to the input connection", cxxopts::value<std::string>(inputConnection), "<string>");
  options.add_option("", "", "persistentID", "A globally unique ID for the VDS, usually an 8-digit hexadecimal number.", cxxopts::value<std::string>(persistentID), "<ID>");
  options.add_option("", "", "uniqueID", "Generate a new globally unique ID when scanning the input SEG-Y file.", cxxopts::value<bool>(uniqueID), "");
  options.add_option("", "", "disable-persistentID", "Disable the persistentID usage, placing the VDS directly into the url location.", cxxopts::value<bool>(disablePersistentID), "");
  options.add_option("", "", "json-output", "Enable json output.", cxxopts::value<bool>(useJsonOutput), "");
  options.add_option("", "", "disable-print-text-header", "Disable printing the text header of the input segy file.", cxxopts::value<bool>(disablePrintSegyTextHeader), "");
  options.add_option("", "", "attribute-name", "The name of the primary VDS channel. The name may be Amplitude (default), Attribute, Depth, Probability, Time, Vavg, Vint, or Vrms", cxxopts::value<std::string>(attributeName)->default_value(AMPLITUDE_ATTRIBUTE_NAME), "<string>");
  options.add_option("", "", "attribute-unit", "The units of the primary VDS channel. The unit name may be blank (default), ft, ft/s, Hz, m, m/s, ms, or s", cxxopts::value<std::string>(attributeUnit), "<string>");
  options.add_option("", "", "value-range", "Set the sample value range by giving minimum and maximum values as a colon-separated pair. By default the value range will be calculated from SEGY data. Using this option will not change sample data; it will only affect the value range stored in the VDS header.", cxxopts::value<std::string>(valueRangeString), "<string>");
  options.add_option("", "", "integer-scale", "Set the scale and offset values used to convert 8/16-bit data to floating point by giving a colon-separated pair. By default this will be calculated from the sample value range. This option is only applicable for a VDS using UInt16 or UInt8 data formats.", cxxopts::value<std::string>(integerScaleString), "<string>");
  options.add_option("", "", "2d", "Import 2D data.", cxxopts::value<bool>(is2D), "");
  options.add_option("", "", "2d-index-axis", "When importing 2D SEGY label the primary axis using 1..N instead of CDP numbers .", cxxopts::value<bool>(is2DIndexAxis), "");
  options.add_option("", "", "offset-sorted", "Import prestack data sorted by trace header Offset value.", cxxopts::value<bool>(isOffsetSorted), "");
  options.add_option("", "", "mute", "Enable Mutes channel in output VDS.", cxxopts::value<bool>(isMutes), "");
  options.add_option("", "", "azimuth", "Enable Azimuth channel in output VDS.", cxxopts::value<bool>(isAzimuth), "");
  options.add_option("", "", "azimuth-type", std::string("Azimuth type. Supported azimuth types are: ") + supportedAzimuthTypes + ".", cxxopts::value<std::string>(azimuthTypeString), "<string>");
  options.add_option("", "", "azimuth-unit", std::string("Azimuth unit. Supported azimuth units are: ") + supportedAzimuthUnits + ".", cxxopts::value<std::string>(azimuthUnitString), "<string>");
  options.add_option("", "", "azimuth-scale", "Azimuth scale factor. Trace header field Azimuth values will be multiplied by this factor.", cxxopts::value<float>(azimuthScaleFactor), "<value>");
  options.add_option("", "", "respace-gathers", std::string("Respace traces in prestack gathers by Offset trace header field. Supported options are: ") + supportedTraceSpacingTypes + ".", cxxopts::value<std::string>(traceSpacingByOffsetString), "<string>");
  options.add_option("", "", "segy-survey-coordinate-system-unit", std::string("Ignore the unit defined in the SEG-Y file and use the defined unit instead. Possible values: Meters, Feet"), cxxopts::value<std::string>(segySurveyCoordinateSystemUnitString), "<string>");
  options.add_option("", "", "survey-coordinate-system-unit", std::string("Unit used in the VDS for survey coordinate system. Possible values: Meters (default), Feet, Original"), cxxopts::value<std::string>(surveyCoordinateSystemUnitString), "<string>");
  options.add_option("", "", "resume", std::string("Resume mode."), cxxopts::value<bool>(resumeMode), "");
  options.add_option("", "", "flush-frequency", std::string("Flush frequency in seconds. SEGYImport can resume imports at flush checkpoints. 0 (zero) results in never flushing. Default is 60."), cxxopts::value<int>(flushFrequency), "<value>");
  options.add_option("", "q", "quiet", "Disable info level output.", cxxopts::value<bool>(disableInfo), "");
  options.add_option("", "Q", "very-quiet", "Disable warning level output.", cxxopts::value<bool>(disableWarning), "");
  options.add_option("", "h", "help", "Print this help information.", cxxopts::value<bool>(help), "");
  options.add_option("", "H", "help-connection", "Print help information about the connection string.", cxxopts::value<bool>(helpConnection), "");
  options.add_option("", "", "version", "Print version information.", cxxopts::value<bool>(version), "");

  options.add_option("", "", "input", "", cxxopts::value<std::vector<std::string>>(fileNames), "");
  options.parse_positional("input");

  OpenVDS::OutputPrinter outputPrinter(useJsonOutput, OpenVDS::OutputPrinter::getLogLevel(disableWarning, disableInfo));
  if (argc == 1)
  {
    outputPrinter.printInfo("Args", options.help());
    return EXIT_SUCCESS;
  }

  try
  {
    auto result = options.parse(argc, argv);
    overrideSampleStart = result.count("sample-start") != 0;
    overrideBrickSize = result.count("brick-size") != 0;
    overrideLODLevels = result.count("lod-levels") != 0;
    overrideCreate2DLODs = result.count("create-2d-lods") != 0;
    overrideMargin = result.count("margin") != 0;
  }
  catch (cxxopts::OptionParseException& e)
  {
    outputPrinter.printError("Args", e.what());
    return EXIT_FAILURE;
  }

  if (!fileInfoFileNames.empty() && fileInfoFileNames.size() != fileNames.size())
  {
    outputPrinter.printError("Args", "Different number of input SEG-Y file names and file-info file names. Use multiple --file-info arguments to specify a file-info file for each input SEG-Y file.");
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
    outputPrinter.printVersion("SEGYImport");
    return EXIT_SUCCESS;
  }

  {
    std::string originalSegySurveyCoordinatesystemUnitString = segySurveyCoordinateSystemUnitString;
    std::string originalSurveyCoordinatesystemUnitString = surveyCoordinateSystemUnitString;
    std::transform(segySurveyCoordinateSystemUnitString.begin(), segySurveyCoordinateSystemUnitString.end(), segySurveyCoordinateSystemUnitString.begin(), asciitolower);
    std::transform(surveyCoordinateSystemUnitString.begin(), surveyCoordinateSystemUnitString.end(), surveyCoordinateSystemUnitString.begin(), asciitolower);
    if (!surveyCoordinateSystemTransformer.assertSegyUnitName(segySurveyCoordinateSystemUnitString))
    {
      outputPrinter.printError("Args", fmt::format("Invalid segy-survey-coordinate-system specified: {}", originalSegySurveyCoordinatesystemUnitString));
      return EXIT_FAILURE;
    }
    if (!surveyCoordinateSystemTransformer.assertVDSUnitName(surveyCoordinateSystemUnitString))
    {
      outputPrinter.printError("Args", fmt::format("Invalid survey-coordinate-system specified: {}", originalSurveyCoordinatesystemUnitString));
      return EXIT_FAILURE;
    }
  }

  // set up the compression method
  OpenVDS::CompressionMethod compressionMethod = OpenVDS::CompressionMethod::None;

  std::transform(compressionMethodString.begin(), compressionMethodString.end(), compressionMethodString.begin(), asciitolower);

  if (compressionMethodString.empty()) compressionMethod = OpenVDS::CompressionMethod::None;
  else if (compressionMethodString == "none")                          compressionMethod = OpenVDS::CompressionMethod::None;
  else if (compressionMethodString == "wavelet")                       compressionMethod = OpenVDS::CompressionMethod::Wavelet;
  else if (compressionMethodString == "rle")                           compressionMethod = OpenVDS::CompressionMethod::RLE;
  else if (compressionMethodString == "zip")                           compressionMethod = OpenVDS::CompressionMethod::Zip;
  else if (compressionMethodString == "waveletnormalizeblock")         compressionMethod = OpenVDS::CompressionMethod::WaveletNormalizeBlock;
  else if (compressionMethodString == "waveletlossless")               compressionMethod = OpenVDS::CompressionMethod::WaveletLossless;
  else if (compressionMethodString == "waveletnormalizeblocklossless") compressionMethod = OpenVDS::CompressionMethod::WaveletNormalizeBlockLossless;
  else
  {
    outputPrinter.printError("CompressionMethod", "Unknown compression method", compressionMethodString);
    return EXIT_FAILURE;
  }

  if (!OpenVDS::IsCompressionMethodSupported(compressionMethod))
  {
    outputPrinter.printError("CompressionMethod", "Unsupported compression method", compressionMethodString);
    return EXIT_FAILURE;
  }

  if ((compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlock || compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlockLossless) && !ignoreWarnings)
  {
    outputPrinter.printError("CompressionMethod", fmt::format("The {} compression method will lead to more blocking artifacts and lower performance and is not recommmended to use.\nUse --ignore-warnings to force the import to go ahead.", (compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlock)? "WaveletNormalizeBlock" : "WaveletNormalizeBlockLossless"));
    return EXIT_FAILURE;
  }

  if (overrideSampleFormatString.size() && !SEGY::DataSampleFormatCodeFromString(overrideSampleFormatString.c_str(), overrideSampleFormat))
  {
    outputPrinter.printError("Override sample format", fmt::format("Unable to recognize input data sample format override: {}", overrideSampleFormatString));
    return EXIT_FAILURE;
  }

  if (!overrideVDSFormatString.empty())
  {
    SEGY::BinaryHeader::DataSampleFormatCode formatCode;
    if (!SEGY::DataSampleFormatCodeFromString(overrideVDSFormatString.c_str(), formatCode))
    {
      outputPrinter.printError("Override VDS format", fmt::format("Unable to recognize VDS data sample format override: {}", overrideVDSFormatString));
      return EXIT_FAILURE;
    }
    if (formatCode == SEGY::BinaryHeader::DataSampleFormatCode::UInt8)
    {
      vdsFormatOverride = VDSFormatOverride::Override8Bit;
    }
    else if (formatCode == SEGY::BinaryHeader::DataSampleFormatCode::UInt16)
    {
      vdsFormatOverride = VDSFormatOverride::Override16Bit;
    }
    else
    {
      outputPrinter.printError("Override VDS format", fmt::format("VDS data sample format override may be only UInt16 or UInt8. {} is not allowed.", overrideVDSFormatString));
      return EXIT_FAILURE;
    }
  }

  if (!valueRangeString.empty())
  {
    OpenVDS::Error error;
    userValueRange = ColonPairToFloatRange(valueRangeString, error);
    if (error.code != 0)
    {
      outputPrinter.printError("Set sample value range", fmt::format("Sample value range error: {}", error.string));
      return EXIT_FAILURE;
    }

    if (userValueRange.Min > userValueRange.Max)
    {
      outputPrinter.printError("Set sample value range", fmt::format("Sample value range minimum must be less than maximum. Minimum: {}  Maximum: {}", userValueRange.Min, userValueRange.Max));
      return EXIT_FAILURE;
    }
  }

  if (!integerScaleString.empty())
  {
    OpenVDS::Error error;
    userIntegerScaleOffset = ColonPairToFloatRange(integerScaleString, error);
    if (error.code != 0)
    {
      outputPrinter.printError("Set integer scale", fmt::format("Integer scale error: {}", error.string));
      return EXIT_FAILURE;
    }
  }

  if (compressionMethod == OpenVDS::CompressionMethod::Wavelet || compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlock || compressionMethod == OpenVDS::CompressionMethod::WaveletLossless || compressionMethod == OpenVDS::CompressionMethod::WaveletNormalizeBlockLossless)
  {
    if (!overrideBrickSize)
    {
      brickSize = 128;
    }
    if (!overrideMargin)
    {
      margin = 4;
    }
  }

  if (is2D)
  {
    defaultSecondaryKey = "EnsembleNumber";
  }

  if (primaryKey.empty())
  {
    primaryKey = defaultPrimaryKey;
  }
  if (secondaryKey.empty())
  {
    secondaryKey = defaultSecondaryKey;
  }

  // get the canonical field name for the primary and secondary key
  ResolveAlias(primaryKey);
  ResolveAlias(secondaryKey);

  // set the enum value for primary key
  if (primaryKey == "inlinenumber")
  {
    primaryKeyValue = PrimaryKeyValue::InlineNumber;
  }
  else if (primaryKey == "crosslinenumber")
  {
    primaryKeyValue = PrimaryKeyValue::CrosslineNumber;
  }
  else
  {
    primaryKeyValue = PrimaryKeyValue::Other;
  }

  SEGY::SEGYType segyType = SEGY::SEGYType::Poststack;

  if (isOffsetSorted)
  {
    segyType = SEGY::SEGYType::PrestackOffsetSorted;
  }
  else if (is2D)
  {
    if (prestack)
    {
      segyType = SEGY::SEGYType::Prestack2D;
    }
    else
    {
      segyType = SEGY::SEGYType::Poststack2D;
    }
  }
  else if (primaryKey == "inlinenumber" || primaryKey == "crosslinenumber")
  {
    if (prestack)
    {
      segyType = SEGY::SEGYType::Prestack;
    }
    else
    {
      segyType = SEGY::SEGYType::Poststack;
    }
  }
  else if (primaryKey == "ensemblenumber")
  {
    segyType = SEGY::SEGYType::CDPGathers;
  }
  else if (primaryKey == "receiver")
  {
    segyType = SEGY::SEGYType::ReceiverGathers;
  }
  else if (primaryKey == "energysourcepointnumber")
  {
    segyType = SEGY::SEGYType::ShotGathers;
  }
  else
  {
    outputPrinter.printError("SEGY", "Primary key does not match a known SEG-Y type");
    return EXIT_FAILURE;
  }

  if(segyType == SEGY::SEGYType::Poststack && !overrideLODLevels)
  {
    LODLevels = 4;
  }
  else if(segyType == SEGY::SEGYType::Poststack2D && !overrideCreate2DLODs)
  {
    create2DLODs = true;
  }

  if (fileNames.empty())
  {
    outputPrinter.printError("SEGY", "No input SEG-Y file specified");
    return EXIT_FAILURE;
  }

  if (fileNames.size() > 1 && segyType != SEGY::SEGYType::Prestack)
  {
    outputPrinter.printError("SEGY", "Only one input SEG-Y file may be specified");
    return EXIT_FAILURE;
  }

  if (singleConnection && inputConnection.size())
  {
    outputPrinter.printError("Args", "Both --single-connection and --input-connection specified.");
      return EXIT_FAILURE;
  }
  if (singleConnection)
    inputConnection = urlConnection;

  if (uniqueID && !persistentID.empty())
  {
    outputPrinter.printError("Args", "--uniqueID does not make sense when the persistentID is specified");
    return EXIT_FAILURE;
  }

  if (disablePersistentID && !persistentID.empty())
  {
    outputPrinter.printError("Args", "--disable-PersistentID does not make sense when the persistentID is specified");
    return EXIT_FAILURE;
  }

  SEGY::Endianness headerEndianness = (littleEndian ? SEGY::Endianness::LittleEndian : SEGY::Endianness::BigEndian);

  if (!headerFormatFileName.empty())
  {
    OpenVDS::Error
      error;
    DataProvider headerFormatDataProvider = CreateDataProvider(headerFormatFileName, inputConnection, error);

    if (error.code != 0)
    {
      outputPrinter.printError("File", "Could not open header format file", headerFormatFileName);
      return EXIT_FAILURE;
    }

    ParseHeaderFormatFile(headerFormatDataProvider, g_traceHeaderFields, headerEndianness, error);

    if (error.code != 0)
    {
      outputPrinter.printError("File", "Could not read header format file", headerFormatFileName, error.string);
      return EXIT_FAILURE;
    }
  }

  if (headerFields.size())
  {
    OpenVDS::Error error;
    if (!ParseHeaderFieldArgs(headerFields, g_traceHeaderFields, headerEndianness, error))
    {
      outputPrinter.printError("HeaderFields", "Could not parse header-fields", error.string);
      return EXIT_FAILURE;
    }
  }

  if (!azimuthTypeString.empty())
  {
    std::transform(azimuthTypeString.begin(), azimuthTypeString.end(), azimuthTypeString.begin(), asciitolower);
    if (azimuthTypeString == "azimuth")
    {
      azimuthType = SEGY::AzimuthType::Azimuth;
    }
    else if (azimuthTypeString == "offsetxy")
    {
      azimuthType = SEGY::AzimuthType::OffsetXY;
    }
    else
    {
      outputPrinter.printError("Args", fmt::format("Unknown Azimuth type '{}'", azimuthTypeString));
      return EXIT_FAILURE;
    }
  }

  if (!azimuthUnitString.empty())
  {
    std::transform(azimuthUnitString.begin(), azimuthUnitString.end(), azimuthUnitString.begin(), asciitolower);
    if (azimuthUnitString == "radians")
    {
      azimuthUnit = SEGY::AzimuthUnits::Radians;
    }
    else if (azimuthUnitString == "degrees")
    {
      azimuthUnit = SEGY::AzimuthUnits::Degrees;
    }
    else
    {
      outputPrinter.printError("Args", fmt::format("Unknown Azimuth unit '{}'", azimuthUnitString));
      return EXIT_FAILURE;
    }
  }

  if (!traceSpacingByOffsetString.empty())
  {
    std::transform(traceSpacingByOffsetString.begin(), traceSpacingByOffsetString.end(), traceSpacingByOffsetString.begin(), asciitolower);
    if (traceSpacingByOffsetString == "off")
    {
      traceSpacingByOffset = TraceSpacingByOffset::Off;
    }
    else if (traceSpacingByOffsetString == "on")
    {
      traceSpacingByOffset = TraceSpacingByOffset::On;
    }
    else if (traceSpacingByOffsetString == "auto")
    {
      traceSpacingByOffset = TraceSpacingByOffset::Auto;
    }
    else
    {
      outputPrinter.printError("Args", fmt::format("Unknown --respace-gathers option '{}'", traceSpacingByOffsetString));
      return EXIT_FAILURE;
    }
  }

  SEGY::HeaderField
    primaryKeyHeaderField,
    secondaryKeyHeaderField;

  if (g_traceHeaderFields.find(primaryKey) != g_traceHeaderFields.end())
  {
    primaryKeyHeaderField = g_traceHeaderFields[primaryKey];
  }
  else
  {
    outputPrinter.printError("HeaderFields", "Unrecognized header field given for primary key", primaryKey);
    return EXIT_FAILURE;
  }

  if (g_traceHeaderFields.find(secondaryKey) != g_traceHeaderFields.end())
  {
    secondaryKeyHeaderField = g_traceHeaderFields[secondaryKey];
  }

  SEGY::HeaderField
    startTimeHeaderField = g_traceHeaderFields["starttime"],
    offsetHeaderField = g_traceHeaderFields["offset"];

  OpenVDS::Error
    error;
  std::string errorFileName;
  auto dataProviders = CreateDataProviders(fileNames, inputConnection, error, errorFileName);
  if (error.code != 0)
  {
    // TODO need to name which file failed to open
    outputPrinter.printError("IO", "Could not open input file", errorFileName, error.string);
    return EXIT_FAILURE;
  }

  outputPrinter.printVersion("SEGYImport");
  outputPrinter.printNewLine(OpenVDS::LogLevel::Info);

  if (!disablePrintSegyTextHeader)
  {
    auto& dataProvider = dataProviders.front();
    std::unique_ptr<uint8_t[]> inputData(new uint8_t[3200]);

    if (!dataProvider.Read(inputData.get(), 0, 3200, error))
    {
      outputPrinter.printError("IO", "Could not read SEGY Text header", errorFileName, error.string);
      return EXIT_FAILURE;
    }
    std::string output;
    output.resize(3250);
    if (SEGY::autoDetectSEGYTextHeaderIsEBCDIC(inputData.get(), 3200))
    {
      auto outputSize = SEGY::convertSEGYEBCDICHeaderToASCII(inputData.get(), 3200, &output[0], output.size());
      output.resize(outputSize);
    }
    else
    {
      output.resize(3200);
      memcpy(&output[0], inputData.get(), 3200);
    }
    outputPrinter.printInfo("SEGY text header", output);

    if (url.empty() && !scan)
      return EXIT_SUCCESS;
  
    outputPrinter.printNewLine(OpenVDS::LogLevel::Info);
  }

  SEGYFileInfo
    fileInfo(headerEndianness);
  fileInfo.m_segyType = segyType;

  const auto
    binInfoCrosslineFieldIndex = fileInfo.Is2D() ? secondaryKey.c_str() : "crosslinenumber";  // for 2D need to borrow the BinInfo crossline field for ensemble number
  auto
    xcoordHeaderFieldIndex = "ensemblexcoordinate",
    ycoordHeaderFieldIndex = "ensembleycoordinate";
  if (segyType == SEGY::SEGYType::ShotGathers)
  {
    xcoordHeaderFieldIndex = "sourcexcoordinate",
    ycoordHeaderFieldIndex = "sourceycoordinate";
  }
  else if (segyType == SEGY::SEGYType::ReceiverGathers)
  {
    xcoordHeaderFieldIndex = "groupxcoordinate";
    ycoordHeaderFieldIndex = "groupycoordinate";
  }

  SEGYBinInfoHeaderFields
    binInfoHeaderFields(g_traceHeaderFields["inlinenumber"], g_traceHeaderFields[binInfoCrosslineFieldIndex], g_traceHeaderFields["coordinatescale"], g_traceHeaderFields[xcoordHeaderFieldIndex], g_traceHeaderFields[ycoordHeaderFieldIndex], scale);

  // Scan the file if '--scan' was passed or we're uploading but no fileInfo file was specified
  if (scan || fileInfoFileNames.empty())
  {
    if (!uniqueID)
    {
      OpenVDS::HashCombiner
        hash;

      for (std::string const& fileName : fileNames)
      {
        hash.Add(fileName);
      }

      fileInfo.m_persistentID = OpenVDS::HashCombiner(hash);
    }

    bool success = fileInfo.Scan(dataProviders, error, primaryKeyHeaderField, secondaryKeyHeaderField, startTimeHeaderField, offsetHeaderField, binInfoHeaderFields);

    if (!success)
    {
      outputPrinter.printError("File", "Failed to scan file", fileNames[0], error.string);
      return EXIT_FAILURE;
    }

    if (overrideSampleStart)
    {
      fileInfo.m_startTimeMilliseconds = sampleStart;
    }

    if (overrideSampleFormatString.size())
    {
      fileInfo.m_dataSampleFormatCode = overrideSampleFormat;
    }

    if (fileInfo.m_detectedScale > 1 && scale == 0.0)
    {
      double scaleUsed = SEGYFileInfo::convertScaleToScaleFactor(fileInfo.m_detectedScale);
      double proposedScale = SEGYFileInfo::convertScaleToScaleFactor(-fileInfo.m_detectedScale);
      std::string warning = fmt::format("Detected scale factor: {} larger than 1.0 in SEG-Y. Usually this is an error. Instead try and add the argument: --scale {}", scaleUsed, proposedScale);
      outputPrinter.printWarning("ScaleFactor", warning);
    }
    // If we are in scan mode we serialize the result of the file scan either to a fileInfo file (if specified) or to stdout and exit
    if (scan)
    {
      for (size_t fileIndex = 0; fileIndex < fileNames.size(); ++fileIndex)
      {
        Json::Value jsonFileInfo = SerializeSEGYFileInfo(fileInfo, fileIndex);

        Json::StreamWriterBuilder wbuilder;
        wbuilder["indentation"] = "  ";
        std::string document = Json::writeString(wbuilder, jsonFileInfo);

        if (fileInfoFileNames.empty())
        {
          fmt::print(stdout, "{}", document);
        }
        else
        {
          OpenVDS::Error
            error;
          const auto
            & fileInfoFileName = fileInfoFileNames[fileIndex];

          if (OpenVDS::IsSupportedProtocol(fileInfoFileName))
          {
            std::string dirname;
            std::string basename;
            std::string parameters;
            splitUrl(fileInfoFileName, dirname, basename, parameters, error);
            if (error.code)
            {
              outputPrinter.printError("IO", "Failed to creating IOManager for", fileInfoFileName, error.string);
              return EXIT_FAILURE;
            }
            std::string scanUrl = dirname + parameters;
            std::unique_ptr<OpenVDS::IOManager> ioManager(OpenVDS::IOManager::CreateIOManager(scanUrl, urlConnection, OpenVDS::IOManager::ReadWrite, error));
            if (error.code)
            {
              outputPrinter.printError("IO", "Failed to creating IOManager for", fileInfoFileName, error.string);
              return EXIT_FAILURE;
            }
            auto shared_data = std::make_shared<std::vector<uint8_t>>();
            shared_data->insert(shared_data->end(), document.begin(), document.end());
            auto req = ioManager->WriteObject(basename, "", "text/plain", {}, shared_data, {});
            req->WaitForFinish(error);
            if (error.code)
            {
              outputPrinter.printError("IO", "Failed to write", fileInfoFileName, error.string);
              return EXIT_FAILURE;
            }
          }
          else
          {
            OpenVDS::File
              fileInfoFile;

            fileInfoFile.Open(fileInfoFileName, true, false, true, error);

            if (error.code != 0)
            {
              outputPrinter.printError("IO", "Could not create file info file", fileInfoFileName);
              return EXIT_FAILURE;
            }

            fileInfoFile.Write(document.data(), 0, (int32_t)document.size(), error);

            if (error.code != 0)
            {
              outputPrinter.printError("IO", "Could not write file info to file", fileInfoFileName);
              return EXIT_FAILURE;
            }
          }
        }
      }
      return EXIT_SUCCESS;
    }
  }
  else if (!fileInfoFileNames.empty())
  {
    OpenVDS::Error
      error;

    for (size_t fileIndex = 0; fileIndex < fileInfoFileNames.size(); ++fileIndex)
    {
      const auto
        & fileInfoFileName = fileInfoFileNames[fileIndex];

      DataProvider fileInfoDataProvider = CreateDataProvider(fileInfoFileName, inputConnection, error);

      if (error.code != 0)
      {
        outputPrinter.printError("IO", "Could not create data provider for", fileInfoFileName, error.string);
        return EXIT_FAILURE;
      }

      bool success = parseSEGYFileInfoFile(fileInfoDataProvider, fileInfo, fileIndex, fileIndex != 0, error);

      if (!success)
      {
        outputPrinter.printError("FileInfo", "Parse SEGYFileInfo", fileInfoFileName, error.string);
        return EXIT_FAILURE;
      }
    }

    if (overrideSampleStart)
    {
      fileInfo.m_startTimeMilliseconds = sampleStart;
    }
    if (overrideSampleFormatString.size())
    {
      fileInfo.m_dataSampleFormatCode = overrideSampleFormat;
    }
  }
  else
  {
    outputPrinter.printError("IO", "No SEG-Y file info file specified");
    return EXIT_FAILURE;
  }

  if (vdsFormatOverride != VDSFormatOverride::None)
  {
    if (fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat && fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat)
    {
      outputPrinter.printError("Override VDS format", fmt::format("VDS data sample format override may be used only when the SEGY data sample format is IBMFloat or IEEEFloat. SEGY format is {}.",
        SEGY::DataSampleFormatCodeToString(fileInfo.m_dataSampleFormatCode)));
      return EXIT_FAILURE;
    }
  }

  if (!integerScaleString.empty())
  {
    // Only allow integer scale option when the VDS will be U8/U16
    if (vdsFormatOverride == VDSFormatOverride::None &&
      fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::Int8 && fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::UInt8 &&
      fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::Int16 && fileInfo.m_dataSampleFormatCode != SEGY::BinaryHeader::DataSampleFormatCode::UInt16)
    {
      outputPrinter.printError("Set integer scale", fmt::format("The integer scale option is only allowed when the SEGY data sample format is 8-bit or 16-bit, or when the VDS sample data format option is used. SEGY format is {}.",
        SEGY::DataSampleFormatCodeToString(fileInfo.m_dataSampleFormatCode)));
      return EXIT_FAILURE;
    }
  }

  if (persistentID.empty() && !disablePersistentID)
  {
    persistentID = fmt::format("{:X}", fileInfo.m_persistentID);
  }

  // Check for only a single segment

  const bool isOneSegment = fileInfo.IsOffsetSorted()
    ? fileInfo.m_segmentInfoListsByOffset.size() == 1 && fileInfo.m_segmentInfoListsByOffset[0].size() == 1
    : fileInfo.m_segmentInfoLists.size() == 1 && fileInfo.m_segmentInfoLists[0].size() == 1;

  if (!is2D && isOneSegment && !ignoreWarnings)
  {
    outputPrinter.printError("SegmentInfoList", "Warning: There is only one segment, either this is 2D data or this usually indicates using the wrong header format for the input dataset.\nUse --2d for 2D data. Use --ignore-warnings to force the import to go ahead.");
    return EXIT_FAILURE;
  }

  // Determine value range, fold and primary/secondary step

  OpenVDS::FloatRange valueRange;
  int fold = 1, primaryStep = 1, secondaryStep = 1;
  std::vector<int> gatherOffsetValues;

  const float valueRangePercentile = 99.5f; // 99.5f is the same default as Petrel uses.

  // Create the appropriate TraceInfo2DManager here so that it can be called during AnalzyeSegment.
    //
    // Note: Piggybacking the gathering of 2D trace info onto AnalyzeSegment is possible because
    // a) 2D SEGY has exactly one segment, and b) AnalyzeSegment reads every trace in the segment its
    // analyzing. If (b) is no longer true because we've changed the analysis strategy then something
    // will need to take the place of this piggybacking.
    //
  // default header fields to get X/Y coordinates for trace positions
  const char
    * xCoordFieldName2D = "ensemblexcoordinate",
    * yCoordFieldName2D = "ensembleycoordinate";
  if (segyType == SEGY::SEGYType::ReceiverGathers)
  {
    xCoordFieldName2D = "groupxcoordinate";
    yCoordFieldName2D = "groupycoordinate";
  }
  else if (segyType == SEGY::SEGYType::ShotGathers)
  {
    xCoordFieldName2D = "sourcexcoordinate";
    yCoordFieldName2D = "sourceycoordinate";
  }
  auto traceInfo2DManager = fileInfo.Is2D()
    ? std::unique_ptr<TraceInfo2DManager>(new TraceInfo2DManagerImpl(fileInfo.m_headerEndianness, scale, g_traceHeaderFields["coordinatescale"], g_traceHeaderFields[xCoordFieldName2D], g_traceHeaderFields[yCoordFieldName2D], g_traceHeaderFields["starttime"], g_traceHeaderFields["energysourcepointnumber"], secondaryKeyHeaderField))
    : std::unique_ptr<TraceInfo2DManager>(new TraceInfo2DManager());

  bool
    analyzeResult = false;
  if (fileInfo.IsOffsetSorted())
  {
    const auto
      primaryKey = findRepresentativePrimaryKey(fileInfo, primaryStep);
    switch (fileInfo.m_dataSampleFormatCode)
    {
    case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
    case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
      analyzeResult = analyzePrimaryKey<float>(dataProviders, fileInfo, primaryKey, valueRangePercentile, valueRange, fold, secondaryStep, gatherOffsetValues, outputPrinter, error);
      break;
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
      analyzeResult = analyzePrimaryKey<uint16_t>(dataProviders, fileInfo, primaryKey, valueRangePercentile, valueRange, fold, secondaryStep, gatherOffsetValues, outputPrinter, error);
      break;
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
      analyzeResult = analyzePrimaryKey<uint8_t>(dataProviders, fileInfo, primaryKey, valueRangePercentile, valueRange, fold, secondaryStep, gatherOffsetValues, outputPrinter, error);
      break;
    default:
      error.code = -1;
      error.string = fmt::format("Unknown input format {}.", SEGY::DataSampleFormatCodeToString(fileInfo.m_dataSampleFormatCode));
    }
  }
  else
  {
    int fileIndex;
    const auto& representativeSegment = findRepresentativeSegment(fileInfo, primaryStep, fileIndex);
    assert(fileIndex < int(dataProviders.size()));
    switch (fileInfo.m_dataSampleFormatCode)
    {
    case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
    case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
      analyzeResult = analyzeSegment<float>(dataProviders[fileIndex], fileInfo, representativeSegment, valueRangePercentile, valueRange, fold, secondaryStep, segyType, traceInfo2DManager.get(), traceSpacingByOffset, gatherOffsetValues, outputPrinter, error);
      break;
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
      analyzeResult = analyzeSegment<uint16_t>(dataProviders[fileIndex], fileInfo, representativeSegment, valueRangePercentile, valueRange, fold, secondaryStep, segyType, traceInfo2DManager.get(), traceSpacingByOffset, gatherOffsetValues, outputPrinter, error);
      break;
    case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
    case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
      analyzeResult = analyzeSegment<uint8_t>(dataProviders[fileIndex], fileInfo, representativeSegment, valueRangePercentile, valueRange, fold, secondaryStep, segyType, traceInfo2DManager.get(), traceSpacingByOffset, gatherOffsetValues, outputPrinter, error);
      break;
    default:
      error.code = -1;
      error.string = fmt::format("Unknown input format {}.", SEGY::DataSampleFormatCodeToString(fileInfo.m_dataSampleFormatCode));
    }
  }

  if (!analyzeResult || error.code != 0)
  {
    outputPrinter.printError("SEGY", error.string);
    return EXIT_FAILURE;
  }

  if (fileInfo.IsUnbinned())
  {
    // For unbinned data we need to build the trace info based on segment info.
    traceInfo2DManager = std::unique_ptr<TraceInfo2DManager>(new TraceInfo2DManagerImpl(fileInfo.m_headerEndianness, scale, g_traceHeaderFields["coordinatescale"], g_traceHeaderFields[xCoordFieldName2D], g_traceHeaderFields[yCoordFieldName2D], g_traceHeaderFields["starttime"], g_traceHeaderFields["energysourcepointnumber"], secondaryKeyHeaderField));
    buildUnbinned2DTraceInfo(traceInfo2DManager.get(), fileInfo);
  }

  if (!valueRangeString.empty())
  {
    // Override the computed value range with the value range given by the user
    valueRange = userValueRange;
  }

  double overrideScale = 1.0;
  if (vdsFormatOverride == VDSFormatOverride::Override16Bit)
  {
    overrideScale = (valueRange.Max - valueRange.Min) / 65535.0;
  }
  else if (vdsFormatOverride == VDSFormatOverride::Override8Bit)
  {
    overrideScale = (valueRange.Max - valueRange.Min) / 255.0;
  }

  if (IsSEGYTypeUnbinned(segyType))
  {
    // For unbinned data the segments are the gathers, so the fold is the number of traces in the longest segment
    assert(fold == 1 && "analyzeSegment should report a fold of 1 for unbinned types");
    // If we ever want to print the fold for diagnostic purposes we should loop through the segments and determine the actual fold here
  }
  else if (segyType == SEGY::SEGYType::Poststack || segyType == SEGY::SEGYType::Poststack2D)
  {
    if (fold > 1)
    {
      outputPrinter.printError("SEGY", fmt::format("Detected a fold of '{0}', this usually indicates using the wrong header format or primary key for the input dataset or that the input data is binned prestack data (PSTM/PSDM gathers) in which case the --prestack option should be used.", fold));
      return EXIT_FAILURE;
    }
  }
  else
  {
    if (fold <= 1)
    {
      outputPrinter.printError("SEGY", fmt::format("Detected a fold of '{0}', this usually indicates using the wrong header format or primary key for the input dataset or that the input data is poststack in which case the --prestack option should not been used.", fold));
      return EXIT_FAILURE;
    }
  }

  // Create layout descriptor
  enum OpenVDS::VolumeDataLayoutDescriptor::BrickSize
    brickSizeEnum;

  switch (brickSize)
  {
  case 32: brickSizeEnum = OpenVDS::VolumeDataLayoutDescriptor::BrickSize_32; break;
  case 64: brickSizeEnum = OpenVDS::VolumeDataLayoutDescriptor::BrickSize_64; break;
  case 128: brickSizeEnum = OpenVDS::VolumeDataLayoutDescriptor::BrickSize_128; break;
  case 256: brickSizeEnum = OpenVDS::VolumeDataLayoutDescriptor::BrickSize_256; break;
  default:
    outputPrinter.printError("Args", "Illegal brick size (must be 32, 64, 128 or 256)");
    return EXIT_FAILURE;
  }

  if(LODLevels < int(OpenVDS::VolumeDataLayoutDescriptor::LODLevels_None) || LODLevels > int(OpenVDS::VolumeDataLayoutDescriptor::LODLevels_12))
  {
    outputPrinter.printError("Args", "Illegal number of LOD levels (max is 12)");
    return EXIT_FAILURE;
  }

  OpenVDS::VolumeDataLayoutDescriptor::Options layoutOptions = create2DLODs ? OpenVDS::VolumeDataLayoutDescriptor::Options_Create2DLODs : OpenVDS::VolumeDataLayoutDescriptor::Options_None;

  const int negativeMargin = margin;
  const int positiveMargin = margin;
  const int brickSize2DMultiplier = 4;

  OpenVDS::VolumeDataLayoutDescriptor layoutDescriptor(brickSizeEnum, negativeMargin, positiveMargin, brickSize2DMultiplier, OpenVDS::VolumeDataLayoutDescriptor::LODLevels(LODLevels), layoutOptions);

  SEGY::SampleUnits
    sampleUnits;

  if (sampleUnit.empty() || sampleUnit == "ms" || sampleUnit == "millisecond" || sampleUnit == "milliseconds")
  {
    sampleUnits = SEGY::SampleUnits::Milliseconds;
  }
  else if (sampleUnit == "m" || sampleUnit == "meter" || sampleUnit == "meters")
  {
    sampleUnits = SEGY::SampleUnits::Meters;
  }
  else if (sampleUnit == "ft" || sampleUnit == "foot" || sampleUnit == "feet")
  {
    sampleUnits = SEGY::SampleUnits::Feet;
  }
  else
  {
    outputPrinter.printError("Args", "Unknown sample unit: {}, legal units are 'ms', 'm' or 'ft'\n", sampleUnit);
    return EXIT_FAILURE;
  }

  if (!(attributeName == DEFAULT_ATTRIBUTE_NAME || attributeName == AMPLITUDE_ATTRIBUTE_NAME || attributeName == DEPTH_ATTRIBUTE_NAME
    || attributeName == PROBABILITY_ATTRIBUTE_NAME || attributeName == TIME_ATTRIBUTE_NAME || attributeName == AVERAGE_VELOCITY_ATTRIBUTE_NAME
    || attributeName == INTERVAL_VELOCITY_ATTRIBUTE_NAME || attributeName == RMS_VELOCITY_ATTRIBUTE_NAME))
  {
    const auto msg = fmt::format("Unknown attribute name: {}, legal names are '{}', '{}', '{}', '{}', '{}', '{}', '{}', or '{}'\n",
      attributeName, DEFAULT_ATTRIBUTE_NAME, AMPLITUDE_ATTRIBUTE_NAME, DEPTH_ATTRIBUTE_NAME, PROBABILITY_ATTRIBUTE_NAME, TIME_ATTRIBUTE_NAME, AVERAGE_VELOCITY_ATTRIBUTE_NAME,
      INTERVAL_VELOCITY_ATTRIBUTE_NAME, RMS_VELOCITY_ATTRIBUTE_NAME);
    outputPrinter.printError("Args", msg);
    return EXIT_FAILURE;
  }

  if (!(attributeUnit.empty() || attributeUnit == KNOWNMETADATA_UNIT_FOOT || attributeUnit == KNOWNMETADATA_UNIT_FEET_PER_SECOND
    || attributeUnit == "Hz" || attributeUnit == KNOWNMETADATA_UNIT_METER || attributeUnit == KNOWNMETADATA_UNIT_METERS_PER_SECOND
    || attributeUnit == KNOWNMETADATA_UNIT_MILLISECOND || attributeUnit == KNOWNMETADATA_UNIT_SECOND))
  {
    const auto msg = fmt::format("Unknown attribute unit: {}, legal units are blank (no units), '{}', '{}', '{}', '{}', '{}', '{}', or '{}'\n",
      attributeUnit, KNOWNMETADATA_UNIT_FOOT, KNOWNMETADATA_UNIT_FEET_PER_SECOND, "Hz", KNOWNMETADATA_UNIT_METER, KNOWNMETADATA_UNIT_METERS_PER_SECOND,
      KNOWNMETADATA_UNIT_MILLISECOND, KNOWNMETADATA_UNIT_SECOND);
    outputPrinter.printError("Args", msg);
    return EXIT_FAILURE;
  }

  const bool is2DCDPAxis = !is2DIndexAxis;

  // Create axis descriptors
  std::vector<OpenVDS::VolumeDataAxisDescriptor> axisDescriptors = createAxisDescriptors(fileInfo, sampleUnits, fold, primaryStep, secondaryStep, keepOriginalOrder, is2DCDPAxis, traceInfo2DManager->Count());

  // Check for excess of empty traces

  int64_t
    traceCountInVDS = 1;

  for(int axis = 1; axis < (int)axisDescriptors.size(); axis++)
  {
    traceCountInVDS *= axisDescriptors[axis].GetNumSamples();
  }

  const auto
    totalTraceCount = std::accumulate(fileInfo.m_traceCounts.begin(), fileInfo.m_traceCounts.end(), static_cast<int64_t>(0));

  if(traceCountInVDS >= totalTraceCount * 2 && !ignoreWarnings)
  {
    std::string msg = fmt::format("There is more than {:.1f}% empty traces in the VDS, this usually indicates using the wrong header format or primary key for the input dataset.\nUse --ignore-warnings to force the import to go ahead.", double(traceCountInVDS - totalTraceCount) * 100.0 / double(traceCountInVDS));
    outputPrinter.printError("SEGY", msg);
    return EXIT_FAILURE;
  }

  // Create metadata
  OpenVDS::MetadataContainer
    metadataContainer;

  createImportInformationMetadata(dataProviders, metadataContainer, error);

  if (error.code != 0)
  {
    outputPrinter.printError("Metadata", error.string);
    return EXIT_FAILURE;
  }

  {
    SEGY::BinaryHeader::MeasurementSystem
      autoDetectedSegyMeasurementSystem;

    // get SEGY metadata from first file
    createSEGYMetadata(dataProviders[0], fileInfo, metadataContainer, autoDetectedSegyMeasurementSystem, error);

    if (error.code != 0)
    {
      outputPrinter.printError("Metadata", error.string);
      return EXIT_FAILURE;
    }

    surveyCoordinateSystemTransformer.resolveUnits(segySurveyCoordinateSystemUnitString, autoDetectedSegyMeasurementSystem, surveyCoordinateSystemUnitString);
  }

  int offsetStart = 0, offsetEnd = 0;

  if (!gatherOffsetValues.empty())
  {
    // use minmax_element because the offset values are not necessarily sorted (and use 1 for the offset step because the values aren't necessarily spaced consistently)
    const auto
      minMax = std::minmax_element(gatherOffsetValues.begin(), gatherOffsetValues.end());
    offsetStart = *minMax.first,
      offsetEnd = *minMax.second;
  }

  OffsetChannelInfo
    offsetInfo(fileInfo.HasGatherOffset(), 
       surveyCoordinateSystemTransformer.convertToVdsDistance(static_cast<float>(offsetStart))
      , surveyCoordinateSystemTransformer.convertToVdsDistance(static_cast<float>(offsetEnd))
      , surveyCoordinateSystemTransformer.convertToVdsDistance(1.0f));

  // Create channel descriptors
  std::vector<OpenVDS::VolumeDataChannelDescriptor> channelDescriptors = createChannelDescriptors(fileInfo, valueRange, offsetInfo, attributeName, attributeUnit,
    isAzimuth, isMutes, vdsFormatOverride, overrideScale, !integerScaleString.empty(), userIntegerScaleOffset, error);

  if (error.code)
  {
    outputPrinter.printError("Failed creating channel descriptors", error.string);
    return EXIT_FAILURE;
  }

  // Always create metadata for WKT (if set) and SEGY units
  createSurveyCoordinateSystemWKTAndUnitMetadata(fileInfo, surveyCoordinateSystemTransformer.vdsMeasurementSystem, crsWkt, metadataContainer);

  if (primaryKeyValue == PrimaryKeyValue::InlineNumber || primaryKeyValue == PrimaryKeyValue::CrosslineNumber)
  {
    // only create the lattice metadata if the primary key is Inline or Crossline, else we may not have Inline/Crossline bin data
    createSurveyCoordinateSystemMetadata(fileInfo, surveyCoordinateSystemTransformer, metadataContainer, primaryKeyValue);
  }

  TraceInfo2DOptions
    traceInfoOptions{};
  std::vector<int>
    traceInfoKeys;

  if (fileInfo.Is2D())
  {
    // output all four items for 2D SEGY
    traceInfoOptions = { true, true, true, true };

    if (is2DCDPAxis)
    {
      // the primary axis has ensemble numbers so we can leave out the ensemble numbers metadata BLOB
      traceInfoOptions.hasEnsembleNumbers = false;
    }

    // calculate all ensemble numbers for trace info keys (can't use found ensemble numbers because there may be gaps)
    const int
      ensembleAxisIndex = segyType == SEGY::SEGYType::Prestack2D ? 2 : 1;

    const auto
      foundEnsembleNumbers = traceInfo2DManager->EnsembleNumbers();
    const auto
      minMax = std::minmax_element(foundEnsembleNumbers.begin(), foundEnsembleNumbers.end());
    const auto
      ensembleNumberBegin = *(minMax.first),
      ensembleNumberEnd = *(minMax.second);
    const auto
      ensembleCount = axisDescriptors[ensembleAxisIndex].GetNumSamples();
    int ensembleNumberStep = 1;
    bool risingNumbers = true;
    if (ensembleCount > 1)
    {
      ensembleNumberStep = (ensembleNumberEnd - ensembleNumberBegin) / (ensembleCount - 1);
      risingNumbers = foundEnsembleNumbers[0] < foundEnsembleNumbers[1];
    }

    traceInfoKeys.reserve(ensembleCount);
    if (risingNumbers)
    {
      for (int ensembleNumber = ensembleNumberBegin; ensembleNumber <= ensembleNumberEnd; ensembleNumber += ensembleNumberStep)
      {
        traceInfoKeys.push_back(ensembleNumber);
      }
    }
    else
    {
      for (int ensembleNumber = ensembleNumberEnd; ensembleNumber >= ensembleNumberBegin; ensembleNumber -= ensembleNumberStep)
      {
        traceInfoKeys.push_back(ensembleNumber);
      }
    }
  }
  else if (fileInfo.IsUnbinned())
  {
    // key values are the segment primary keys
    for (const auto& segmentInfoList : fileInfo.m_segmentInfoLists)
    {
      for (const auto& segmentInfo : segmentInfoList)
      {
        traceInfoKeys.push_back(segmentInfo.m_primaryKey);
      }
    }

    // output positions for all unbinned data
    traceInfoOptions.hasTracePositions = true;

    if (segyType == SEGY::SEGYType::CDPGathers)
    {
      // also output ensemble numbers for CDP gathers
      traceInfoOptions.hasEnsembleNumbers = true;
    }
    else if (segyType == SEGY::SEGYType::ShotGathers)
    {
      // also output ESPs for shot gathers
      traceInfoOptions.hasESPs = true;
    }
    // no additional output for receiver gathers
  }

  create2DTraceInformationMetadata(traceInfoOptions, traceInfo2DManager.get(), metadataContainer, surveyCoordinateSystemTransformer, traceInfoKeys, error);

  if (error.code != 0)
  {
    outputPrinter.printError("Metadata", error.string);
    return EXIT_FAILURE;
  }

  OpenVDS::ScopedVDSHandle handle;
  url = GetUrlWithPersistentID(url, persistentID, OpenVDS::IsSupportedProtocol(url));
  if (resumeMode)
  {
    OpenVDS::Error openError;
    handle = OpenVDS::Open(url, urlConnection, openError);
    if (openError.code != 0)
    {
      outputPrinter.printError("VDS", "Could not open VDS in resume mode", openError.string);
      return EXIT_FAILURE;
    }
    auto layout = OpenVDS::GetLayout(handle);
    {
      auto layoutJson = OpenVDS::SerializeVolumeDataLayoutDescriptor(*layout);
      auto generatedlayoutJson = OpenVDS::SerializeVolumeDataLayoutDescriptor(layoutDescriptor);
      if (layoutJson["forceFullResolutionDimension"].asBool() == generatedlayoutJson["forceFullResolutionDimension"].asBool())
        generatedlayoutJson["fullResolutionDimension"] = layoutJson["fullResolutionDimension"];
      if (layoutJson != generatedlayoutJson)
      {
        outputPrinter.printError("VDS", "VolumeDataLayout mismatch. The opened VolumeDataLayout does match the VolumeDataLayout generated from input.", fmt::format("\nOpened: {}\nGenerated: {}\n", convertToString(layoutJson), convertToString(generatedlayoutJson)));
        return EXIT_FAILURE;
      }
    }
    {
      auto axisJson = OpenVDS::SerializeAxisDescriptors(*layout);
      auto generatedAxisJson = SerializeAxisDescriptors(axisDescriptors);
      if (axisJson != generatedAxisJson)
      {
        outputPrinter.printError("VDS", "VolumeDataAxisDescriptors mismatch. The Axis descriptors from the opened VDS does not match the axis descriptors generated from input."
          , fmt::format("\nOpened: {}\nGenerated: {}\n", convertToString(axisJson), convertToString(generatedAxisJson)));
        return EXIT_FAILURE;
      }
    }
    {
      auto channelJson = OpenVDS::SerializeChannelDescriptors(*layout);
      auto generatedChannelJson = SerializeChannelDescriptors(channelDescriptors);
      if (channelJson != generatedChannelJson)
      {
        outputPrinter.printError("VDS", "VolumeDataChannelDescriptors mismatch. The Channel descriptors from the opened VDS does not match the channel descriptors generated from input."
          , fmt::format("\nOpened: {}\nGenerated: {}\n", convertToString(channelJson), convertToString(generatedChannelJson)));
        return EXIT_FAILURE;
      }
    }
  }
  else
  {
    OpenVDS::Error createError;
    auto openOptions = OpenVDS::CreateOpenOptions(url, urlConnection, createError);
    if (createError.code == 0)
    {
      //Update the legal tag in case its a DMS connection
      if (openOptions->connectionType == OpenVDS::OpenOptions::DMS)
      {
        auto& legalTag = static_cast<OpenVDS::DMSOpenOptions&>(*openOptions).legalTag;
        if (legalTag.empty())
        {
          legalTag = GetLegalTag(dataProviders);
          outputPrinter.printInfo("VDS", fmt::format("legal tag :{} added to the open options", legalTag));
        }
      }

      handle = OpenVDS::Create(*openOptions, layoutDescriptor, axisDescriptors, channelDescriptors, metadataContainer, compressionMethod, compressionTolerance, createError);
    }
    
    if (createError.code != 0)
    {
      outputPrinter.printError("VDS", "Could not create VDS", createError.string);
      return EXIT_FAILURE;
    }
  }

  auto accessManager = OpenVDS::GetAccessManager(handle);
  auto layout = accessManager.GetVolumeDataLayout();

  OpenVDS::DimensionsND writeDimensionGroup = OpenVDS::DimensionsND::Dimensions_012;

  if (IsSEGYTypeUnbinned(segyType) || segyType == SEGY::SEGYType::Poststack2D)
  {
    writeDimensionGroup = OpenVDS::DimensionsND::Dimensions_01;
  }
  else if(segyType == SEGY::SEGYType::Prestack && primaryKey == "crosslinenumber")
  {
    writeDimensionGroup = OpenVDS::DimensionsND::Dimensions_013;
  }
  else if (fileInfo.IsOffsetSorted())
  {
    writeDimensionGroup = OpenVDS::DimensionsND::Dimensions_023;
  }

  int offsetChannelIndex = 0, azimuthChannelIndex = 0, muteChannelIndex = 0;

  if (fileInfo.HasGatherOffset())
  {
    offsetChannelIndex = findChannelDescriptorIndex("Offset", channelDescriptors);
    if (offsetChannelIndex < 0)
    {
      outputPrinter.printError("VDS", "Could not find VDS channel descriptor for Offset");
      return EXIT_FAILURE;
    }
  }

  if (isAzimuth)
  {
    azimuthChannelIndex = findChannelDescriptorIndex("Azimuth", channelDescriptors);
    if (azimuthChannelIndex < 0)
    {
      outputPrinter.printError("VDS", "Could not find VDS channel descriptor for Azimuth");
      return EXIT_FAILURE;
    }
  }

  if (isMutes)
  {
    muteChannelIndex = findChannelDescriptorIndex("Mute", channelDescriptors);
    if (muteChannelIndex < 0)
    {
      outputPrinter.printError("VDS", "Could not find VDS channel descriptor for Mute");
      return EXIT_FAILURE;
    }
  }

  auto accessMode = resumeMode ? OpenVDS::VolumeDataAccessManager::AccessMode_ReadWrite : OpenVDS::VolumeDataAccessManager::AccessMode_Create;

  auto amplitudeAccessor = accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, 0, 8, accessMode);
  auto traceFlagAccessor = accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, 1, 8, accessMode);
  auto segyTraceHeaderAccessor = accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, 2, 8, accessMode);
  auto offsetAccessor = fileInfo.HasGatherOffset() ? accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, offsetChannelIndex, 8, accessMode) : nullptr;
  auto azimuthAccessor = isAzimuth ? accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, azimuthChannelIndex, 8, accessMode) : nullptr;
  auto muteAccessor = isMutes ? accessManager.CreateVolumeDataPageAccessor(writeDimensionGroup, 0, muteChannelIndex, 8, accessMode) : nullptr;

  int64_t traceByteSize = fileInfo.TraceByteSize();

  std::shared_ptr<DataView> dataView;

  double percentage = -1.0;
  outputPrinter.printInfo("ImportLocation", "Importing into", url);

  struct ChunkInfo
  {
    int min[OpenVDS::Dimensionality_Max];
    int max[OpenVDS::Dimensionality_Max];
    int64_t chunk;
    int sampleStart;
    int sampleCount;
    int secondaryKeyStart;
    int secondaryKeyStop;
    int primaryKeyStart;
    int primaryKeyStop;
    std::map<size_t, std::pair<size_t, size_t>>
      lowerUpperSegmentIndices;
    bool skip;
  };

  // chunk information for iterating crossline-sorted data in input order (instead of default chunk order)
  std::array<int64_t, 4>
    dimChunkCounts = { 0, 0, 0, 0 },
    dimChunkPitches = { 1, 1, 1, 1 };
  const auto
    dimensionality = axisDescriptors.size();

  // limit DataViewManager's memory use to 1.5 sets of brick inlines
  const int64_t dvmMemoryLimit = 3LL * (writeDimensionGroup == OpenVDS::DimensionsND::Dimensions_01 ? 1 : brickSize) * axisDescriptors[1].GetNumSamples() * fileInfo.TraceByteSize() / 2LL;

  // create DataViewManagers and TraceDataManagers for each input file
  std::vector<std::shared_ptr<DataViewManager>>
    dataViewManagers;
  std::vector<TraceDataManager>
    traceDataManagers;
  const auto
    perFileMemoryLimit = dvmMemoryLimit / dataProviders.size();

  assert(dataProviders.size() == (fileInfo.IsOffsetSorted() ? fileInfo.m_segmentInfoListsByOffset.size() : fileInfo.m_segmentInfoLists.size()));

  for (size_t fileIndex = 0; fileIndex < dataProviders.size(); ++fileIndex)
  {
    dataViewManagers.emplace_back(std::make_shared<DataViewManager>(dataProviders[fileIndex], perFileMemoryLimit));
    traceDataManagers.emplace_back(dataViewManagers.back(), 128, traceByteSize, fileInfo.m_traceCounts[fileIndex]);
  }

  const int
    primaryKeyDimension = PrimaryKeyDimension(fileInfo, primaryKeyValue),
    secondaryKeyDimension = SecondaryKeyDimension(fileInfo, primaryKeyValue);

  std::set<int64_t>
    ignoredTraces;

  std::vector<ChunkInfo> chunkInfos;
  chunkInfos.resize(amplitudeAccessor->GetChunkCount());
  for (int64_t chunk = 0; chunk < amplitudeAccessor->GetChunkCount(); chunk++)
  {
    auto &chunkInfo = chunkInfos[chunk];
    chunkInfo.skip = true;
    amplitudeAccessor->GetChunkMinMax(chunk, chunkInfo.min, chunkInfo.max);

    if (amplitudeAccessor->GetChunkVolumeDataHash(chunk) == 0)
      chunkInfo.skip = false;
    if (chunkInfo.min[0] == 0)
    {
      if (traceFlagAccessor->GetChunkVolumeDataHash(traceFlagAccessor->GetMappedChunkIndex(chunk)) == 0)
        chunkInfo.skip = false;
      if (segyTraceHeaderAccessor->GetChunkVolumeDataHash(segyTraceHeaderAccessor->GetMappedChunkIndex(chunk)) == 0)
        chunkInfo.skip = false;
      if (offsetAccessor != nullptr && offsetAccessor->GetChunkVolumeDataHash(offsetAccessor->GetMappedChunkIndex(chunk)) == 0)
        chunkInfo.skip = false;
      if (azimuthAccessor != nullptr && azimuthAccessor->GetChunkVolumeDataHash(azimuthAccessor->GetMappedChunkIndex(chunk)) == 0)
        chunkInfo.skip = false;
      if (muteAccessor != nullptr && muteAccessor->GetChunkVolumeDataHash(muteAccessor->GetMappedChunkIndex(chunk)) == 0)
        chunkInfo.skip = false;
    }

    if (primaryKeyValue == PrimaryKeyValue::CrosslineNumber)
    {
      for (size_t i = 0; i < dimChunkCounts.size(); ++i)
      {
        bool isZero = true;
        for (size_t j = 0; isZero && j < dimChunkCounts.size(); ++j)
        {
          if (j != i)
          {
            isZero = chunkInfo.min[j] == 0;
          }
        }
        if (isZero)
        {
          ++dimChunkCounts[i];
        }
      }
    }

    chunkInfo.sampleStart = chunkInfo.min[0];
    chunkInfo.sampleCount = chunkInfo.max[0] - chunkInfo.min[0];

    OpenVDS::Error traceIndexError;

    chunkInfo.secondaryKeyStart = (int)floorf(layout->GetAxisDescriptor(secondaryKeyDimension).SampleIndexToCoordinate(chunkInfo.min[secondaryKeyDimension]) + 0.5f);
    chunkInfo.secondaryKeyStop = (int)floorf(layout->GetAxisDescriptor(secondaryKeyDimension).SampleIndexToCoordinate(chunkInfo.max[secondaryKeyDimension] - 1) + 0.5f);

    if (fileInfo.Is2D() && traceIndexError.code != 0)
    {
      outputPrinter.printWarning("2DEnsembleIndex", "Could not translate trace index to ensemble number", fmt::format("{}", error.code), error.string);
      break;
    }

    chunkInfo.primaryKeyStart = fileInfo.Is2D() ? 0 : (int)floorf(layout->GetAxisDescriptor(primaryKeyDimension).SampleIndexToCoordinate(chunkInfo.min[primaryKeyDimension]) + 0.5f);
    chunkInfo.primaryKeyStop = fileInfo.Is2D() ? 0 : (int)floorf(layout->GetAxisDescriptor(primaryKeyDimension).SampleIndexToCoordinate(chunkInfo.max[primaryKeyDimension] - 1) + 0.5f);
    if(chunkInfo.primaryKeyStart > chunkInfo.primaryKeyStop)
    {
      std::swap(chunkInfo.primaryKeyStart, chunkInfo.primaryKeyStop);
    }

    // For each input file, find the lower/upper segments and then add data requests to that file's traceDataManager

    const auto segmentInfoListsSize = fileInfo.IsOffsetSorted() ? fileInfo.m_segmentInfoListsByOffset.size() : fileInfo.m_segmentInfoLists.size();

    for (size_t fileIndex = 0; fileIndex < segmentInfoListsSize; ++fileIndex)
    {
      assert(fileInfo.IsOffsetSorted() ? chunkInfo.min[1] < int(gatherOffsetValues.size()) : true);

      const int
        offsetSortedOffsetValue = fileInfo.IsOffsetSorted() ? gatherOffsetValues[chunkInfo.min[1]] : 0;

      if (fileInfo.IsOffsetSorted())
      {
        // for offset sorted chunks should only have a single offset (i.e. dim 02 or 023)
        assert(chunkInfo.min[1] + 1 == chunkInfo.max[1]);

        // If the calculated offset value isn't present in this file's map then skip
        if (fileInfo.m_segmentInfoListsByOffset[fileIndex].find(offsetSortedOffsetValue) == fileInfo.m_segmentInfoListsByOffset[fileIndex].end())
        {
          continue;
        }
      }

      auto&
        segmentInfoList = fileInfo.IsOffsetSorted()
        ? fileInfo.m_segmentInfoListsByOffset[fileIndex][offsetSortedOffsetValue]
        : fileInfo.m_segmentInfoLists[fileIndex];

      // does this file have any segments in the primary key range?
      bool hasSegments;
      if (fileInfo.IsUnbinned())
      {
        // Unbinned primary keys are 1-based segment indices
        hasSegments = 1 <= chunkInfo.primaryKeyStop && static_cast<int>(segmentInfoList.size()) >= chunkInfo.primaryKeyStart;
      }
      else
      {
        if(segmentInfoList.front().m_primaryKey <= segmentInfoList.back().m_primaryKey)
        {
          hasSegments = segmentInfoList.front().m_primaryKey <= chunkInfo.primaryKeyStop && segmentInfoList.back().m_primaryKey >= chunkInfo.primaryKeyStart;
        }
        else
        {
          hasSegments = segmentInfoList.back().m_primaryKey <= chunkInfo.primaryKeyStop && segmentInfoList.front().m_primaryKey >= chunkInfo.primaryKeyStart;
        }
      }
      if (hasSegments)
      {
        std::vector<SEGYSegmentInfo>::iterator
          lower,
          upper;

        if (fileInfo.Is2D())
        {
          // Hopefully 2D files always a single line
          lower = segmentInfoList.begin();
          upper = segmentInfoList.end();
        }
        else if (fileInfo.IsUnbinned())
        {
          // For unbinned data we set lower and upper based on the 1-based indices instead of searching on the segment primary keys.
          // When we implement raw gathers mode we'll need to modify this.
          lower = segmentInfoList.begin();
          std::advance(lower, chunkInfo.primaryKeyStart - 1);
          upper = segmentInfoList.begin();
          // no "- 1" because we want upper to be the stop iterator
          std::advance(upper, chunkInfo.primaryKeyStop);
        }
        else
        {
          if(segmentInfoList.front().m_primaryKey <= segmentInfoList.back().m_primaryKey)
          {
            lower = std::lower_bound(segmentInfoList.begin(), segmentInfoList.end(), chunkInfo.primaryKeyStart, [](SEGYSegmentInfo const& segmentInfo, int primaryKey)->bool { return segmentInfo.m_primaryKey < primaryKey; });
            upper = std::upper_bound(segmentInfoList.begin(), segmentInfoList.end(), chunkInfo.primaryKeyStop,  [](int primaryKey, SEGYSegmentInfo const& segmentInfo)->bool { return primaryKey < segmentInfo.m_primaryKey; });
          }
          else
          {
            lower = std::lower_bound(segmentInfoList.begin(), segmentInfoList.end(), chunkInfo.primaryKeyStop,  [](SEGYSegmentInfo const& segmentInfo, int primaryKey)->bool { return segmentInfo.m_primaryKey > primaryKey; });
            upper = std::upper_bound(segmentInfoList.begin(), segmentInfoList.end(), chunkInfo.primaryKeyStart, [](int primaryKey, SEGYSegmentInfo const& segmentInfo)->bool { return primaryKey > segmentInfo.m_primaryKey; });
          }
        }

        const size_t lowerSegmentIndex = std::distance(segmentInfoList.begin(), lower);
        const size_t upperSegmentIndex = std::distance(segmentInfoList.begin(), upper);

        chunkInfo.lowerUpperSegmentIndices[fileIndex] = std::make_pair(lowerSegmentIndex, upperSegmentIndex);

        if (!chunkInfo.skip)
          traceDataManagers[fileIndex].addDataRequests(chunkInfo.secondaryKeyStart, chunkInfo.secondaryKeyStop, lower, upper);
      }
    }
  }

  for (size_t i = 1; i < dimChunkCounts.size(); ++i)
  {
    dimChunkPitches[i] = dimChunkPitches[i - 1] * dimChunkCounts[i - 1];
  }

  // data and temp buffers used when overriding the VDS sample data format
  std::vector<float> overrideFloatTrace;
  std::vector<uint16_t> overrideU16Trace;
  std::vector<uint8_t> overrideU8Trace;

  if (vdsFormatOverride == VDSFormatOverride::Override16Bit)
  {
    overrideFloatTrace.resize(fileInfo.m_sampleCount);
    overrideU16Trace.resize(fileInfo.m_sampleCount);
  }
  else if (vdsFormatOverride == VDSFormatOverride::Override8Bit)
  {
    overrideFloatTrace.resize(fileInfo.m_sampleCount);
    overrideU8Trace.resize(fileInfo.m_sampleCount);
  }

  if (flushFrequency == 0)
  {
    flushFrequency = std::numeric_limits<decltype(flushFrequency)>::max();
  }
  auto last_flush = std::chrono::steady_clock::now();

  bool encountedErrors = false;
  for (int64_t chunkSequence = 0; chunkSequence < amplitudeAccessor->GetChunkCount() && error.code == 0; chunkSequence++)
  {
    double new_percentage = double(chunkSequence) / amplitudeAccessor->GetChunkCount() * 100.0;
    if (new_percentage - percentage > 0.3333)
    {
      percentage = new_percentage;
      outputPrinter.printPercentage(percentage);
    }
    auto
      chunk = chunkSequence;

    if (primaryKeyValue == PrimaryKeyValue::CrosslineNumber && (dimensionality == 3 || dimensionality == 4) && !isDisableCrosslineReordering)
    {
      // recompute the chunk number for the crossline-sorted chunk corresponding to the current sequence number

      std::array<int64_t, 4> chunkCoords = { 0, 0, 0, 0 };
      if (dimensionality == 3)
      {
        auto dv = std::div(chunkSequence, dimChunkCounts[0]);
        chunkCoords[0] = dv.rem;
        dv = std::div(dv.quot, dimChunkCounts[2]);  // 2 not 1 because we're working in crossline-major space
        chunkCoords[1] = dv.quot;
        chunkCoords[2] = dv.rem;
      }
      else
      {
        auto dv = std::div(chunkSequence, dimChunkCounts[0]);
        chunkCoords[0] = dv.rem;
        dv = std::div(dv.quot, dimChunkCounts[1]);
        chunkCoords[1] = dv.rem;
        dv = std::div(dv.quot, dimChunkCounts[3]);  // 3 not 2 because we're working in crossline-major space
        chunkCoords[2] = dv.quot;
        chunkCoords[3] = dv.rem;
      }

      chunk = 0;
      for (size_t i = 0; i < chunkCoords.size(); ++i)
      {
        chunk += chunkCoords[i] * dimChunkPitches[i];
      }
    }

    auto &chunkInfo = chunkInfos[chunk];
    if (chunkInfo.skip)
      continue;
    // if we've crossed to a new primary key range then trim the trace page cache
    if (chunk > 0)
    {
      const auto& previousChunkInfo = chunkInfos[chunk - 1];
      if (!previousChunkInfo.skip)
      {

        for (size_t chunkFileIndex = 0; chunkFileIndex < dataProviders.size(); ++chunkFileIndex)
        {
          auto prevIndexIter = previousChunkInfo.lowerUpperSegmentIndices.find(chunkFileIndex);
          if (prevIndexIter != previousChunkInfo.lowerUpperSegmentIndices.end())
          {
            auto currentIndexIter = chunkInfo.lowerUpperSegmentIndices.find(chunkFileIndex);
            if (currentIndexIter != chunkInfo.lowerUpperSegmentIndices.end())
            {
              // This file is active in both the current and previous chunks. Check to see if we've progressed to a new set of primary keys.
              auto previousLowerSegmentIndex = std::get<0>(prevIndexIter->second);
              auto currentLowerSegmentIndex = std::get<0>(currentIndexIter->second);
              if (currentLowerSegmentIndex > previousLowerSegmentIndex)
              {
                // we've progressed to a new set of primary keys; remove earlier pages from the cache
                traceDataManagers[chunkFileIndex].retirePagesBefore(fileInfo.m_segmentInfoLists[chunkFileIndex][currentLowerSegmentIndex].m_traceStart);
              }
            }
            else
            {
              // This file was active in the previous chunk but not in the current chunk, which implies that we don't
              // need any more data from this file.
              traceDataManagers[chunkFileIndex].retireAllPages();
            }
          }
          // else This file isn't used in either the previous or current chunks. We don't need to do anything.
        }
      }
    }

    OpenVDS::VolumeDataPage* amplitudePage = nullptr;
    if (amplitudeAccessor->GetChunkVolumeDataHash(chunk) == 0)
      amplitudePage = amplitudeAccessor->CreatePage(chunk);
    OpenVDS::VolumeDataPage* traceFlagPage = nullptr;
    OpenVDS::VolumeDataPage* segyTraceHeaderPage = nullptr;
    OpenVDS::VolumeDataPage* offsetPage = nullptr;
    OpenVDS::VolumeDataPage* azimuthPage = nullptr;
    OpenVDS::VolumeDataPage* mutePage = nullptr;

    if (chunkInfo.min[0] == 0)
    {
      if (traceFlagAccessor->GetChunkVolumeDataHash(traceFlagAccessor->GetMappedChunkIndex(chunk)) == 0)
        traceFlagPage = traceFlagAccessor->CreatePage(traceFlagAccessor->GetMappedChunkIndex(chunk));
      if (segyTraceHeaderAccessor->GetChunkVolumeDataHash(segyTraceHeaderAccessor->GetMappedChunkIndex(chunk)) == 0)
        segyTraceHeaderPage = segyTraceHeaderAccessor->CreatePage(segyTraceHeaderAccessor->GetMappedChunkIndex(chunk));
      if (offsetAccessor != nullptr && offsetAccessor->GetChunkVolumeDataHash(offsetAccessor->GetMappedChunkIndex(chunk)) == 0)
      {
        offsetPage = offsetAccessor->CreatePage(offsetAccessor->GetMappedChunkIndex(chunk));
      }
      if (azimuthAccessor != nullptr && azimuthAccessor->GetChunkVolumeDataHash(azimuthAccessor->GetMappedChunkIndex(chunk)) == 0)
      {
        azimuthPage = azimuthAccessor->CreatePage(azimuthAccessor->GetMappedChunkIndex(chunk));
      }
      if (muteAccessor != nullptr && muteAccessor->GetChunkVolumeDataHash(muteAccessor->GetMappedChunkIndex(chunk)) == 0)
      {
        mutePage = muteAccessor->CreatePage(muteAccessor->GetMappedChunkIndex(chunk));
      }
    }

    int amplitudePitch[OpenVDS::Dimensionality_Max];
    int traceFlagPitch[OpenVDS::Dimensionality_Max];
    int segyTraceHeaderPitch[OpenVDS::Dimensionality_Max];
    int offsetPitch[OpenVDS::Dimensionality_Max];
    int azimuthPitch[OpenVDS::Dimensionality_Max];
    int mutePitch[OpenVDS::Dimensionality_Max];

    void* amplitudeBuffer = amplitudePage ? amplitudePage->GetWritableBuffer(amplitudePitch) : nullptr;
    void* traceFlagBuffer = traceFlagPage ? traceFlagPage->GetWritableBuffer(traceFlagPitch) : nullptr;
    void* segyTraceHeaderBuffer = segyTraceHeaderPage ? segyTraceHeaderPage->GetWritableBuffer(segyTraceHeaderPitch) : nullptr;
    void* offsetBuffer = offsetPage ? offsetPage->GetWritableBuffer(offsetPitch) : nullptr;
    void* azimuthBuffer = azimuthPage ? azimuthPage->GetWritableBuffer(azimuthPitch) : nullptr;
    void* muteBuffer = mutePage ? mutePage->GetWritableBuffer(mutePitch) : nullptr;

    assert(!amplitudeBuffer || amplitudePitch[0] == 1);
    assert(!traceFlagBuffer || traceFlagPitch[fileInfo.IsOffsetSorted() ? 2 : 1] == 1);
    assert(!segyTraceHeaderBuffer || segyTraceHeaderPitch[fileInfo.IsOffsetSorted() ? 2 : 1] == SEGY::TraceHeaderSize);
    assert(!offsetBuffer || offsetPitch[fileInfo.IsOffsetSorted() ? 2 : 1] == 1);
    assert(!azimuthBuffer || azimuthPitch[fileInfo.IsOffsetSorted() ? 2 : 1] == 1);
    assert(!muteBuffer || mutePitch[fileInfo.IsOffsetSorted() ? 2 : 1] == 1);
    
    const auto segmentInfoListsSize = fileInfo.IsOffsetSorted() ? fileInfo.m_segmentInfoListsByOffset.size() : fileInfo.m_segmentInfoLists.size();

    for (size_t fileIndex = 0; fileIndex < segmentInfoListsSize && error.code == 0; ++fileIndex)
    {
      auto result = chunkInfo.lowerUpperSegmentIndices.find(fileIndex);
      if (result == chunkInfo.lowerUpperSegmentIndices.end())
      {
        continue;
      }

      assert(fileInfo.IsOffsetSorted() ? chunkInfo.min[1] < int(gatherOffsetValues.size()) : true);

      const int
        offsetSortedOffsetValue = fileInfo.IsOffsetSorted() ? gatherOffsetValues[chunkInfo.min[1]] : 0;

      const auto lowerSegmentIndex = std::get<0>(result->second);
      const auto upperSegmentIndex = std::get<1>(result->second);

      const auto&
        segmentInfo = fileInfo.IsOffsetSorted() ? fileInfo.m_segmentInfoListsByOffset[fileIndex][offsetSortedOffsetValue] : fileInfo.m_segmentInfoLists[fileIndex];
      auto&
        traceDataManager = traceDataManagers[fileIndex];

      // We loop through the segments that have primary keys inside this block and copy the traces that have secondary keys inside this block
      auto lower = segmentInfo.begin() + lowerSegmentIndex;
      auto upper = segmentInfo.begin() + upperSegmentIndex;

      for (auto segment = lower; segment != upper && error.code == 0; ++segment)
      {
        int
          binInfoSecondaryStart = BinInfoSecondaryKeyValue(primaryKeyValue, segment->m_binInfoStart),
          binInfoSecondaryStop = BinInfoSecondaryKeyValue(primaryKeyValue, segment->m_binInfoStop);

        const bool
          isSecondaryFalling = binInfoSecondaryStart > binInfoSecondaryStop;

        int64_t firstTrace;
        if (fileInfo.IsUnbinned())
        {
          // For unbinned gathers the secondary key is the 1-based index of the trace, so to get the
          // first trace we convert the index to 0-based and add that to the segment's start trace.
          // Similarly, for Poststack2D the secondary key is also the 1-based trace index.
          firstTrace = segment->m_traceStart + (static_cast<int64_t>(chunkInfo.secondaryKeyStart) - 1L);
        }
        else if (fileInfo.m_segyType == SEGY::SEGYType::Poststack2D)
        {
          if (is2DCDPAxis)
          {
            int ensembleIndex;
            if (!Find2DEnsembleInChunkRange(traceInfo2DManager.get(), chunkInfo.secondaryKeyStart, chunkInfo.secondaryKeyStop, ensembleIndex))
            {
              // didn't find an ensemble within the chunk's key range, bail out
              break;
            }
            firstTrace = segment->m_traceStart + ensembleIndex;
          }
          else
          {
            // Same as for unbinned data when using 1..N axis annotation
            firstTrace = segment->m_traceStart + (static_cast<int64_t>(chunkInfo.secondaryKeyStart) - 1L);
          }
        }
        else if (fileInfo.m_segyType == SEGY::SEGYType::Prestack2D)
        {
          int ensembleIndex;
          if (is2DCDPAxis)
          {
            if (!Find2DEnsembleInChunkRange(traceInfo2DManager.get(), chunkInfo.secondaryKeyStart, chunkInfo.secondaryKeyStop, ensembleIndex))
            {
              // didn't find an ensemble within the chunk's key range, bail out
              break;
            }
          }
          else
          {
            // For 2D prestack convert 1-based secondary key to 0-based index
            ensembleIndex = chunkInfo.secondaryKeyStart - 1;
          }
          // use ensemble index to lookup the ensemble's first trace number
          firstTrace = EnsembleIndex2DToTraceNumber(traceInfo2DManager.get(), ensembleIndex, error);
          if (error.code)
          {
            outputPrinter.printWarning("IO", "Could not map EnsembleNumber to trace number", fmt::format("{}", error.code), error.string);
            break;
          }
        }
        else
        {
          firstTrace = findFirstTrace(traceDataManager, *segment, chunkInfo.secondaryKeyStart, fileInfo, primaryKeyValue, error);
          if (error.code)
          {
            outputPrinter.printWarning("IO", "Failed when reading data", fmt::format("{}", error.code), error.string);
            break;
          }
        }

        int
          tertiaryIndex = 0,
          currentSecondaryKey = chunkInfo.secondaryKeyStart;

        std::unique_ptr<GatherSpacing> gatherSpacing;

        int64_t
          traceIncrement;
        std::function<bool(int64_t)>
          traceInRange;
        if (isSecondaryFalling && !keepOriginalOrder && !fileInfo.Is2D())
        {
          traceIncrement = -1;
          traceInRange = [segment](int64_t traceNumber) { return traceNumber >= segment->m_traceStart; };
        }
        else
        {
          traceIncrement = 1;
          traceInRange = [segment](int64_t traceNumber) { return traceNumber <= segment->m_traceStop; };
        }

        int previousSecondaryTest = 0;
        for (int64_t trace = firstTrace; traceInRange(trace); trace += traceIncrement, tertiaryIndex++)
        {
          if (fileInfo.Is4D() && !static_cast<bool>(gatherSpacing))
          {
            // get the first GatherSpacing
            // (do it here instead of before the loop to handle the case where 'firstTrace' is past the segment)
            gatherSpacing = CalculateGatherSpacing(fileInfo, fold, gatherOffsetValues, traceDataManager, traceSpacingByOffset, firstTrace, outputPrinter);
          }

          const char* header = traceDataManager.getTraceData(trace, error);
          if (error.code)
          {
            outputPrinter.printWarning("IO", "Failed when reading data", fmt::format("{}", error.code), error.string);
            break;
          }

          const void* data = header + SEGY::TraceHeaderSize;

          if (vdsFormatOverride != VDSFormatOverride::None)
          {
            copySamples<float>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, overrideFloatTrace.data(), (const unsigned char*)data, 0, static_cast<int>(overrideFloatTrace.size()));
            if (vdsFormatOverride == VDSFormatOverride::Override16Bit)
            {
              ConvertOverrideTrace(overrideU16Trace, overrideFloatTrace, valueRange, overrideScale);
            }
            else
            {
              ConvertOverrideTrace(overrideU8Trace, overrideFloatTrace, valueRange, overrideScale);
            }
          }

          int primaryTest = fileInfo.Is2D() ? 0 : SEGY::ReadFieldFromHeader(header, fileInfo.m_primaryKey, fileInfo.m_headerEndianness);
          int secondaryTest = 0;

          if (fileInfo.IsUnbinned())
          {
            secondaryTest = static_cast<int>(trace - segment->m_traceStart + 1);
          }
          else if (fileInfo.m_segyType == SEGY::SEGYType::Poststack2D)
          {
            if (is2DCDPAxis)
            {
              secondaryTest = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);
            }
            else
            {
              secondaryTest = static_cast<int>(trace - segment->m_traceStart + 1);
            }
          }
          else
          {
            secondaryTest = SEGY::ReadFieldFromHeader(header, fileInfo.m_secondaryKey, fileInfo.m_headerEndianness);
            if (fileInfo.m_segyType == SEGY::SEGYType::Prestack2D && !is2DCDPAxis)
            {
              // For 2D prestack convert the EnsembleNumber to the 1..N value used on secondary key axis
              secondaryTest = traceInfo2DManager->GetIndexOfEnsembleNumber(secondaryTest) + 1;
            }
          }

          if(primaryTest != segment->m_primaryKey)
          {
            if(ignoredTraces.find(trace + 1) == ignoredTraces.end())
            {
              outputPrinter.printWarning("SEGY", fmt::format("trace {} has primary key {} that doesn't match with the segment's primary key {}. This trace will be ignored.", trace + 1, primaryTest, segment->m_primaryKey));
              ignoredTraces.insert(trace + 1);
            }
            continue;
          }

          if(!fileInfo.IsUnbinned() && (fileInfo.m_segyType != SEGY::SEGYType::Poststack2D || is2DCDPAxis) && (fileInfo.m_segyType != SEGY::SEGYType::Prestack2D || is2DCDPAxis) &&
             ((isSecondaryFalling ? secondaryTest < binInfoSecondaryStop : secondaryTest > binInfoSecondaryStop  ) ||
              (isSecondaryFalling ? secondaryTest > binInfoSecondaryStart : secondaryTest < binInfoSecondaryStart) ||
              (previousSecondaryTest && ((isSecondaryFalling && traceIncrement > 0) ? secondaryTest > previousSecondaryTest : secondaryTest < previousSecondaryTest))))
          {
            if(ignoredTraces.find(trace + 1) == ignoredTraces.end())
            {
              outputPrinter.printWarning("SEGY", fmt::format("trace {} has a secondary key ({}) that is not sorted correctly. This trace will be ignored.", trace + 1, secondaryTest));
              ignoredTraces.insert(trace + 1);
            }
            continue;
          }

          previousSecondaryTest = secondaryTest;

          // is secondaryTest beyond secondaryKeyStop for this chunk?
          const bool
            isSecondaryBeyondStop  = isSecondaryFalling && (keepOriginalOrder || fileInfo.Is2D()) ? secondaryTest < chunkInfo.secondaryKeyStop : secondaryTest > chunkInfo.secondaryKeyStop;

          // Check if the trace is outside the secondary range and go to the next segment if it is
          if (isSecondaryBeyondStop)
          {
            break;
          }

          if (fileInfo.IsOffsetSorted() && !isOffsetSortedDupeKeyWarned && trace > firstTrace && secondaryTest == currentSecondaryKey)
          {
            outputPrinter.printNewLine(OpenVDS::LogLevel::Info);
            auto
              message = "This offset-sorted SEGY has traces with duplicate key combinations of Offset, Inline, and Crossline. Only one of the traces with duplicate key values will be written to the output VDS.";
            outputPrinter.printWarning("SEGY", message);
            isOffsetSortedDupeKeyWarned = true;
          }

          if (secondaryTest != currentSecondaryKey)
          {
            // we've progressed to a new secondary key, so reset the tertiary (gather) index
            currentSecondaryKey = secondaryTest;
            tertiaryIndex = 0;

            // then get respace info for the next gather
            if (fileInfo.Is4D())
            {
              gatherSpacing = CalculateGatherSpacing(fileInfo, fold, gatherOffsetValues, traceDataManager, traceSpacingByOffset, trace, outputPrinter);
            }
          }

          int
            primaryIndex,
            secondaryIndex;
          if (fileInfo.Is2D())
          {
            primaryIndex = 0;
            if (is2DCDPAxis)
            {
              secondaryIndex = layout->GetAxisDescriptor(secondaryKeyDimension).CoordinateToSampleIndex((float)secondaryTest);
            }
            else
            {
              secondaryIndex = secondaryTest - 1;
            }
          }
          else if (fileInfo.IsUnbinned())
          {
            primaryIndex = static_cast<int>(segment - segmentInfo.begin());
            secondaryIndex = secondaryTest - 1;
          }
          else
          {
            primaryIndex = layout->GetAxisDescriptor(primaryKeyDimension).CoordinateToSampleIndex((float)segment->m_primaryKey);
            secondaryIndex = layout->GetAxisDescriptor(secondaryKeyDimension).CoordinateToSampleIndex((float)secondaryTest);
          }

          assert(primaryIndex >= chunkInfo.min[primaryKeyDimension] && primaryIndex < chunkInfo.max[primaryKeyDimension]);
          assert(secondaryIndex >= chunkInfo.min[secondaryKeyDimension] && secondaryIndex < chunkInfo.max[secondaryKeyDimension]);

          if (fileInfo.Is4D())
          {
            if (fileInfo.IsOffsetSorted())
            {
              // force the tertiary index to the index specified for the current chunk
              tertiaryIndex = chunkInfo.min[1];

              assert(offsetSortedOffsetValue == SEGY::ReadFieldFromHeader(header, offsetHeaderField, fileInfo.m_headerEndianness));
            }
            else
            {
              assert(static_cast<bool>(gatherSpacing));
              tertiaryIndex = gatherSpacing->GetTertiaryIndex(trace);

              // sanity check the new index
              if (tertiaryIndex < 0 || tertiaryIndex >= fold)
              {
                continue;
              }
            }

            if (tertiaryIndex < chunkInfo.min[1] || tertiaryIndex >= chunkInfo.max[1])
            {
              // the current gather trace number is not within the request bounds
              continue;
            }
          }

          if ((fileInfo.Is4D() || fileInfo.m_segyType == SEGY::SEGYType::Prestack2D) && (tertiaryIndex < chunkInfo.min[1] || tertiaryIndex >= chunkInfo.max[1]))
          {
            // the current gather trace number is not within the request bounds
            continue;
          }

          const int
            targetPrimaryIndex = primaryKeyValue == PrimaryKeyValue::CrosslineNumber ? secondaryIndex : primaryIndex,
            targetSecondaryIndex = primaryKeyValue == PrimaryKeyValue::CrosslineNumber ? primaryIndex : secondaryIndex;

          if (amplitudeBuffer)
          {
            const int targetOffset = VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, amplitudePitch);
            switch (fileInfo.m_dataSampleFormatCode)
            {
            case SEGY::BinaryHeader::DataSampleFormatCode::IBMFloat:
            case SEGY::BinaryHeader::DataSampleFormatCode::IEEEFloat:
              if (vdsFormatOverride == VDSFormatOverride::Override16Bit)
              {
                copySamples<uint16_t>(SEGY::Endianness::LittleEndian, SEGY::BinaryHeader::DataSampleFormatCode::UInt16, &reinterpret_cast<uint16_t*>(amplitudeBuffer)[targetOffset], (const unsigned char*)overrideU16Trace.data(), chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              }
              else if (vdsFormatOverride == VDSFormatOverride::Override8Bit)
              {
                copySamples<uint8_t>(SEGY::Endianness::LittleEndian, SEGY::BinaryHeader::DataSampleFormatCode::UInt8, &reinterpret_cast<uint8_t*>(amplitudeBuffer)[targetOffset], (const unsigned char*)overrideU8Trace.data(), chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              }
              else
              {
                copySamples<float>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, &reinterpret_cast<float*>(amplitudeBuffer)[targetOffset], (const unsigned char*)data, chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              }
              break;
            case SEGY::BinaryHeader::DataSampleFormatCode::UInt32:
            case SEGY::BinaryHeader::DataSampleFormatCode::Int32:
              copySamples<float>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, &reinterpret_cast<float*>(amplitudeBuffer)[targetOffset], (const unsigned char*)data, chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              break;
            case SEGY::BinaryHeader::DataSampleFormatCode::UInt16:
            case SEGY::BinaryHeader::DataSampleFormatCode::Int16:
              copySamples<uint16_t>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, &reinterpret_cast<uint16_t*>(amplitudeBuffer)[targetOffset], (const unsigned char*)data, chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              break;
            case SEGY::BinaryHeader::DataSampleFormatCode::UInt8:
            case SEGY::BinaryHeader::DataSampleFormatCode::Int8:
              copySamples<uint8_t>(fileInfo.m_headerEndianness, fileInfo.m_dataSampleFormatCode, &reinterpret_cast<uint8_t*>(amplitudeBuffer)[targetOffset], (const unsigned char*)data, chunkInfo.sampleStart, chunkInfo.sampleStart + chunkInfo.sampleCount);
              break;
            default:
              error.code = -1;
              error.string = fmt::format("Unknown input format {}.", SEGY::DataSampleFormatCodeToString(fileInfo.m_dataSampleFormatCode));
              continue;
            }
          }

          if (traceFlagBuffer)
          {
            const int targetOffset = VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, traceFlagPitch);

            reinterpret_cast<uint8_t*>(traceFlagBuffer)[targetOffset] = true;
          }

          if (segyTraceHeaderBuffer)
          {
            const int targetOffset = VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, segyTraceHeaderPitch);

            memcpy(&reinterpret_cast<uint8_t*>(segyTraceHeaderBuffer)[targetOffset], header, SEGY::TraceHeaderSize);
          }

          if (offsetBuffer)
          {
            const int targetOffset = VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, offsetPitch);

            const auto
              traceOffset = surveyCoordinateSystemTransformer.convertToVdsDistance(static_cast<float>(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offset"], fileInfo.m_headerEndianness)));
            static_cast<float*>(offsetBuffer)[targetOffset] = traceOffset;
          }

          if (azimuthBuffer)
          {
            float
              azimuth;
            if (azimuthType == SEGY::AzimuthType::Azimuth)
            {
              azimuth = static_cast<float>(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["azimuth"], fileInfo.m_headerEndianness));

              // apply azimuth scale
              azimuth = static_cast<float>(azimuth * azimuthScaleFactor);

              if (azimuthUnit == SEGY::AzimuthUnits::Radians)
              {
                azimuth = static_cast<float>(azimuth * 180.0 / M_PI);
              }
            }
            else
            {
              auto
                offsetX = SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offsetx"], fileInfo.m_headerEndianness),
                offsetY = SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["offsety"], fileInfo.m_headerEndianness);
              azimuth = static_cast<float>(atan2(offsetY, offsetX) * 180.0 / M_PI);

              // transform from unit circle azimuth to compass azimuth
              azimuth = fmod(450.0f - azimuth, 360.0f);
            }

            const int targetOffset = VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, azimuthPitch);

            static_cast<float*>(azimuthBuffer)[targetOffset] = azimuth;
          }

          if (muteBuffer)
          {
            auto
              muteStartTime = static_cast<uint16_t>(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["mutestarttime"], fileInfo.m_headerEndianness)),
              muteEndTime = static_cast<uint16_t>(SEGY::ReadFieldFromHeader(header, g_traceHeaderFields["muteendtime"], fileInfo.m_headerEndianness));

            // Need to multiply by 2 the result of VoxelIndexToDataIndex, because it gives the two-component offset, and we want the plain offset into a uint16_t buffer
            const int
              targetOffset = 2 * VoxelIndexToDataIndex(fileInfo, targetPrimaryIndex, targetSecondaryIndex, tertiaryIndex, chunkInfo.min, mutePitch);

            static_cast<uint16_t*>(muteBuffer)[targetOffset] = muteStartTime;
            static_cast<uint16_t*>(muteBuffer)[targetOffset + 1] = muteEndTime;
          }
        }
      }
    }

    if (amplitudePage) amplitudePage->Release();
    if (traceFlagPage) traceFlagPage->Release();
    if (segyTraceHeaderPage) segyTraceHeaderPage->Release();
    if (offsetPage) offsetPage->Release();
    if (azimuthPage) azimuthPage->Release();
    if (mutePage) mutePage->Release();

    auto now = std::chrono::steady_clock::now();
    if (now - last_flush > std::chrono::seconds(flushFrequency))
    {
      last_flush = now;
      accessManager.Flush(error);
      if (error.code)
      {
        encountedErrors = true;
        if (!force)
        {
          outputPrinter.printError("VDS","Use -f/--force to continue uploading after upload errors");
          outputPrinter.printError("Failed to upload object", error.string);
        }
        else
        {
          outputPrinter.printWarning("Failed to upload object", error.string);
          error = OpenVDS::Error();
        }
      }
    }
  }

  amplitudeAccessor->Commit();
  traceFlagAccessor->Commit();
  segyTraceHeaderAccessor->Commit();
  if (offsetAccessor) offsetAccessor->Commit();
  if (azimuthAccessor) azimuthAccessor->Commit();
  if (muteAccessor) muteAccessor->Commit();

  dataView.reset();
  traceDataManagers.clear();
  dataViewManagers.clear();

  int max_flush_retry_count = 100;
  int flush_count = 0;
  do {
    error = {};
    accessManager.Flush(error);
    if (error.code)
    {
      outputPrinter.printWarning("Failed to upload object", error.string);
    }
  } while (error.code && flush_count++ < max_flush_retry_count);

  if (error.code || encountedErrors)
  {
    outputPrinter.printPercentage(100.0);
    outputPrinter.printInfo("Done", fmt::format("Completed importing with errors into {}", url));
  }
  else
  {
    outputPrinter.printPercentage(100.0);
    outputPrinter.printInfo("Done", fmt::format("Successfully imported into {}", url));
  }

  //double elapsed = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start_time).count();
  //fmt::print("Elapsed time is {}.\n", elapsed / 1000);
  if (error.code != 0)
  {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
