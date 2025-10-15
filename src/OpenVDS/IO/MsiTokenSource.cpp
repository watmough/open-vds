/****************************************************************************
** Copyright (c) Microsoft Corporation.
****************************************************************************/

#include "MsiTokenSource.h"
#include "IO/IOManagerCurl.h"
#include "DmsIoFactories/DmsIoManagerFactory.h"
#include <json_cpp_include.h>

#include <future>
#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace OpenVDS
{

// Fetch an MSI access token from the Azure IMDS endpoint.
// Returns an empty string on failure. This function performs a blocking
// HTTP GET to the well-known IMDS address and parses the JSON response.
std::string FetchMsiTokenFromImds()
{
  // Prepare an error object and a lightweight logger for the temporary curl handler.
  Error error;
  Logger logger(static_cast<GlobalStateImpl*>(OpenVDS::GetGlobalState())->logInterface, LogLevel::Info);
  logger.LogInfo("FetchMsiTokenFromImds: start");

  // Create a curl handler instance similar to other IO helpers. If construction
  // fails we return early.
  CurlHandler curlHandler(error, logger, CurlHandler::defaultMaxHostConnections, std::string());
  if (error.code)
  {
    logger.LogError("FetchMsiTokenFromImds: failed to construct CurlHandler");
    return std::string();
  }

  // Split the IMDS request into named parts to make the intent clearer
  // and to allow easy modification later.
  const std::string armResource = "https://management.azure.com/";
  const std::string imdsEndpoint = "http://169.254.169.254/metadata/identity/oauth2/token";
  const std::string apiVersion = "2018-02-01";

  // Use the pre-encoded resource query value for clarity and simplicity.
  // The following percent-encoded value corresponds to: https://management.azure.com/
  const std::string imdsUrl = imdsEndpoint + "?api-version=" + apiVersion + "&resource=https%3A%2F%2Fmanagement.azure.com%2F";
  logger.LogInfo(std::string("FetchMsiTokenFromImds: requesting IMDS URL: ") + imdsUrl);

  // Prepare the download request and required headers and enqueue it.
  auto request = std::make_shared<DownloadRequestCurl>("msi_token", nullptr);
  std::vector<std::string> headers;
  headers.emplace_back("Metadata: true");
  headers.emplace_back("Accept: application/json");

  // Enqueue the request and ask the CurlHandler to retry up to 3 times.
  const int retryCount = 3;
  curlHandler.addDownloadRequest(request, imdsUrl, headers, {}, CurlVerb::GET, retryCount);

  // Wait for the request to finish but only up to a short timeout. If it times out, cancel the request.
  const std::chrono::milliseconds imdsWaitTimeoutMs(3000); // 3s overall wait
  auto waitFuture = std::async(std::launch::async, [&request, &error]() { return request->WaitForFinish(error); });
  if (waitFuture.wait_for(imdsWaitTimeoutMs) != std::future_status::ready)
  {
    logger.LogError("FetchMsiTokenFromImds: IMDS request timed out, canceling");
    request->Cancel();
    waitFuture.wait();
  }

  if (error.code) {
    logger.LogError(std::string("FetchMsiTokenFromImds: request failed: ") + error.string);
    return std::string();
  }

  if (!request->m_downloadHandler) {
    logger.LogError("FetchMsiTokenFromImds: no download handler after request completion");
    return std::string();
  }

  // Parse the response JSON and extract access_token.
  Json::Value root;
  if (!ParseJSONFromBuffer(request->m_downloadHandler->responseData, root, error)) {
    logger.LogError("FetchMsiTokenFromImds: failed to parse JSON response");
    return std::string();
  }

  if (!root.isMember("access_token") || !root["access_token"].isString()) {
    logger.LogError("FetchMsiTokenFromImds: access_token missing in IMDS response");
    return std::string();
  }

  const std::string token = root["access_token"].asString();
  logger.LogInfo(std::string("FetchMsiTokenFromImds: obtained token, length=") + std::to_string(token.size()));
  return token;
}

} // namespace OpenVDS
