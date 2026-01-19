#ifndef OPENVDS_DmsIoMANAGER_FACTORY_H
#define OPENVDS_DmsIoMANAGER_FACTORY_H

#include <OpenVDS/OpenVDS.h>
#include <IO/IOManager.h>
#include <IO/IOManagerCurl.h>
#include <chrono>
#include <json_cpp_include.h>

namespace OpenVDS
{
struct DmsManager
{
  DmsManager(const std::string& authorityUrl, const std::string& appKey, CurlHandler& curlHandler, Logger& logger);
  void setSdTokenDefaultExpiry();
  bool ensureSdToken(Error& error);
  void addHeaders(std::vector<std::string>& headers);

  const std::string m_authorityUrl;
  const std::string m_appKey;
  CurlHandler& m_curlHandler;
  std::string m_sdToken;
  std::chrono::time_point<std::chrono::system_clock> m_sdTokenExpiry;
  Logger& m_logger;
  std::string (*m_authProviderCallback)(const void*);
  const void *m_authProviderCallbackData;
};

struct DmsDataset
{
  DmsDataset(DmsManager& manager, const std::string url, Error& error, const std::string& legalTag="");

  bool open(IOManager::AccessPattern accessPattern, Error& error);
  bool close(uint64_t serializedSize, uint64_t chunkCount, Error& error);

  DmsManager& m_manager;
  const std::string m_url;
  std::string m_tenant;
  std::string m_subproject;
  std::string m_path;
  std::string m_dataset;
  std::string m_lock_id;
  std::string m_service_provider;
  std::string m_gc_url;
  std::string m_accessPolicy;

  IOManager::AccessPattern m_accessPattern;
  bool m_opened;
  std::chrono::time_point<std::chrono::steady_clock> m_azure_sas_token_expires;

  std::string m_legalTag;
  int m_nobjects;

private:
  bool registerDataset(std::vector<std::pair<std::string, std::string>> &responseHeaders, std::vector<uint8_t>& responseData, Error& error);
  bool lockDataset(IOManager::AccessPattern accessPattern, std::vector<std::pair<std::string, std::string>> &responseHeaders, std::vector<uint8_t>& responseData, Error& error);
  bool deleteDataset(Error& error);
};

struct DmsIoManagerFactory
{
  static DmsIoManagerFactory* createDmsIoManagerFactory(const std::string& serviceProvider, DmsDataset &dataset, Logger &logger, Error &error);

  virtual ~DmsIoManagerFactory();

  virtual std::unique_ptr<IOManager> createIOManager(std::shared_ptr<CurlHandler> curlHandler, std::chrono::time_point<std::chrono::steady_clock> &expirationTime, Error& error) = 0;

  struct GcsAccessToken
  {
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> data;
  };
  GcsAccessToken gcsAccessToken(Error& error);

protected:
  DmsIoManagerFactory(DmsDataset& dataset);

  DmsDataset& m_dataset;
};
}
#endif
