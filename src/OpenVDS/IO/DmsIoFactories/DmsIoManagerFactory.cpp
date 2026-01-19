#if defined(_MSC_VER) && _MSC_VER <= 1900
#define _SCL_SECURE_NO_WARNINGS 1
#endif

#define USE_IMPORT_EXPORT 1

#include "DmsIoManagerFactory.h"

#include <json_cpp_include.h>
#include "Base64/Base64.h"
#include <fmt/chrono.h>

#include "IO/SDPath.h"
#include "VDS/Url.h"

#include <chrono>
#include <random>
#include <sstream>
#ifndef OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER
#include "AzureDmsIoManagerFactory.h"
#endif
#ifndef OPENVDS_NO_AWS_IOMANAGER
#include "AwsDmsIoManagerFactory.h"
#include "AnthosDmsIoManagerFactory.h"
#include "IbmDmsIoManagerFactory.h"
#endif //OPENVDS_NO_AWS_IOMANAGER
#ifndef OPENVDS_NO_GCP_IOMANAGER
#include "GcpDmsIoManagerFactory.h"
#endif

#include "VDS/Env.h"

namespace OpenVDS
{
static std::vector<std::string> split(const std::string& text, char sep) 
{
  std::vector<std::string> tokens;
  std::size_t start = 0, end = 0;
  while ((end = text.find(sep, start)) != std::string::npos) {
    tokens.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  tokens.push_back(text.substr(start));
  return tokens;
}

DmsManager::DmsManager(const std::string& authorityUrl, const std::string& appKey, CurlHandler &curlHandler, Logger& logger)
  : m_authorityUrl(authorityUrl.empty() ? getStringEnvironmentVariable("SD_SVC_URL") : authorityUrl)
  , m_appKey(appKey.empty() ? getStringEnvironmentVariable("SD_SVC_API_KEY") : appKey)
  , m_curlHandler(curlHandler)
  , m_logger(logger)
  , m_authProviderCallback(nullptr)
  , m_authProviderCallbackData(nullptr)
{}

void DmsManager::setSdTokenDefaultExpiry()
{
  m_logger.LogWarning("Failed to parse sd token to find expiry, assuming one hour.");
  m_sdTokenExpiry = std::chrono::system_clock::now() + std::chrono::hours(1);
}
bool DmsManager::ensureSdToken(Error &error)
{
  if (m_authProviderCallback && m_sdTokenExpiry < std::chrono::system_clock::now() + std::chrono::minutes(1))
  {
    try
    {
      m_sdToken = m_authProviderCallback(m_authProviderCallbackData);
    }
    catch (const std::runtime_error& ex)
    {
      error.code = -1;
      error.string = ex.what();
      return false;
    }

    if (m_sdToken.empty())
    {
      error.code = -1;
      error.string = "Got empty sdToken from AuthProviderCallback.";
      return false;
    }

    auto tokens = split(m_sdToken, '.');
    if (tokens.size() < 3)
    {
      setSdTokenDefaultExpiry();
      return true;
    }

    auto payload = tokens[1];
    std::vector<unsigned char> decodedData;
    if (!Base64Decode(payload.c_str(), payload.size(), decodedData))
    {
      setSdTokenDefaultExpiry();
      return true;
    }
    Json::Value root;
    if (!ParseJSONFromBuffer(decodedData, root, error))
    {
      error = OpenVDS::Error();
      setSdTokenDefaultExpiry();
      return true;
    }
    try
    {
      if (!root.isMember("exp"))
      {
        setSdTokenDefaultExpiry();
        return true;
      }
      m_sdTokenExpiry = std::chrono::system_clock::from_time_t(time_t(root["exp"].asUInt64()));
    }
    catch (Json::Exception& e)
    {
      (void)e;
      setSdTokenDefaultExpiry();
      return true;
    }
  }
  return true;
}

void DmsManager::addHeaders(std::vector<std::string>& headers)
{
  headers.emplace_back("Content-Type: application/json");
  headers.emplace_back("Accept: application/json");
  headers.emplace_back(fmt::format("appkey: {}", m_appKey));
  headers.emplace_back(fmt::format("authorization: Bearer {}", m_sdToken));
}

static std::string gen_lock_id(IOManager::AccessPattern accessPattern)
{
    const char hex_characters[]={'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<int> dist(0, 15);

    std::string ret;
    ret.reserve(16);
    ret.push_back(accessPattern == IOManager::ReadOnly ? 'R' : 'W');
    for (int i = 0; i < 15; i++)
    {
      ret.push_back(hex_characters[dist(mt)]);
    }
    return ret;
}

DmsDataset::DmsDataset(DmsManager& manager, const std::string url, Error &error, const std::string& legalTag)
  : m_manager(manager)
  , m_url(url)
  , m_accessPattern(IOManager::ReadOnly)
  , m_opened(false)
  , m_nobjects(0)
{
  if (!getSdPath(url, m_tenant, m_subproject, m_path, m_dataset, error))
    return;

  if (m_path.empty())
    m_path = '/';

  if (!legalTag.empty() && m_legalTag.empty())
    m_legalTag = legalTag;
}

bool DmsDataset::registerDataset(std::vector<std::pair<std::string, std::string>> &responseHeaders, std::vector<uint8_t> &responseData, Error& error)
{
  auto url = fmt::format("{}/dataset/tenant/{}/subproject/{}/dataset/{}?path={}", m_manager.m_authorityUrl, URLEncode(m_tenant), URLEncode(m_subproject), URLEncode(m_dataset), URLEncode(m_path));
  std::shared_ptr<UploadRequestCurl> request = std::make_shared<UploadRequestCurl>("create_dataset", std::function<void(const Request & request, const Error & error)>());
  std::vector<std::string> headers;
  m_lock_id = gen_lock_id(IOManager::Create);
  headers.emplace_back(fmt::format("x-seismic-dms-lockid: {}", m_lock_id));
  
  if (!m_legalTag.empty()) {
    headers.emplace_back(fmt::format("ltag: {}", m_legalTag));
  }

  m_manager.addHeaders(headers);
  m_manager.m_curlHandler.addUploadRequest(request, url, headers, CurlVerb::POST, {}, 0, 0);
  request->WaitForFinish(error);
  if (error.code)
  {
    auto responseString = std::string(request->m_uploadHandler->responseData.begin(), request->m_uploadHandler->responseData.end());
    error.string = fmt::format("Seismic dms create dataset failed: {} - {}", error.string, responseString);
    return false;
  }
  responseHeaders = std::move(request->m_uploadHandler->responseHeaders);
  responseData = std::move(request->m_uploadHandler->responseData);
  return true;
}
  
bool DmsDataset::lockDataset(IOManager::AccessPattern accessPattern, std::vector<std::pair<std::string, std::string>> &responseHeaders, std::vector<uint8_t> &responseData, Error& error)
{
  auto url = fmt::format("{}/dataset/tenant/{}/subproject/{}/dataset/{}/lock?openmode={}&path={}", m_manager.m_authorityUrl, URLEncode(m_tenant), URLEncode(m_subproject), URLEncode(m_dataset), accessPattern == IOManager::ReadOnly ? "read" : "write", URLEncode(m_path));
  std::shared_ptr<UploadRequestCurl> request = std::make_shared<UploadRequestCurl>("lock_dataset", std::function<void(const Request & request, const Error & error)>());
  std::vector<std::string> headers;
  m_lock_id = gen_lock_id(accessPattern);
  headers.emplace_back(fmt::format("x-seismic-dms-lockid: {}", m_lock_id));
  m_manager.addHeaders(headers);
  m_manager.m_curlHandler.addUploadRequest(request, url, headers, CurlVerb::PUT, {}, 0, 0);
  request->WaitForFinish(error);
  if (error.code)
  {
    auto responseString = std::string(request->m_uploadHandler->responseData.begin(), request->m_uploadHandler->responseData.end());
    error.string = fmt::format("Seismic dms lock dataset failed: {} - {}", error.string, responseString);
    return false;
  }
  responseHeaders = std::move(request->m_uploadHandler->responseHeaders);
  responseData = std::move(request->m_uploadHandler->responseData);
  return true;
}
  
bool DmsDataset::deleteDataset(Error& error)
{
  auto url = fmt::format("{}/dataset/tenant/{}/subproject/{}/dataset/{}?path={}", m_manager.m_authorityUrl, URLEncode(m_tenant), URLEncode(m_subproject), URLEncode(m_dataset), URLEncode(m_path));
  std::shared_ptr<UploadRequestCurl> request = std::make_shared<UploadRequestCurl>("delete_dataset", std::function<void(const Request & request, const Error & error)>());
  std::vector<std::string> headers;
  m_lock_id = gen_lock_id(IOManager::Create);
  headers.emplace_back(fmt::format("x-seismic-dms-lockid: {}", m_lock_id));
  m_manager.addHeaders(headers);
  m_manager.m_curlHandler.addUploadRequest(request, url, headers, CurlVerb::DEL, {}, 0, 0);
  request->WaitForFinish(error);
  if (error.code)
  {
    auto responseString = std::string(request->m_uploadHandler->responseData.begin(), request->m_uploadHandler->responseData.end());
    error.string = fmt::format("Seismic dms delete dataset failed: {} - {}", error.string, responseString);
    return false;
  }
  return true;
}

bool DmsDataset::open(IOManager::AccessPattern accessPattern, Error &error)
{
  if (m_opened)
  {
    error.code = -1;
    error.string = "Seismic DMS: Opening an allready opened DatasetInstance.";
    return false;
  }

  if (!m_manager.ensureSdToken(error))
    return false;

  {
    std::vector<std::pair<std::string, std::string>> responseHeaders;
    std::vector<uint8_t> responseData;
    if (accessPattern == IOManager::Create)
    {
      if (!registerDataset(responseHeaders, responseData, error))
      {
        //OpenVDS by default overwrites data. This is the same as the SDAPI would do when opening a dataset in overwrite mode
        if (error.code == 409)
        {
          error = Error();
          if (!deleteDataset(error))
          {
            return false;
          }
          responseHeaders.clear();
          responseData.clear();
          if (!registerDataset(responseHeaders, responseData, error))
          {
            return false;
          }
        }
        else
        {
          return false;
        }
      }
    }
    else
    {
      if (!lockDataset(accessPattern, responseHeaders, responseData, error))
      {
        return false;
      }
    }
    m_accessPattern = accessPattern;
    m_opened = true;
    Json::Value root;
    if (!ParseJSONFromBuffer(responseData, root, error))
    {
      return false;
    }
    try
    {
      m_gc_url = root["gcsurl"].asString();
      m_legalTag = root["ltag"].asString();
      m_accessPolicy = root["access_policy"].asString();

      auto metadata = root["filemetadata"];
      m_nobjects = 0;
      if (!metadata.empty())
      {
        m_nobjects = metadata.get("nobjects", 0).asInt();
      }
    }
    catch (const Json::Exception& ex)
    {
      error.code = -1;
      error.string = fmt::format("Seismic dms lock failed: {}", ex.what());
      return false;
    }

    for (auto& header : responseHeaders)
    {
      if (header.first == "service-provider")
        m_service_provider = header.second;
    }

    if (m_service_provider.empty())
    {
      error.code = -1;
      error.string = "Seismic dms lock failed: Missing service-provider header";
      return false;
    }
  }
  return true;
}

std::vector<uint8_t>
static WriteJson(Json::Value root)
{
  std::vector<uint8_t>
    result;

  Json::StreamWriterBuilder wbuilder;
  wbuilder["indentation"] = "    ";
  std::string document = Json::writeString(wbuilder, root);

  // strip carriage return
  result.reserve(document.length());
  for(char c : document)
  {
    if(c != '\r')
    {
      result.push_back(c);
    }
  }

  return result;
}
bool DmsDataset::close(uint64_t serializedSize, uint64_t chunkCount, Error& error)
{
  if (!m_opened)
  {
    error.code = -1;
    error.string = "Seismic DMS: Closing an already closed DatasetInstance.";
    return false;
  }
  if (!m_manager.ensureSdToken(error))
    return false;

  std::string url = fmt::format("{}/dataset/tenant/{}/subproject/{}/dataset/{}?path={}&close={}", m_manager.m_authorityUrl, URLEncode(m_tenant), URLEncode(m_subproject), URLEncode(m_dataset), URLEncode(m_path), m_lock_id);
  std::shared_ptr<UploadRequestCurl> request = std::make_shared<UploadRequestCurl>("lock_dataset", std::function<void(const Request& request, const Error& error)>());
  std::vector<std::string> headers;
  headers.emplace_back(fmt::format("x-seismic-dms-lockid: {}", m_lock_id));
  m_manager.addHeaders(headers);
  std::vector<std::shared_ptr<std::vector<uint8_t>>> data;
  int64_t completeSize = 0;
  if (m_accessPattern != IOManager::ReadOnly)
  {
    Json::Value root;
    Json::Value filemetadata;
    filemetadata["nobjects"] = chunkCount;
    filemetadata["size"] = serializedSize;
    filemetadata["type"] = "GENERIC";
    root["filemetadata"] = filemetadata;
    root["last_modified_date"] = true;

    data.emplace_back(std::make_shared<std::vector<uint8_t>>());
    auto& shared_vector = data.back();
    auto& vector = *shared_vector;
    vector = WriteJson(root);
    completeSize = vector.size();
  }
  m_manager.m_curlHandler.addUploadRequest(request, url, headers, CurlVerb::PATCH, std::move(data), completeSize, 0);
  request->WaitForFinish(error);
  if (error.code || !request->m_uploadHandler)
  {
    auto responseString = std::string(request->m_uploadHandler->responseData.begin(), request->m_uploadHandler->responseData.end());
    error.string = fmt::format("Seismic dms close failed: {} - {}", error.string, responseString);
    return false;
  }
  m_opened = false;
  return true;
}

DmsIoManagerFactory* DmsIoManagerFactory::createDmsIoManagerFactory(const std::string& serviceProvider, DmsDataset &dataset, Logger &logger, Error &error)
{
#ifndef OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER
  if (serviceProvider == "azure")
    return new AzureDmsIoManagerFactory(dataset);
#endif
#ifndef OPENVDS_NO_AWS_IOMANAGER
  if (serviceProvider == "aws")
    return new AwsDmsIoManagerFactory(dataset, logger);
  if (serviceProvider == "anthos")
    return new AnthosDmsIoManagerFactory(dataset, logger);
  if (serviceProvider == "ibm")
    return new IbmDmsIoManagerFactory(dataset, logger);
#endif
#ifndef OPENVDS_NO_GCP_IOMANAGER
  return new GcpDmsIoManagerFactory(dataset, logger);
#else
  error.code = -1;
  error.string = "Failed to find proxy IOManager";
  return nullptr;
#endif
}

DmsIoManagerFactory::DmsIoManagerFactory(DmsDataset& dataset)
  : m_dataset(dataset)
{}
DmsIoManagerFactory::~DmsIoManagerFactory()
{}
DmsIoManagerFactory::GcsAccessToken DmsIoManagerFactory::gcsAccessToken(Error &error)
{
    GcsAccessToken gcsAccessToken;
    std::string readonly = m_dataset.m_accessPattern == IOManager::ReadOnly ? "true" : "false";

    std::string sdPath = m_dataset.m_accessPolicy == "dataset" ? URLEncode(m_dataset.m_url) : URLEncode(fmt::format("sd://{}/{}", m_dataset.m_tenant, m_dataset.m_subproject));
    std::string url = fmt::format("{}/utility/gcs-access-token?readonly={}&sdpath={}", m_dataset.m_manager.m_authorityUrl, readonly, sdPath);
    std::shared_ptr<DownloadRequestCurl> request = std::make_shared<DownloadRequestCurl>("gcs-access-token", nullptr);
    std::vector<std::string> headers;
    m_dataset.m_manager.addHeaders(headers);
    m_dataset.m_manager.m_curlHandler.addDownloadRequest(request, url, headers, {}, CurlVerb::GET);
    request->WaitForFinish(error);
    if (error.code || !request->m_downloadHandler)
    {
      auto responseString = std::string(request->m_downloadHandler->responseData.begin(), request->m_downloadHandler->responseData.end());
      error.string = fmt::format("Seismic DMS: gcs-access-token failed: {} - {}", error.string, responseString);
      return gcsAccessToken;
    }
    gcsAccessToken.data = std::move(request->m_downloadHandler->responseData);
    gcsAccessToken.headers = std::move(request->m_downloadHandler->responseHeaders);
    return gcsAccessToken;
}

}
