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

#include <OpenVDS/OpenVDS.h>

#include "VDS/VDS.h"

#include "VDS/ParseVDSJson.h"

#include <memory>
#include <map>
#include <set>
#include <limits>
#include <functional>

#include <OpenVDS/VolumeDataAccess.h>

#include "VDS/VolumeDataLayoutImpl.h"
#include "VDS/VolumeDataPageAccessorImpl.h"
#include "VDS/VolumeDataAccessManagerImpl.h"
#include "VDS/VolumeDataRequestProcessor.h"
#include "VDS/ConnectionStringParser.h"
#include "VDS/VolumeDataStoreIOManager.h"
#include "VDS/VolumeDataStoreVDSFile.h"
#include "VDS/GlobalStateImpl.h"
#include "VDS/WaveletTypes.h"
#include "VDS/StringToDouble.h"
#include "VDS/Env.h"
#include "VDS/Url.h"

#include "IO/IOManager.h"
#include "IO/IOManagerTransformer.h"
#include "IO/File.h"

#include <fmt/format.h>

#include <OpenVDS/OpenVDSVersion.h>
#include "VDS/Logging.h"
#include "IO/MsiTokenSource.h"

namespace OpenVDS
{

#ifdef AUTODETECT_CA_CERTIFICATES
static void initOpenSSLCaCertFile()
{
  static bool init_done = false;
  if (init_done)
    return;
  init_done = true;
  bool isCertFileSet = isEnvironmentVariableSet("SSL_CERT_FILE");
  bool isCertDirSet = isEnvironmentVariableSet("SSL_CERT_DIR");
  if (!isCertFileSet && !isCertDirSet)
  {
    static const std::string cert_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/usr/share/ssl/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt",
        "/etc/ssl/cert.pem" };
    for (const auto& cert_path : cert_paths)
    {
      if (File::Exists(cert_path))
      {
        setEnvironmentVariable("SSL_CERT_FILE", cert_path);
      }
    }
  }
}

struct InitOpenSSLCaCertFile
{
  InitOpenSSLCaCertFile()
  {
    initOpenSSLCaCertFile();
  }
};

static InitOpenSSLCaCertFile intSsl;
#endif //AUTODETECT_CA_CERTIFICATES

static std::function<IOManager* (IOManager*)> iomanagerTransformer;

static bool isProtocol(const std::string &str, const std::string &literal)
{
  if (str.size() < literal.size())
    return false;
  std::string protocol(str.data(), str.data() + literal.size());
  std::transform(protocol.begin(), protocol.end(), protocol.begin(), asciitolower);
  return memcmp(protocol.data(), literal.data(), literal.size()) == 0;
}

static std::string removeProtocol(const std::string &str, const std::string &literal)
{
  return std::string(str.begin() + literal.size(), str.end());
}

static bool isTrue(const std::string& str)
{
  auto value = str;
  std::transform(value.begin(), value.end(), value.begin(), asciitolower);
  static const std::string trueValues[] = { "1", "on", "true", "yes" };
  for (auto& trueValue : trueValues)
  {
    if (value == trueValue)
    {
      return true;
    }
  }

  return false;
}

static std::unique_ptr<OpenOptions> createS3OpenOptions(const std::string &url, const std::string &connectionString, Error &error)
{
  std::unique_ptr<AWSOpenOptions> openOptions(new AWSOpenOptions());

  auto connectionStringMap = ParseConnectionString(connectionString, error);
  if (error.code)
  {
    return nullptr;
  }

  if (url.size() < 1)
  {
    error.code = -1;
    error.string = "S3 url is missing bucket";
  }
  auto end = url.data() + url.size();
  auto bucket_end = std::find(url.data(), end, '/');
  openOptions->bucket = std::string(url.data(), bucket_end);

  auto urlKeyBegin = bucket_end + 1;
  if (urlKeyBegin < end)
  {
    openOptions->key = std::string(urlKeyBegin, end);
  }

  for (auto& connectionPair : connectionStringMap)
  {
    if (connectionPair.first == "region")
    {
      openOptions->region = connectionPair.second;
    }
    else if (connectionPair.first == "endpointoverride" || connectionPair.first == "endpoint_override")
    {
      openOptions->endpointOverride = connectionPair.second;
    }
    else if (connectionPair.first == "accesskeyid" || connectionPair.first == "access_key_id"
      || connectionPair.first == "awsaccesskeyid" || connectionPair.first == "aws_access_key_id")
    {
      openOptions->accessKeyId = connectionPair.second;
    }
    else if (connectionPair.first == "secretkey" || connectionPair.first == "secretaccesskey"
      || connectionPair.first == "secret_key" || connectionPair.first == "secret_access_key"
      ||connectionPair.first == "awssecretkey" || connectionPair.first == "awssecretaccesskey"
      || connectionPair.first == "aws_secret_key" || connectionPair.first == "aws_secret_access_key")
    {
      openOptions->secretKey = connectionPair.second;
    }
    else if (connectionPair.first == "sessiontoken" || connectionPair.first == "session_token")
    {
      openOptions->sessionToken = connectionPair.second;
    }
    else if (connectionPair.first == "expiration")
    {
      openOptions->expiration = connectionPair.second;
    }
    else if (connectionPair.first == "connectiontimeoutms" || connectionPair.first == "connection_timeout_ms")
    {
      openOptions->connectionTimeoutMs = strtol(&connectionPair.second[0], nullptr, 10);
      if (openOptions->connectionTimeoutMs == 0)
      {
        error.string = "Invalid connectionTimeoutMs connection string parameter";
        error.code = -1;
        return nullptr;
      }
    }
    else if (connectionPair.first == "requesttimeoutms" || connectionPair.first == "request_timeout_ms")
    {
      openOptions->requestTimeoutMs = strtol(&connectionPair.second[0], nullptr, 10);
      if (openOptions->requestTimeoutMs == 0)
      {
        error.string = "Invalid requestTimeoutMs connection string parameter";
        error.code = -1;
        return nullptr;
      }
    }
    else if (connectionPair.first == "disableinitapi" || connectionPair.first == "disable_init_api")
    {
      openOptions->disableInitApi = isTrue(connectionPair.second);
    }
    else
    {
      error.code = -1;
      error.string = fmt::format("Invalid key \"{}\" in S3 connection string.", connectionPair.first);
      return openOptions;
    }
  }
  return openOptions;
}

static std::unique_ptr<OpenOptions> createAzureOpenOptions(const std::string &url, const std::string &connectionString, Error &error)
{
  std::unique_ptr<AzureOpenOptions> openOptions(new AzureOpenOptions());
  if (url.size() < 1)
  {
    error.code = -1;
    error.string = "Azure url is missing container";
  }
  auto end = url.data() + url.size();
  auto container_end = std::find(url.data(), end, '/');
  openOptions->container = std::string(url.data(), container_end);

  auto urlBlobBegin = container_end + 1;
  if (urlBlobBegin < end)
  {
    openOptions->blob = std::string(urlBlobBegin, end);
  }
  
  auto connectionStringMap = ParseConnectionString(connectionString, error);
  auto bearer_it = connectionStringMap.find("bearertoken");
  if (bearer_it != connectionStringMap.end())
  {
    for (auto& connectionPair : connectionStringMap)
    {
      if (connectionPair.first == "accountname")
        openOptions->accountName = connectionPair.second;
      else if (connectionPair.first == "bearertoken")
      {
        openOptions->bearerToken = connectionPair.second;
      }
      else
      {
        error.code = -1;
        error.string = fmt::format("Invalid key \"{}\" in Azure connection string.", connectionPair.first);
        return nullptr;
      }
    }
    openOptions->bearerToken = bearer_it->second;
    openOptions->accountName = connectionStringMap["accountname"];
  }
  else
  {
    openOptions->connectionString = connectionString;
  }
  return openOptions;
}

