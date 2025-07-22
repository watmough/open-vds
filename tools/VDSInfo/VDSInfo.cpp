/****************************************************************************
** Copyright 2022 The Open Group
** Copyright 2022 Bluware, Inc.
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

#include <fmt/printf.h>
#include <json/json.h>

#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <SEGYUtils/SEGY.h>

#include <Base64/Base64.h>


#include <cxxopts.hpp>
#include <PrintHelpers.h>
#include <HelpConnection.h>

#ifdef _WIN32
#include <WindowsUnicodeMain.h>
#endif

namespace OpenVDS
{
  extern Json::Value SerializeVolumeDataLayoutDescriptor(VolumeDataLayout const &volumeDataLayout);
  extern Json::Value SerializeAxisDescriptors(VolumeDataLayout const &volumeDataLayout);
  extern Json::Value SerializeChannelDescriptors(VolumeDataLayout const &volumeDataLayout);
}

std::string compressionStringForCompressionMethod(OpenVDS::CompressionMethod method)
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
  }
  return "";
}

static std::string MetadataTypeToString(OpenVDS::MetadataType type)
{
  switch (type)
  {
  case OpenVDS::MetadataType::Int:
    return "Int";
  case OpenVDS::MetadataType::IntVector2:
    return "IntVector2";
  case OpenVDS::MetadataType::IntVector3:
    return "IntVector3";
  case OpenVDS::MetadataType::IntVector4:
    return "IntVector4";
  case OpenVDS::MetadataType::Float:
    return "Float";
  case OpenVDS::MetadataType::FloatVector2:
    return "FloatVector2";
  case OpenVDS::MetadataType::FloatVector3:
    return "FloatVector3";
  case OpenVDS::MetadataType::FloatVector4:
    return "FloatVector4";
  case OpenVDS::MetadataType::Double:
    return "Double";
  case OpenVDS::MetadataType::DoubleVector2:
    return "DoubleVector2";
  case OpenVDS::MetadataType::DoubleVector3:
    return "DoubleVector3";
  case OpenVDS::MetadataType::DoubleVector4:
    return "DoubleVector4";
  case OpenVDS::MetadataType::String:
    return "String";
  case OpenVDS::MetadataType::BLOB:
    return "BLOB";
  }
  return "";
}

template<typename T, size_t N>
Json::Value getJsonFromVector(const OpenVDS::Vector<T, N> &vec)
{
  Json::Value ret;
  for (int i = 0; i < int(N); i++)
  {
    ret.append(vec[i]);
  }
  return ret;
}

Json::Value getJsonFromMetadata(const OpenVDS::MetadataKey &key, OpenVDS::VolumeDataLayout *layout)
{
  Json::Value value;
  value["category"] = key.GetCategory();
  value["name"] = key.GetName();
  value["type"] = MetadataTypeToString(key.GetType());
  switch (key.GetType())
  {
  case OpenVDS::MetadataType::Int:
    value["value"] = layout->GetMetadataInt(key.GetCategory(), key.GetName());
    break;
  case OpenVDS::MetadataType::IntVector2:
    value["value"] = getJsonFromVector(layout->GetMetadataIntVector2(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::IntVector3:
    value["value"] = getJsonFromVector(layout->GetMetadataIntVector3(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::IntVector4:
    value["value"] = getJsonFromVector(layout->GetMetadataIntVector4(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::Float:
    value["value"] = layout->GetMetadataFloat(key.GetCategory(), key.GetName());
    break;
  case OpenVDS::MetadataType::FloatVector2:
    value["value"] = getJsonFromVector(layout->GetMetadataFloatVector2(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::FloatVector3:
    value["value"] = getJsonFromVector(layout->GetMetadataFloatVector3(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::FloatVector4:
    value["value"] = getJsonFromVector(layout->GetMetadataFloatVector4(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::Double:
    value["value"] = layout->GetMetadataDouble(key.GetCategory(), key.GetName());
    break;
  case OpenVDS::MetadataType::DoubleVector2:
    value["value"] = getJsonFromVector(layout->GetMetadataDoubleVector2(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::DoubleVector3:
    value["value"] = getJsonFromVector(layout->GetMetadataDoubleVector3(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::DoubleVector4:
    value["value"] = getJsonFromVector(layout->GetMetadataDoubleVector4(key.GetCategory(), key.GetName()));
    break;
  case OpenVDS::MetadataType::String:
    value["value"] = layout->GetMetadataString(key.GetCategory(), key.GetName());
    break;
  case OpenVDS::MetadataType::BLOB:
    std::vector<uint8_t> blob;
    layout->GetMetadataBLOB(key.GetCategory(), key.GetName(), blob);
    std::vector<char> base64;
    Base64Encode(blob.data(), int64_t(blob.size()), base64);
    std::string strbase64(base64.data(), base64.data() + base64.size());
    value["value"] = strbase64;
  }
  return value;
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


#ifdef UNICODE_MAIN
int UNICODE_MAIN(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  std::string help_info = R"info(VDSInfo - A tool for extracting info from a VDS

Use -H or see online documentation for connection string paramters:
https://osdu.pages.opengroup.org/platform/domain-data-mgmt-services/seismic/open-vds/connection.html

Example for getting the TextHeader from the imported SEGY file autodecoded EBCDIC to ASCII:
VDSInfo --metadata-name TextHeader -b -e -w 80 s3://bluware-jorgen-dev/volve
)info";
  cxxopts::Options options("VDSInfo", help_info);
  options.positional_help("<url>");

  std::vector<std::string> urlarg;
  std::string connection;
  std::string metadataPrintName;
  std::string metadataPrintCategory;

  bool axisDescriptors = false;
  bool channelDescriptors = false;
  bool volumeDataLayout = false;
  bool dimensionGroups = false;
  bool compressionInfo = false;
  bool metaKeys = false;
  bool metadataFirstBlob = false;
  bool metadataAutoDecodeEBCDIC = false;
  bool metadataAll = false;
  int  textDecodeWidth = std::numeric_limits<int>::max();
  bool useJsonOutput = false;
  bool help = false;
  bool helpConnection = false;
  bool version = false;

//connection options
  options.add_option("", "", "urlpos", "Url with vendor specific protocol.", cxxopts::value<std::vector<std::string>>(urlarg), "<string>");
  options.add_option("", "", "connection", "Vendor specific connection string.", cxxopts::value<std::string>(connection), "<string>");

///action
  options.add_option("", "", "axis", "Print axis descriptors.", cxxopts::value<bool>(axisDescriptors), "");
  options.add_option("", "", "channels", "Print channel descriptors.", cxxopts::value<bool>(channelDescriptors), "");
  options.add_option("", "", "layout", "Print layout.", cxxopts::value<bool>(volumeDataLayout), "");
  options.add_option("", "g", "dimension-groups", "List dimension groups.", cxxopts::value<bool>(dimensionGroups), "");
  options.add_option("", "", "compression-info", "Print compression-info.", cxxopts::value<bool>(compressionInfo), "");

  options.add_option("", "", "metadata-all", "Print all of the metadata", cxxopts::value<bool>(metadataAll), "");
  options.add_option("", "", "metadatakeys", "Print metadata keys.", cxxopts::value<bool>(metaKeys), "");
  options.add_option("", "", "metadata-name", "Print metadata matching name.", cxxopts::value<std::string>(metadataPrintName), "<string>");
  options.add_option("", "", "metadata-category", "Print metadata matching category.", cxxopts::value<std::string>(metadataPrintCategory), "<string>");
  options.add_option("", "b", "metadata-firstblob", "Print first blob found.", cxxopts::value<bool>(metadataFirstBlob), "");
  options.add_option("", "e", "metadata-autodecode", "Autodetect EBCDIC and decode to ASCII for blobs.", cxxopts::value<bool>(metadataAutoDecodeEBCDIC), "");
  options.add_option("", "w", "metadata-force-width", "Force output width.", cxxopts::value<int>(textDecodeWidth), "");
  
  options.add_option("", "", "url", "Url with vendor specific protocol or VDS file. (Available as positional argument as well).", cxxopts::value<std::vector<std::string>>(urlarg), "<string>");

  options.add_option("", "", "json-output", "Enable json output.", cxxopts::value<bool>(useJsonOutput), "");
  options.add_option("", "h", "help", "Print this help information", cxxopts::value<bool>(help), "");
  options.add_option("", "H", "help-connection", "Print help information about the connection string", cxxopts::value<bool>(helpConnection), "");
  options.add_option("", "", "version", "Print version information.", cxxopts::value<bool>(version), "");

  options.parse_positional("urlpos");

  OpenVDS::OutputPrinter printer(useJsonOutput, OpenVDS::LogLevel::Info);
  if(argc == 1)
  {
    printer.printInfo("Args", options.help());
    return EXIT_SUCCESS;
  }

  try
  {
    options.parse(argc, argv);
  }
  catch(cxxopts::OptionParseException &e)
  {
    printer.printError("Args", e.what());
    return EXIT_FAILURE;
  }

  if (help)
  {
    printer.printInfo("Args", options.help());
    return EXIT_SUCCESS;
  }
  if (helpConnection)
  {
    printer.printInfo("Args", GetConnectionHelpString());
    return EXIT_SUCCESS;
  }

  if (version)
  {
    printer.printVersion("VDSInfo");
    return EXIT_SUCCESS;
  }

  if (urlarg.empty())
  {
    printer.printError("Args", "Failed - missing url/vdsfile argument");
    return EXIT_FAILURE;
  }
  
  std::string url;
  if(!urlarg.empty()) url = urlarg[0];

  if (!axisDescriptors && !channelDescriptors && !volumeDataLayout && !dimensionGroups && !compressionInfo && !metaKeys && metadataPrintName.empty() && metadataPrintCategory.empty() && !metadataAll)
  {
    axisDescriptors = true;
    channelDescriptors = true;
    volumeDataLayout = true;
    compressionInfo = true;
    metadataAll = true;
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
    printer.printError("VDS", "Could not open VDS", openError.string);
    return EXIT_FAILURE;
  }

  auto layout = OpenVDS::GetLayout(handle);
  if (!layout)
  {
    printer.printError("VDS", "Internal error, no layout");
    return EXIT_FAILURE;
  }
  
  Json::Value root;

  Json::Value layoutJson;
  if (volumeDataLayout)
  {
    layoutJson["layoutDescriptor"] = OpenVDS::SerializeVolumeDataLayoutDescriptor(*layout);
    layoutJson["layoutHash"] = layout->GetLayoutHash();
  }

  if(dimensionGroups)
  {
    auto manager = OpenVDS::GetAccessManager(handle);
    struct { OpenVDS::DimensionsND dimensions; std::string name; } dimensionGroups[] = 
      {
        { OpenVDS::Dimensions_012, "Dimensions_012" },
        { OpenVDS::Dimensions_013, "Dimensions_013" },
        { OpenVDS::Dimensions_014, "Dimensions_014" },
        { OpenVDS::Dimensions_015, "Dimensions_015" },
        { OpenVDS::Dimensions_023, "Dimensions_023" },
        { OpenVDS::Dimensions_024, "Dimensions_024" },
        { OpenVDS::Dimensions_025, "Dimensions_025" },
        { OpenVDS::Dimensions_034, "Dimensions_034" },
        { OpenVDS::Dimensions_035, "Dimensions_035" },
        { OpenVDS::Dimensions_045, "Dimensions_045" },
        { OpenVDS::Dimensions_123, "Dimensions_123" },
        { OpenVDS::Dimensions_124, "Dimensions_124" },
        { OpenVDS::Dimensions_125, "Dimensions_125" },
        { OpenVDS::Dimensions_134, "Dimensions_134" },
        { OpenVDS::Dimensions_135, "Dimensions_135" },
        { OpenVDS::Dimensions_145, "Dimensions_145" },
        { OpenVDS::Dimensions_234, "Dimensions_234" },
        { OpenVDS::Dimensions_235, "Dimensions_235" },
        { OpenVDS::Dimensions_245, "Dimensions_245" },
        { OpenVDS::Dimensions_345, "Dimensions_345" },
        { OpenVDS::Dimensions_01,  "Dimensions_01" },
        { OpenVDS::Dimensions_02,  "Dimensions_02" },
        { OpenVDS::Dimensions_03,  "Dimensions_03" },
        { OpenVDS::Dimensions_04,  "Dimensions_04" },
        { OpenVDS::Dimensions_05,  "Dimensions_05" },
        { OpenVDS::Dimensions_12,  "Dimensions_12" },
        { OpenVDS::Dimensions_13,  "Dimensions_13" },
        { OpenVDS::Dimensions_14,  "Dimensions_14" },
        { OpenVDS::Dimensions_15,  "Dimensions_15" },
        { OpenVDS::Dimensions_23,  "Dimensions_23" },
        { OpenVDS::Dimensions_24,  "Dimensions_24" },
        { OpenVDS::Dimensions_25,  "Dimensions_25" },
        { OpenVDS::Dimensions_34,  "Dimensions_34" },
        { OpenVDS::Dimensions_35,  "Dimensions_35" },
        { OpenVDS::Dimensions_45,  "Dimensions_45" }
      };

    root["dimensionGroups"] = Json::Value(Json::arrayValue);
    for(auto dimensionGroup : dimensionGroups)
    {
      auto status = manager.GetVDSProduceStatus(dimensionGroup.dimensions, 0, 0);
      if(status == OpenVDS::VDSProduceStatus::Normal)
      {
        root["dimensionGroups"].append(dimensionGroup.name);
      }
    }
  }

  if (compressionInfo)
  {
    auto& compressionInfo = layoutJson["compressionInfo"];
    compressionInfo["compressionMethod"] = compressionStringForCompressionMethod(OpenVDS::GetCompressionMethod(handle));
    if(OpenVDS::CompressionMethod_IsWavelet(OpenVDS::GetCompressionMethod(handle)))
    {
      compressionInfo["tolerance"] = OpenVDS::GetCompressionTolerance(handle);
      compressionInfo["compressedSize"] = OpenVDS::GetWaveletCompressedSize(handle);
      compressionInfo["uncompressedSize"] = OpenVDS::GetWaveletUncompressedSize(handle);
      for(auto const& adaptiveLevel : OpenVDS::GetWaveletAdaptiveLevels(handle))
      {
        Json::Value adaptiveLevelJson;
        adaptiveLevelJson["compressedSize"] = adaptiveLevel.compressedSize;
        adaptiveLevelJson["compressionRatio"] = adaptiveLevel.compressionRatio;
        adaptiveLevelJson["compressionTolerance"] = adaptiveLevel.compressionTolerance;
        compressionInfo["adaptiveLevels"].append(adaptiveLevelJson);
      }
    }
  }
  if (axisDescriptors)
  {
    layoutJson["axisDescriptors"] = OpenVDS::SerializeAxisDescriptors(*layout);
  }
  if (channelDescriptors)
  {
    layoutJson["channelDescriptors"] = OpenVDS::SerializeChannelDescriptors(*layout);
  }

  if (layoutJson.size())
    root["layoutInfo"] = layoutJson;

  if (metaKeys)
  {
    Json::Value metaKeysInfo;
    for (auto &key : layout->GetMetadataKeys())
    {
      Json::Value jsonKey;
      jsonKey["type"] = MetadataTypeToString(key.GetType());
      jsonKey["category"] = key.GetCategory();
      jsonKey["name"] = key.GetName();
      metaKeysInfo.append(jsonKey);
    }
    root["metaKeysInfo"] = metaKeysInfo;
  }

  if (metadataPrintName.size() || metadataPrintCategory.size() || metadataAll)
  {
    std::vector<OpenVDS::MetadataKey> to_print;
    std::vector<OpenVDS::MetadataKey> to_print_blobs;
    for (auto &key : layout->GetMetadataKeys())
    {
      if (!metadataAll)
      {
        if (metadataPrintName.size() && metadataPrintName != key.GetName())
          continue;
        if (metadataPrintCategory.size() && metadataPrintCategory != key.GetCategory() && !metadataAll)
          continue;
      }
      if (key.GetType() == OpenVDS::MetadataType::BLOB && metadataFirstBlob && !metadataAll)
        to_print_blobs.push_back(key);
      else
        to_print.push_back(key);
    }

    if (root.empty() && (metadataFirstBlob || to_print.empty()))
    {
      if (to_print_blobs.size())
      {
        auto &key = to_print_blobs.front();
        std::vector<uint8_t> vector;
        layout->GetMetadataBLOB(key.GetCategory(), key.GetName(), vector);
        bool decodeEBCDIC = false;
        if (metadataAutoDecodeEBCDIC)
        {
          decodeEBCDIC = SEGY::autoDetectSEGYTextHeaderIsEBCDIC(vector.data(), vector.size());
        }
        std::string output;

        auto outputSize = 3200;
        if (textDecodeWidth > 0)
          outputSize += 3200 / textDecodeWidth + 1;
        output.resize(outputSize);
        if (decodeEBCDIC)
        {
          auto filled = SEGY::convertSEGYEBCDICHeaderToASCII(vector.data(), vector.size(), &output[0], output.size(), textDecodeWidth);
          output.resize(filled);
        }
        else
        {
          output.resize(std::min(vector.size(), output.size()));
          memcpy(&output[0], vector.data(), output.size());
        }
        fwrite(output.data(), 1, output.size(), stdout);
        fprintf(stderr, "\n");
      }
    }
    else
    {
      Json::Value metaInfo;
      for (auto& metadatakey : to_print)
      {
        metaInfo.append(getJsonFromMetadata(metadatakey, layout));
      }
      if (metaInfo.size())
        root["metadata"] = metaInfo;
    }
  }


  while (root.isObject() && root.size() == 1 && root.begin()->isObject())
    root = *root.begin();

  if (root.size())
  {
    std::string outstring = convertToString(root);
    fwrite(outstring.c_str(), 1, outstring.size(), stdout);
  }


  return EXIT_SUCCESS;
}