static std::unique_ptr<OpenOptions> createAzureSASOpenOptions(const std::string &url, const std::string &connectionString, Error& error)
{
  std::unique_ptr<AzurePresignedOpenOptions> openOptions(new AzurePresignedOpenOptions());
  const char http[] = "https://";
  openOptions->baseUrl.reserve(sizeof(http) - 1 + url.size());
  openOptions->baseUrl.insert(0, http, sizeof(http) - 1);
  openOptions->baseUrl.append(url.data(), url.data() + url.size());

  auto connectionStringMap = ParseConnectionString(connectionString, error);
  for (auto& connectionPair : connectionStringMap)
  {
    if (connectionPair.first == "suffix")
    {
      openOptions->urlSuffix = connectionPair.second;
    }
    else
    {
      error.code = -1;
      error.string = fmt::format("Invalid key \"{}\" in AzureSAS connection string.", connectionPair.first);
      return openOptions;
    }
  }
  return openOptions;
}

const std::set<std::string> TrueWords = { "true", "yes", "on" };
const std::set<std::string> FalseWords = { "false", "no", "off" };

static std::unique_ptr<OpenOptions> createGoogleOpenOptions(const std::string & url, const std::string & connectionString, Error& error)
{
  std::unique_ptr<GoogleOpenOptions> openOptions(new GoogleOpenOptions());
  auto connectionStringMap = ParseConnectionString(connectionString, error);
  if (error.code)
  {
    return nullptr;
  }

  if (url.size() < 1)
  {
    error.code = -1;
    error.string = "GS url is missing bucket";
  }
  auto end = url.data() + url.size();
  auto bucket_end = std::find(url.data(), end, '/');
  openOptions->bucket = std::string(url.data(), bucket_end);

  auto urlKeyBegin = bucket_end + 1;
  if (urlKeyBegin < end)
  {
    openOptions->pathPrefix = std::string(urlKeyBegin, end);
  }

  bool useSignedUrl = false;

  for (auto& connectionPair : connectionStringMap)
  {
    if (connectionPair.first == "token")
    {
      openOptions->credentialsType = GoogleOpenOptions::CredentialsType::AccessToken;
      openOptions->credentials = connectionPair.second;
    }
    else if (connectionPair.first == "credentialsfile")
    {
        openOptions->credentialsType = GoogleOpenOptions::CredentialsType::Path;
        openOptions->credentials = connectionPair.second;
    }
    else if (connectionPair.first == "jsoncredentials")
    {
        openOptions->credentialsType = GoogleOpenOptions::CredentialsType::Json;
        openOptions->credentials = connectionPair.second;
    }
    else if (connectionPair.first == "signedurl")
    {
        std::string value = connectionPair.second;
        std::transform(value.begin(), value.end(), value.begin(), asciitolower);
        if (TrueWords.find(value) != TrueWords.end())
        {
            useSignedUrl = true;
        }
        else if (FalseWords.find(value) == FalseWords.end())
        {
            error.code = -3;
            error.string = fmt::format("Invalid value \"{}\" for SignedUrl in GS connection string.", connectionPair.second);
            return openOptions;
        }
    }
    else
    {
      error.code = -1;
      error.string = fmt::format("Invalid key \"{}\" in GS connection string.", connectionPair.first);
      return openOptions;
    }
  }
  
  if (useSignedUrl)
  {
      if (!openOptions->SetSignedUrl())
      {
          error.code = -2;
          error.string = "SignedUrl option cannot be applied in GS connection string.";
      }
  }

  return openOptions;
}

static std::unique_ptr<OpenOptions> createDMSOpenOptions(const std::string & url, const std::string & connectionString, Error& error)
{
  std::unique_ptr<DMSOpenOptions> openOptions(new DMSOpenOptions());
  auto connectionStringMap = ParseConnectionString(connectionString, error);
  if (error.code)
  {
    return nullptr;
  }

  if (url.size() < 1)
  {
    error.code = -1;
    error.string = "SD url is missing bucket";
  }

  openOptions->datasetPath = url;

  for (auto& connectionPair : connectionStringMap)
  {
    if (connectionPair.first == "sdauthorityurl" || connectionPair.first == "sd_authority_url")
      openOptions->sdAuthorityUrl = connectionPair.second;
    if (connectionPair.first == "sdapikey" || connectionPair.first == "sd_api_key")
      openOptions->sdApiKey = connectionPair.second;
    if (connectionPair.first == "sdtoken" || connectionPair.first == "sd_token")
      openOptions->sdToken = connectionPair.second;
    if (connectionPair.first == "authtokenurl" || connectionPair.first == "auth_token_url")
      openOptions->authTokenUrl = connectionPair.second;
    if (connectionPair.first == "refreshtoken" || connectionPair.first == "refresh_token")
      openOptions->refreshToken = connectionPair.second;
    if (connectionPair.first == "clientid" || connectionPair.first == "client_id")
      openOptions->clientId = connectionPair.second;
    if (connectionPair.first == "clientsecret" || connectionPair.first == "client_secret")
      openOptions->clientSecret = connectionPair.second;
    if (connectionPair.first == "scopes")
      openOptions->scopes = connectionPair.second;
    if (connectionPair.first == "usefilenameforsinglefiledatasets" || connectionPair.first == "use_file_name_for_single_file_datasets"
      || connectionPair.first == "use_filename_for_single_file_datasets")
      openOptions->useFileNameForSingleFileDatasets = isTrue(connectionPair.second);
    if (connectionPair.first == "legaltag" || connectionPair.first == "legal_tag")
      openOptions->legalTag = connectionPair.second;
    if (connectionPair.first == "httpproxy" || connectionPair.first == "http_proxy")
      openOptions->httpProxy = connectionPair.second;
    if (connectionPair.first == "msienable" || connectionPair.first == "msi_enable") {
      Logger logger(static_cast<GlobalStateImpl*>(OpenVDS::GetGlobalState())->logInterface, LogLevel::Info);
      logger.LogInfo("createDMSOpenOptions: MSI ENABLE : TRUE");
      std::string msiToken = OpenVDS::FetchMsiTokenFromImds();
      // Log whether we obtained an MSI token or not for diagnostics.
      if (!msiToken.empty())
      {
        openOptions->sdToken = std::move(msiToken);
        logger.LogInfo("createDMSOpenOptions: obtained MSI token and assigned to sdToken");
      }
      else
      {
        // Explicit MSI was requested but failed -> treat as error and abort open/options creation.
        logger.LogError("createDMSOpenOptions: MSI token not available from IMDS");
        error.code = -1;
        error.string = "MSI token not available from IMDS (msienable=true)";
        return nullptr;
      }
    }
  }
  return openOptions;
}

static std::unique_ptr<OpenOptions> createHttpOpenOptions(const std::string & url, const std::string & connectionString, Error& error)
{
  std::unique_ptr<HttpOpenOptions> openOptions(new HttpOpenOptions(url));
  return openOptions;
}

static std::unique_ptr<OpenOptions> createVDSFileOpenOptions(const std::string & url, const std::string & connectionString, Error& error)
{
  std::unique_ptr<VDSFileOpenOptions> openOptions(new VDSFileOpenOptions());
  openOptions->fileName = URLDecode(url);
  return openOptions;
}

static std::unique_ptr<OpenOptions> createInMemoryOpenOptions(const std::string & url, const std::string & connectionString, Error& error)
{
  std::unique_ptr<InMemoryOpenOptions> openOptions(new InMemoryOpenOptions());
  openOptions->name =  URLDecode(url);
  return openOptions;
}

typedef std::string (*UrlTransformer)(const std::string &url, const std::string &transformer);
typedef std::unique_ptr<OpenOptions>(*OpenOptionsCreator)(const std::string &url, const std::string &connectionString, Error& error);
struct UrlToOpenOptions
{
  std::string protocol;
  UrlTransformer transformer;
  OpenOptionsCreator creator;
};

static const std::vector<UrlToOpenOptions> urlToOpenOptions = {
  {std::string("s3://"), &removeProtocol, &createS3OpenOptions },
  {std::string("az://"), &removeProtocol, &createAzureOpenOptions },
  {std::string("azure://"), &removeProtocol, &createAzureOpenOptions },
  {std::string("azuresas://"), &removeProtocol, &createAzureSASOpenOptions },
  {std::string("gs://"), &removeProtocol, &createGoogleOpenOptions },
  {std::string("sd://"), nullptr, &createDMSOpenOptions },
  {std::string("http://"), nullptr, &createHttpOpenOptions },
  {std::string("https://"), nullptr, &createHttpOpenOptions },
  {std::string("file://"), &removeProtocol, &createVDSFileOpenOptions },
  {std::string("inmemory://"), &removeProtocol, &createInMemoryOpenOptions}
};

static bool GetWaveletAdaptiveInfo(std::string& connectionString, WaveletAdaptiveMode &mode, float &tolerance, float &ratio, Error &error)
{
  std::vector<std::string> waveletAdaptiveKeys;
  waveletAdaptiveKeys.emplace_back("WaveletAdaptiveTolerance");
  waveletAdaptiveKeys.emplace_back("WaveletAdaptiveRatio");
  auto waveletAdaptivePair = RemoveKeyValue(waveletAdaptiveKeys, connectionString, error);
  if (error.code || waveletAdaptivePair.first < 0)
    return false;
  assert(waveletAdaptivePair.first < int(waveletAdaptiveKeys.size()));
  if (waveletAdaptivePair.first == 0)
  {
    tolerance = float(StringToDouble(waveletAdaptivePair.second, error));
    if (error.code)
      error.string = fmt::format("Connection string parameter WaveletAdaptiveTolerance: {}", error.string);
    mode = WaveletAdaptiveMode::Tolerance;
  }
  else if (waveletAdaptivePair.first == 1)
  {
    ratio = float(StringToDouble(waveletAdaptivePair.second, error));
    if (error.code)
      error.string = fmt::format("Connection string parameter WaveletAdaptiveRatio: {}", error.string);
    mode = WaveletAdaptiveMode::Ratio;
  }
  return true;
}

static bool GetRequestThreadCount(std::string& connectionString, int &threadCount, Error& error)
{
  std::vector<std::string> keys;
  keys.emplace_back("RequestThreadCount");
  keys.emplace_back("request_thread_count");
  auto threadCountPair = RemoveKeyValue(keys, connectionString, error);
  if (error.code || threadCountPair.first < 0)
    return false;
  assert(threadCountPair.first < 2);
  try
  {
    threadCount = std::stoi(threadCountPair.second);
  }
  catch (std::invalid_argument const& ex)
  {
    error.code = -1;
    error.string = fmt::format("Failed to parse integer value of connection parameter {}: {}.", keys[threadCountPair.first], ex.what());
    return false;
  }
  catch (std::out_of_range const& ex)
  {
    error.code = -1;
    error.string = fmt::format("Failed to integer value to a valid range for connection parameter {}: {}.", keys[threadCountPair.first], ex.what());
    return false;
  }
  return true;
}

static bool GetLogLevel(std::string& connectionString, LogLevel &level, Error &error)
{
  std::vector<std::string> keys;
  keys.emplace_back("LogLevel");
  keys.emplace_back("log_level");
  auto logLevelPair = RemoveKeyValue(keys, connectionString, error);
  if (error.code || logLevelPair.first < 0)
    return false;
  assert(logLevelPair.first < 2);
  std::string logLevelStr = logLevelPair.second;
  std::transform(logLevelStr.begin(), logLevelStr.end(), logLevelStr.begin(), asciitolower);
  if (logLevelStr == "none")
  {
    level = LogLevel::None;
  }
  else if (logLevelStr == "error")
  {
    level = LogLevel::Error;
  }
  else if (logLevelStr == "warning")
  {
    level = LogLevel::Warning;
  }
  else if (logLevelStr == "info")
  {
    level = LogLevel::Info;
  }
  else if (logLevelStr == "trace")
  {
    level = LogLevel::Trace;
  }
  else
  {
    error.code = -1;
    error.string = fmt::format("Not recognized logLevel {}", logLevelPair.second);
    return false;
  }

  return true;
}

// This function is also used from IOManager.cpp, it might make more sense to move it there
OpenOptions* CreateOpenOptions(StringWrapper urlWrapper, StringWrapper connectionStringWrapper, Error& error)
{
  error = Error();
  std::unique_ptr<OpenOptions> openOptions;
  
  std::string url(urlWrapper.data, urlWrapper.size);
  std::string connectionString(connectionStringWrapper.data, connectionStringWrapper.size);

  WaveletAdaptiveMode adaptiveMode = WaveletAdaptiveMode::Tolerance;
  float adaptiveTolerance = 0.0f;
  float adaptiveRatio = 0.0f;
  bool adaptiveDataSet = GetWaveletAdaptiveInfo(connectionString, adaptiveMode, adaptiveTolerance, adaptiveRatio, error);
  if (error.code)
    return nullptr;

  LogLevel logLevel = LogLevel::Warning;
  bool logLevelSet = GetLogLevel(connectionString, logLevel, error);
  if (error.code)
    return nullptr;

  int requestThreadCount;
  bool requestThreadCountSet = GetRequestThreadCount(connectionString, requestThreadCount, error);
  if (error.code)
    return nullptr;

  for (auto& urlToOpenOption : urlToOpenOptions)
  {
    if (!isProtocol(url, urlToOpenOption.protocol))
      continue;
    auto transformed_url = urlToOpenOption.transformer ? urlToOpenOption.transformer(url, urlToOpenOption.protocol) : url;
    openOptions = urlToOpenOption.creator(transformed_url, connectionString, error);
    break;
  }

  if (error.code)
  {
    return nullptr;
  }

  if (!openOptions)
  {
    openOptions.reset(new VDSFileOpenOptions(url));
  }

  if (openOptions && adaptiveDataSet)
  {
    openOptions->waveletAdaptiveMode = adaptiveMode;
    if (adaptiveMode == WaveletAdaptiveMode::Ratio)
      openOptions->waveletAdaptiveRatio = adaptiveRatio;
    if (adaptiveMode == WaveletAdaptiveMode::Tolerance)
      openOptions->waveletAdaptiveTolerance = adaptiveTolerance;
  }
  
  if (openOptions && logLevelSet)
  {
    openOptions->logLevel = logLevel;
  }

  if (openOptions && requestThreadCountSet)
  {
    openOptions->requestThreadCount = requestThreadCount;
  }

  return openOptions.release();
}

static bool Init(VDS *vds, VolumeDataStore *volumeDataStore, Error& error)
{
  vds->produceStatuses.clear();
  vds->produceStatuses.resize(int(Dimensions_45) + 1, VolumeDataLayer::ProduceStatus_Unavailable);

  vds->volumeDataStore.reset(volumeDataStore);

  std::vector<uint8_t> serializedVolumeDataLayout;
  if(!vds->volumeDataStore->ReadSerializedVolumeDataLayout(serializedVolumeDataLayout, error))
  {
    return false;
  }
  if(!ParseVolumeDataLayout(serializedVolumeDataLayout, vds->layoutDescriptor, vds->axisDescriptors, vds->channelDescriptors, vds->descriptorStrings, vds->metadataContainer, volumeDataStore->IsChannelZipped(), error))
  {
    return false;
  }
  vds->metadataContainer.ClearDirtyFlag();
  CreateVolumeDataLayout(*vds);

  vds->accessManager = std::shared_ptr<VolumeDataAccessManagerImpl>(VolumeDataAccessManagerImpl::Create(*vds), [](VolumeDataAccessManagerImpl *accessManager) { accessManager->Release(); });
  return true;
}

VolumeDataLayer *FindCachedLayer(VDS &vds)
{
  assert(vds.volumeDataLayout.get());

  DimensionGroup
    dimensionGroup = (vds.volumeDataLayout->GetDimensionality() == 2) ? DimensionGroup::DimensionGroup_01 : DimensionGroup::DimensionGroup_012;

  VolumeDataLayer *volumeDataLayer = vds.volumeDataLayout->GetBaseLayer(dimensionGroup, 0);

  if(!volumeDataLayer || volumeDataLayer->GetProduceStatus() != VolumeDataLayer::ProduceStatus_Normal)
  {
    for (int i = DimensionGroup::DimensionGroup_01; i < DimensionGroup::DimensionGroup_3D_Max; i++)
    {
      dimensionGroup = DimensionGroup(i);
      volumeDataLayer = vds.volumeDataLayout->GetBaseLayer(dimensionGroup, 0);
      if(volumeDataLayer && volumeDataLayer->GetProduceStatus() == VolumeDataLayer::ProduceStatus_Normal)
      {
        break;
      }
    }
  }

  return volumeDataLayer;
}

void InitWaveletAdaptiveLoadLevel(VDS &vds, OpenOptions const &options)
{
  VolumeDataLayer *volumeDataLayer = FindCachedLayer(vds);

  if(volumeDataLayer && volumeDataLayer->GetProduceStatus() == VolumeDataLayer::ProduceStatus_Normal)
  {
    CompressionInfo
      compressionInfo = vds.volumeDataStore->GetEffectiveAdaptiveLevel(volumeDataLayer, options.waveletAdaptiveMode, options.waveletAdaptiveTolerance, options.waveletAdaptiveRatio);

    vds.volumeDataLayout->SetCompressionMethod(compressionInfo.GetCompressionMethod());
    vds.volumeDataLayout->SetCompressionTolerance(compressionInfo.GetTolerance());
    vds.volumeDataLayout->SetWaveletAdaptiveLoadLevel(compressionInfo.GetAdaptiveLevel());
  }
}

static int32_t GetInternalCubeSizeLOD0(const VolumeDataLayoutDescriptor &desc)
{
  int32_t size = int32_t(1) << desc.GetBrickSize();

  size -= desc.GetNegativeMargin();
  size -= desc.GetPositiveMargin();

  assert(size > 0);

  return size;
}

static int32_t GetLODCount(const VolumeDataLayoutDescriptor &desc)
{
  return desc.GetLODLevels() + 1;
}

std::string GetLayerName(VolumeDataLayer const &volumeDataLayer)
{
  if(volumeDataLayer.GetChannelIndex() == 0)
  {
    return fmt::format("{}LOD{}", DimensionGroupUtil::GetDimensionGroupName(volumeDataLayer.GetChunkDimensionGroup()), volumeDataLayer.GetLOD());
  }
  else
  {
    assert(std::string(volumeDataLayer.GetVolumeDataChannelDescriptor().GetName()) != "");
    return fmt::format("{}{}LOD{}", volumeDataLayer.GetVolumeDataChannelDescriptor().GetName(), DimensionGroupUtil::GetDimensionGroupName(volumeDataLayer.GetPrimaryChannelLayer().GetChunkDimensionGroup()), volumeDataLayer.GetLOD());
  }
}

void CreateVolumeDataLayout(VDS &vds, CompressionMethod compressionMethod, float compressionTolerance)
{
  int32_t dimensionality = int32_t(vds.axisDescriptors.size());

  // Check if input layouts are valid so we can create a new layout
  if (dimensionality < 2)
  {
    vds.volumeDataLayout.reset();
    return;
  }

  // Update compression settings if they are outside effectively valid ranges,
  if (compressionTolerance < WAVELET_MIN_COMPRESSION_TOLERANCE)
  {
    compressionTolerance = WAVELET_MIN_COMPRESSION_TOLERANCE;
  }

  if (compressionMethod == CompressionMethod::WaveletLossless || compressionMethod == CompressionMethod::WaveletNormalizeBlockLossless)
  {
    compressionTolerance = WAVELET_MIN_COMPRESSION_TOLERANCE;
  }

  const int actualValueRangeChannel = -1;
  const FloatRange actualValueRange = FloatRange(1, 0);
  const int waveletAdaptiveLoadLevel = -1;

  vds.volumeDataLayout.reset(
    new VolumeDataLayoutImpl(
      vds,
      vds.layoutDescriptor,
      vds.axisDescriptors,
      vds.channelDescriptors,
      actualValueRangeChannel,
      actualValueRange,
      VolumeDataHash::GetUniqueHash(),
      compressionMethod,
      compressionTolerance,
      false,
      waveletAdaptiveLoadLevel));

  for(int32_t dimensionGroupIndex = 0; dimensionGroupIndex < DimensionGroup_3D_Max; dimensionGroupIndex++)
  {
    DimensionGroup dimensionGroup = (DimensionGroup)dimensionGroupIndex;

    int32_t nChunkDimensionality = DimensionGroupUtil::GetDimensionality(dimensionGroup);

        // Check if highest dimension in chunk is higher than the highest dimension in the dataset or 1D
    if(DimensionGroupUtil::GetDimension(dimensionGroup, nChunkDimensionality - 1) >= dimensionality ||
       nChunkDimensionality == 1)
    {
      continue;
    }

    assert(nChunkDimensionality == 2 || nChunkDimensionality == 3);

    int32_t physicalLODLevels = (nChunkDimensionality == 3 || vds.layoutDescriptor.IsCreate2DLODs()) ? GetLODCount(vds.layoutDescriptor) : 1;
    int32_t brickSize = GetInternalCubeSizeLOD0(vds.layoutDescriptor) * (nChunkDimensionality == 2 ? vds.layoutDescriptor.GetBrickSizeMultiplier2D() : 1);

    vds.volumeDataLayout->CreateLayers(dimensionGroup, brickSize, physicalLODLevels, vds.produceStatuses[DimensionGroupUtil::GetDimensionsNDFromDimensionGroup(dimensionGroup)]);
  }
}

static void copyMetadataToContainer(MetadataContainer &container, const MetadataReadAccess &readAccess)
{
  std::unordered_set<std::string> categories;
  for (auto &key : readAccess.GetMetadataKeys())
  {
    categories.insert(key.GetCategory());
  }
  for (auto &category : categories)
  {
    container.CopyMetadata(category.c_str(), &readAccess);
  }
}

static bool Init(VDS *vds, VolumeDataStore* volumeDataStore, VolumeDataLayoutDescriptor const &layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const &metadata, CompressionMethod compressionMethod, float compressionTolerance, Error &error)
{
  if(!VolumeDataStore::IsCompressionMethodSupported(compressionMethod))
  {
    error.code = -1;
    error.string = "Unsupported compression method";
    return false;
  }

  vds->produceStatuses.clear();
  vds->produceStatuses.resize(int(Dimensions_45) + 1, VolumeDataLayer::ProduceStatus_Unavailable);

  vds->volumeDataStore.reset(volumeDataStore);

  if (layoutDescriptor.GetBrickSize() < VolumeDataLayoutDescriptor::BrickSize_32 || layoutDescriptor.GetBrickSize() > VolumeDataLayoutDescriptor::BrickSize_4096)
  {
    error.code = -1;
    error.string = "Invalid brick size";
    return false;
  }

  if (layoutDescriptor.GetNegativeMargin() < 0 || layoutDescriptor.GetNegativeMargin() >= (1 << layoutDescriptor.GetBrickSize()) / 2)
  {
    error.code = -1;
    error.string = "Invalid negative margin";
    return false;
  }

  if (layoutDescriptor.GetPositiveMargin() < 0 || layoutDescriptor.GetPositiveMargin() >= (1 << layoutDescriptor.GetBrickSize()) / 2)
  {
    error.code = -1;
    error.string = "Invalid positive margin";
    return false;
  }

  if (layoutDescriptor.GetBrickSizeMultiplier2D() <= 0 || layoutDescriptor.GetBrickSizeMultiplier2D() * (1 << layoutDescriptor.GetBrickSize()) > 4096)
  {
    error.code = -1;
    error.string = "Invalid brick size 2D multiplier";
    return false;
  }

  if (layoutDescriptor.GetLODLevels() < VolumeDataLayoutDescriptor::LODLevels_None || layoutDescriptor.GetLODLevels() > VolumeDataLayoutDescriptor::LODLevels_12)
  {
    error.code = -1;
    error.string = "Invalid LOD count";
    return false;
  }

  if (layoutDescriptor.GetFullResolutionDimension() < -1 || layoutDescriptor.GetFullResolutionDimension() >= int(axisDescriptors.size))
  {
    error.code = -1;
    error.string = "Full resolution dimension must be a valid dimension or -1";
    return false;
  }

  vds->layoutDescriptor = layoutDescriptor;

  DescriptorStringContainer &
    descriptorStrings = vds->descriptorStrings;

  for(size_t i = 0; i < axisDescriptors.size; i++)
  {
    auto &axisDescriptor = axisDescriptors.data[i];

    if (!axisDescriptor.GetName())
    {
      error.code = -1;
      error.string = "Axis descriptor has not been initialized";
      return false;
    }

    if (axisDescriptor.GetNumSamples() < 1 || axisDescriptor.GetNumSamples() > 1000000)
    {
      error.code = -1;
      error.string = "Axis descriptor has invalid number of samples";
      return false;
    }

    if (!std::isfinite(axisDescriptor.GetCoordinateMin()) || !std::isfinite(axisDescriptor.GetCoordinateMax()))
    {
      error.code = -1;
      error.string = "Axis coordinates must be finite numbers";
      return false;
    }

    vds->axisDescriptors.push_back(VolumeDataAxisDescriptor(axisDescriptor.GetNumSamples(), descriptorStrings.Add(axisDescriptor.GetName()), descriptorStrings.Add(axisDescriptor.GetUnit()), axisDescriptor.GetCoordinateMin(), axisDescriptor.GetCoordinateMax()));
  }

  std::unordered_set<std::string>
    channelNames;

  for(size_t i = 0; i < channelDescriptors.size; i++)
  {
    auto &channelDescriptor = channelDescriptors.data[i];

    if (!channelDescriptor.GetName())
    {
      error.code = -1;
      error.string = "Channel descriptor has not been been initialized";
      return false;
    }

    if(channelNames.find(channelDescriptor.GetName()) != channelNames.end())
    {
      error.code = -1;
      error.string = "Channel name must be unique";
      return false;
    }
    channelNames.insert(channelDescriptor.GetName());

    if(i == 0)
    {
      if(!channelDescriptor.IsRenderable())
      {
        error.code = -1;
        error.string = "The primary channel must be renderable";
        return false;
      }
      if(channelDescriptor.GetMapping() != VolumeDataMapping::Direct)
      {
        error.code = -1;
        error.string = "The primary channel must be direct mapped";
        return false;
      }
    }
    else
    {
      if (std::string(channelDescriptor.GetName()).empty())
      {
        error.code = -1;
        error.string = "Additional channel descriptors must be named";
        return false;
      }

      if(channelDescriptor.GetMapping() == VolumeDataMapping::Direct)
      {
        if(channelDescriptor.GetMappedValueCount() != 1)
        {
          error.code = -1;
          error.string = "Direct mapped channels must have 1 mapped value";
          return false;
        }
      }
      else if(channelDescriptor.GetMapping() == VolumeDataMapping::PerTrace)
      {
        if(channelDescriptor.GetMappedValueCount() < 1)
        {
          error.code = -1;
          error.string = "Per-Trace mapped channels must have at least 1 mapped value";
          return false;
        }
      }
      else
      {
        error.code = -1;
        error.string = "Unknown channel mapping";
        return false;
      }
    }

    if (channelDescriptor.GetFormat() == VolumeDataFormat::Format_1Bit && channelDescriptor.IsUseNoValue())
    {
      error.code = -1;
      error.string = "1-bit channels cannot use no value";
      return false;
    }

    if (!std::isfinite(channelDescriptor.GetValueRangeMin()) || !std::isfinite(channelDescriptor.GetValueRangeMax()))
    {
      error.code = -1;
      error.string = "Channel value range must be finite numbers";
      return false;
    }

    if (channelDescriptor.GetValueRangeMin() > channelDescriptor.GetValueRangeMax())
    {
      error.code = -1;
      error.string = "Invalid channel value range (min > max)";
      return false;
    }

    VolumeDataChannelDescriptor::Flags flags = VolumeDataChannelDescriptor::Default;

    if(channelDescriptor.IsDiscrete())                     flags = flags | VolumeDataChannelDescriptor::DiscreteData;
    if(!channelDescriptor.IsAllowLossyCompression())       flags = flags | VolumeDataChannelDescriptor::NoLossyCompression;
    if(channelDescriptor.IsUseZipForLosslessCompression()) flags = flags | VolumeDataChannelDescriptor::NoLossyCompressionUseZip;
    if(!channelDescriptor.IsRenderable())                  flags = flags | VolumeDataChannelDescriptor::NotRenderable;

    if(channelDescriptor.IsUseNoValue())
    {
      vds->channelDescriptors.push_back(VolumeDataChannelDescriptor(channelDescriptor.GetFormat(), channelDescriptor.GetComponents(), descriptorStrings.Add(channelDescriptor.GetName()), descriptorStrings.Add(channelDescriptor.GetUnit()), channelDescriptor.GetValueRangeMin(), channelDescriptor.GetValueRangeMax(), channelDescriptor.GetMapping(), channelDescriptor.GetMappedValueCount(), flags, channelDescriptor.GetNoValue(), channelDescriptor.GetIntegerScale(), channelDescriptor.GetIntegerOffset()));
    }
    else
    {
      vds->channelDescriptors.push_back(VolumeDataChannelDescriptor(channelDescriptor.GetFormat(), channelDescriptor.GetComponents(), descriptorStrings.Add(channelDescriptor.GetName()), descriptorStrings.Add(channelDescriptor.GetUnit()), channelDescriptor.GetValueRangeMin(), channelDescriptor.GetValueRangeMax(), channelDescriptor.GetMapping(), channelDescriptor.GetMappedValueCount(), flags, channelDescriptor.GetIntegerScale(), channelDescriptor.GetIntegerOffset()));
    }
  }

  copyMetadataToContainer(vds->metadataContainer, metadata);

  CreateVolumeDataLayout(*vds, compressionMethod, compressionTolerance);

  vds->produceStatuses.clear();
  vds->produceStatuses.resize(int(Dimensions_45) + 1, VolumeDataLayer::ProduceStatus_Unavailable);

  if (!vds->volumeDataStore->WriteSerializedVolumeDataLayout(SerializeVolumeDataLayout(*vds), error))
    return false;
  if (!vds->volumeDataStore->SerializeAndWriteLayerStatus(error))
    return false;

  vds->accessManager = std::shared_ptr<VolumeDataAccessManagerImpl>(VolumeDataAccessManagerImpl::Create(*vds), [](VolumeDataAccessManagerImpl *accessManager) { accessManager->Release(); });
  return true;
}

void SetIoManagerTransformer(std::function<IOManager* (IOManager*)> transformer)
{
  iomanagerTransformer = transformer;
}

class OpenVDSInterfaceImpl : public OpenVDSInterface
{
  friend OpenVDSInterface &GetOpenVDSInterface(const char* version);
  OpenVDSInterfaceImpl() {}
  ~OpenVDSInterfaceImpl() {}

  VDSHandle                 Open(const OpenOptions& options, Error &error);
  VDSHandle                 Create(const OpenOptions& options, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, Error &error);

public:
  class ErrorGuard : public Error
  {
    ErrorHandler m_errorHandler;
    Error       *m_errorPtr;
  public:
    ErrorGuard(ErrorHandler errorHandler, Error *errorPtr) : Error(), m_errorHandler(errorHandler), m_errorPtr(errorPtr) {}
   ~ErrorGuard() noexcept(false) { m_errorHandler(m_errorPtr, code, string.c_str()); }
  };

  OpenOptions*              CreateOpenOptions(StringWrapper url, StringWrapper connectionString, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  bool                      IsSupportedProtocol(StringWrapper url) final override;
  VDSHandle                 Open(StringWrapper url, StringWrapper connectionString, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 OpenWithAdaptiveCompressionTolerance(StringWrapper url, StringWrapper connectionString, float waveletAdaptiveTolerance, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 OpenWithAdaptiveCompressionRatio(StringWrapper url, StringWrapper connectionString, float waveletAdaptiveRatio, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 Open(const OpenOptions& options, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 Open(IOManager*ioManager, LogLevel logLevel, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  bool                      IsCompressionMethodSupported(CompressionMethod compressionMethod) final override;
  VDSHandle                 Create(StringWrapper url, StringWrapper connectionString, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 Create(const OpenOptions& options, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VDSHandle                 Create(IOManager* ioManager, VolumeDataLayoutDescriptor const &layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const &metadata, CompressionMethod compressionMethod, float compressionTolerance, LogLevel logLevel, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  VolumeDataLayout         *GetLayout(VDSHandle handle) final override;
  IVolumeDataAccessManager *GetAccessManagerInterface(VDSHandle handle) final override;
  MetadataWriteAccess      *GetMetadataWriteAccessInterface(VDSHandle handle) final override;
  CompressionMethod         GetCompressionMethod(VDSHandle handle) final override;
  float                     GetCompressionTolerance(VDSHandle handle) final override;
  void                      Close(VDSHandle handle, bool flush) final override;
  void                      Close(VDSHandle handle, bool flush, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  void                      RetryableClose(VDSHandle handle, bool flush) final override;
  void                      RetryableClose(VDSHandle handle, bool flush, ErrorHandler errorHandler, Error *errorPtr=nullptr) final override;
  GlobalState              *GetGlobalState() final override;
  int64_t                   GetWaveletCompressedSize(VDSHandle handle) final override;
  int64_t                   GetWaveletUncompressedSize(VDSHandle handle) final override;
  void                      GetWaveletAdaptiveLevels(VDSHandle handle, WaveletAdaptiveLevelsHandler WaveletAdaptiveLevelsHandler, std::vector<WaveletAdaptiveLevel> *waveletAdaptiveLevelsVector) final override;

  const char               *GetOpenVDSName() final override;
  const char               *GetOpenVDSVersion() final override;
  void                      GetOpenVDSVersion(int &major, int &minor, int &patch) final override;
  const char               *GetOpenVDSRevision() final override;

  static OpenVDSInterfaceImpl &GetInstance()
  {
    static OpenVDSInterfaceImpl instance;
    return instance;
  }
};

OpenOptions* OpenVDSInterfaceImpl::CreateOpenOptions(StringWrapper urlWrapper, StringWrapper connectionStringWrapper, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  return OpenVDS::CreateOpenOptions(urlWrapper, connectionStringWrapper, error);
}

bool OpenVDSInterfaceImpl::IsSupportedProtocol(StringWrapper url)
{
  std::string u(url.data, url.size);
  for (auto& urlToOpenOpiton : urlToOpenOptions)
  {
    if (isProtocol(u, urlToOpenOpiton.protocol))
      return true;
  }
  return false;
}

VDSHandle OpenVDSInterfaceImpl::Open(StringWrapper url, StringWrapper connectionString, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<OpenOptions> openOptions(OpenVDS::CreateOpenOptions(url, connectionString, error));
  if (error.code || !openOptions)
    return nullptr;

  return Open(*(openOptions.get()), error);
}

VDSHandle OpenVDSInterfaceImpl::OpenWithAdaptiveCompressionTolerance(StringWrapper url, StringWrapper connectionString, float waveletAdaptiveTolerance, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<OpenOptions> openOptions(OpenVDS::CreateOpenOptions(url, connectionString, error));
  if (error.code || !openOptions)
    return nullptr;

  openOptions->waveletAdaptiveMode = WaveletAdaptiveMode::Tolerance;
  openOptions->waveletAdaptiveTolerance = waveletAdaptiveTolerance;

  return Open(*(openOptions.get()), error);
}

VDSHandle OpenVDSInterfaceImpl::OpenWithAdaptiveCompressionRatio(StringWrapper url, StringWrapper connectionString, float waveletAdaptiveRatio, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<OpenOptions> openOptions(OpenVDS::CreateOpenOptions(url, connectionString, error));
  if (error.code || !openOptions)
    return nullptr;

  openOptions->waveletAdaptiveMode = WaveletAdaptiveMode::Ratio;
  openOptions->waveletAdaptiveRatio = waveletAdaptiveRatio;

  return Open(*(openOptions.get()), error);
}

VDSHandle OpenVDSInterfaceImpl::Open(IOManager *ioManager, LogLevel loglevel, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<VDS> ret(new VDS(-1, loglevel));

  if(Init(ret.get(), new VolumeDataStoreIOManager(*ret, ioManager, IOManager::ReadOnly), error))
  {
    return ret.release();
  }
  else
  {
    return nullptr;
  }
}

VDS *OpenVDSInterfaceImpl::Open(const OpenOptions &options, Error &error)
{
  std::unique_ptr<VDS> ret(new VDS(options.requestThreadCount, options.logLevel));
  std::unique_ptr<VolumeDataStore> volumeDataStore;

  if(options.connectionType != OpenOptions::VDSFile)
  {
    std::unique_ptr<IOManager> ioManager(IOManager::CreateIOManager(options, IOManager::AccessPattern::ReadOnly, error));
    if (error.code)
      return nullptr;

    if (iomanagerTransformer)
      ioManager.reset(iomanagerTransformer(ioManager.release()));

    volumeDataStore.reset(new VolumeDataStoreIOManager(*ret, ioManager.release(), IOManager::ReadOnly));
  }
  else
  {
    const VDSFileOpenOptions &fileOptions = static_cast<const VDSFileOpenOptions &>(options);
    volumeDataStore.reset(new VolumeDataStoreVDSFile(*ret, fileOptions.fileName, VolumeDataStoreVDSFile::ReadOnly, error));
    if (error.code)
      return nullptr;
  }

  if(Init(ret.get(), volumeDataStore.release(), error))
  {
    assert(ret.get());
    InitWaveletAdaptiveLoadLevel(*ret.get(), options);
    return ret.release();
  }
  else
  {
    return nullptr;
  }
}

VDS *OpenVDSInterfaceImpl::Open(const OpenOptions &options, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  return Open(options, error);
}

VolumeDataLayout *OpenVDSInterfaceImpl::GetLayout(VDS *vds)
{
  if (!vds)
    return nullptr;
  return vds->volumeDataLayout.get();
}

IVolumeDataAccessManager *OpenVDSInterfaceImpl::GetAccessManagerInterface(VDS *vds)
{
  if (!vds)
    return nullptr;
  return vds->accessManager.get();
}

MetadataWriteAccess *OpenVDSInterfaceImpl::GetMetadataWriteAccessInterface(VDS *vds)
{
  if (!vds)
    return nullptr;
  Error error;
  if (!vds->volumeDataStore->EnableWriting(error))
  {
    vds->logger.LogError(fmt::format("Failed to enable writing on GetMetadataWriteAccessInterface: {}.", error.string));
    return nullptr;
  }
  return &vds->metadataContainer;
}

bool OpenVDSInterfaceImpl::IsCompressionMethodSupported(CompressionMethod compressionMethod)
{
  return VolumeDataStore::IsCompressionMethodSupported(compressionMethod);
}

VDSHandle OpenVDSInterfaceImpl::Create(IOManager *ioManager, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, LogLevel logLevel, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<VDS> ret(new VDS(-1, logLevel));

  if(Init(ret.get(), new VolumeDataStoreIOManager(*ret, ioManager, IOManager::Create), layoutDescriptor, axisDescriptors, channelDescriptors, metadata, compressionMethod, compressionTolerance, error))
  {
    return ret.release();
  }
  else
  {
    return nullptr;
  }
}

VDSHandle OpenVDSInterfaceImpl::Create(StringWrapper url, StringWrapper connectionString, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  std::unique_ptr<OpenOptions> openOptions(OpenVDS::CreateOpenOptions(url, connectionString, error));
  if (error.code || !openOptions)
    return nullptr;

  return Create(*openOptions, layoutDescriptor, axisDescriptors, channelDescriptors, metadata, compressionMethod, compressionTolerance, error);
}

VDSHandle OpenVDSInterfaceImpl::Create(const OpenOptions& options, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, Error &error)
{
  std::unique_ptr<VDS> ret(new VDS(options.requestThreadCount, options.logLevel));
  std::unique_ptr<VolumeDataStore> volumeDataStore;

  if(options.connectionType != OpenOptions::VDSFile)
  {
    std::unique_ptr<IOManager> ioManager(IOManager::CreateIOManager(options, IOManager::AccessPattern::Create, error));
    if (error.code)
      return nullptr;

    volumeDataStore.reset(new VolumeDataStoreIOManager(*ret, ioManager.release(), IOManager::Create));
  }
  else
  {
    const VDSFileOpenOptions &fileOptions = static_cast<const VDSFileOpenOptions &>(options);
    volumeDataStore.reset(new VolumeDataStoreVDSFile(*ret, fileOptions.fileName, VolumeDataStoreVDSFile::Create, error));
    if (error.code)
      return nullptr;
  }

  if(Init(ret.get(), volumeDataStore.release(), layoutDescriptor, axisDescriptors, channelDescriptors, metadata, compressionMethod, compressionTolerance, error))
  {
    return ret.release();
  }
  else
  {
    return nullptr;
  }
}

VDSHandle OpenVDSInterfaceImpl::Create(const OpenOptions& options, VolumeDataLayoutDescriptor const& layoutDescriptor, VectorWrapper<VolumeDataAxisDescriptor> axisDescriptors, VectorWrapper<VolumeDataChannelDescriptor> channelDescriptors, MetadataReadAccess const& metadata, CompressionMethod compressionMethod, float compressionTolerance, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);
  return Create(options, layoutDescriptor, axisDescriptors, channelDescriptors, metadata, compressionMethod, compressionTolerance, error);
}

CompressionMethod OpenVDSInterfaceImpl::GetCompressionMethod(VDS *vds)
{
  if (!vds)
    return CompressionMethod::None;
  return vds->volumeDataLayout->GetCompressionMethod();
}

float OpenVDSInterfaceImpl::GetCompressionTolerance(VDS *vds)
{
  if (!vds)
    return 0;
  return vds->volumeDataLayout->GetCompressionTolerance();
}

void OpenVDSInterfaceImpl::Close(VDS *vds, bool flush)
{
  if (!vds)
    return;
  Error flushError;
  if (flush)
    vds->accessManager->Flush([](Error* error, int errorCode, const char* errorMessage) { error->code = errorCode; error->string = errorMessage; }, &flushError);
  Error error;
  bool success = vds->volumeDataStore->Close(error);
  vds->accessManager->Invalidate();
  delete vds;
  if (flushError.code)
  {
    throw InvalidOperation(fmt::format("Flush in Close failed: {}", flushError.string).c_str());
  }
  if(!success)
  {
    throw InvalidOperation(fmt::format("Close failed: {}", error.string).c_str());
  }
}

void OpenVDSInterfaceImpl::Close(VDS *vds, bool flush, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);

  if (!vds)
    return;

  if (flush)
  {
    vds->accessManager->Flush([](Error* error, int errorCode, const char* errorMessage) { error->code = errorCode; error->string = errorMessage; }, &error);
  }

  if (error.code)
  {
    Error throwawayError;
    vds->volumeDataStore->Close(throwawayError);
  }
  else
  {
    vds->volumeDataStore->Close(error);
  }
  vds->accessManager->Invalidate();
  delete vds;
}

void OpenVDSInterfaceImpl::RetryableClose(VDS *vds, bool flush)
{
  if (!vds)
    return;
  Error error;
  if (flush)
  {
    vds->accessManager->Flush([](Error* error, int errorCode, const char* errorMessage) { error->code = errorCode; error->string = errorMessage; }, &error);
    throw InvalidOperation(fmt::format("Flush in Close failed: {}", error.string).c_str());
  }

  bool success = vds->volumeDataStore->Close(error);
  if(!success)
  {
    throw InvalidOperation(fmt::format("Close failed: {}", error.string).c_str());
  }
  vds->accessManager->Invalidate();
  delete vds;
}

void OpenVDSInterfaceImpl::RetryableClose(VDS *vds, bool flush, ErrorHandler errorHandler, Error *errorPtr)
{
  ErrorGuard error(errorHandler, errorPtr);

  if (!vds)
    return;

  if (flush)
  {
    vds->accessManager->Flush([](Error* error, int errorCode, const char* errorMessage) { error->code = errorCode; error->string = errorMessage; }, &error);
    if (error.code)
      return;
  }

  if(vds->volumeDataStore->Close(error))
  {
    vds->accessManager->Invalidate();
    delete vds;
  }
}

GlobalState *OpenVDSInterfaceImpl::GetGlobalState()
{
  static GlobalStateImpl globalState;
  return &globalState;
}

int64_t
OpenVDSInterfaceImpl::GetWaveletCompressedSize(VDS *vds)
{
  float compressionTolerance = 0;
  int64_t uncompressedSize = 0 ;
  int64_t adaptiveLevelSizes[WAVELET_ADAPTIVE_LEVELS] = {};

  if(vds)
  {
    vds->volumeDataStore->GetWaveletAdaptiveLevelSizes(FindCachedLayer(*vds), compressionTolerance, uncompressedSize, adaptiveLevelSizes);
  }

  return adaptiveLevelSizes[0];
}

int64_t
OpenVDSInterfaceImpl::GetWaveletUncompressedSize(VDS *vds)
{
  float compressionTolerance = 0;
  int64_t uncompressedSize = 0 ;
  int64_t adaptiveLevelSizes[WAVELET_ADAPTIVE_LEVELS] = {};

  if(vds)
  {
    vds->volumeDataStore->GetWaveletAdaptiveLevelSizes(FindCachedLayer(*vds), compressionTolerance, uncompressedSize, adaptiveLevelSizes);
  }

  return uncompressedSize;
}

void
OpenVDSInterfaceImpl::GetWaveletAdaptiveLevels(VDS *vds, WaveletAdaptiveLevelsHandler waveletAdaptiveLevelsHandler, std::vector<WaveletAdaptiveLevel> *waveletAdaptiveLevelsVector)
{
  float compressionTolerance = 0;
  int64_t uncompressedSize = 0 ;
  int64_t adaptiveLevelSizes[WAVELET_ADAPTIVE_LEVELS] = {};

  if(vds)
  {
    vds->volumeDataStore->GetWaveletAdaptiveLevelSizes(FindCachedLayer(*vds), compressionTolerance, uncompressedSize, adaptiveLevelSizes);

    for(int level = 1; level < WAVELET_ADAPTIVE_LEVELS; level++)
    {
      if(!adaptiveLevelSizes[level]) break;

      waveletAdaptiveLevelsHandler(waveletAdaptiveLevelsVector, compressionTolerance, (float)uncompressedSize / (float)adaptiveLevelSizes[level], adaptiveLevelSizes[level]);
      compressionTolerance *= 2.0f;
    }
  }
}

const char *OpenVDSInterfaceImpl::GetOpenVDSName()
{
  return OPENVDS_PROJECT_NAME;
}

const char *OpenVDSInterfaceImpl::GetOpenVDSVersion()
{
  return OPENVDS_VERSION;
}

void OpenVDSInterfaceImpl::GetOpenVDSVersion(int &major, int &minor, int &patch)
{
  major = OPENVDS_MAJOR_VERSION;
  minor = OPENVDS_MINOR_VERSION;
  patch = OPENVDS_PATCH_VERSION;
}

const char *OpenVDSInterfaceImpl::GetOpenVDSRevision()
{
  return OPENVDS_REVISION;
}

static OpenVDSInterface *openVDSInterfacePointer = &OpenVDSInterfaceImpl::GetInstance();

#define OPENVDS_STRINGIZE_UNEXPANDED(N) #N
#define OPENVDS_STRINGIZE(N) OPENVDS_STRINGIZE_UNEXPANDED(N)

OpenVDSInterface &GetOpenVDSInterface(const char* version)
{
  static const char expectedVersion[] = OPENVDS_STRINGIZE(OPENVDS_MAJOR_VERSION) "." OPENVDS_STRINGIZE(OPENVDS_MINOR_VERSION) ".";
  if(strncmp(version, expectedVersion, sizeof(expectedVersion) - 1) != 0) throw InvalidOperation("OpenVDS interface version mismatch");

  return *openVDSInterfacePointer;
}

void SetOpenVDSInterface(OpenVDSInterface &openVDSInterface)
{
  openVDSInterfacePointer = &openVDSInterface;
}

}
