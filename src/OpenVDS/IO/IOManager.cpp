/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
** Copyright 2020 Microsoft Corp.
** Copyright 2020 Google, Inc.
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
#define USE_IMPORT_EXPORT 1
#include "IOManager.h"

#include "IOManagerInMemory.h"

#ifndef OPENVDS_NO_AWS_IOMANAGER
#include "IOManagerAWSCurl.h"
#endif
#ifndef OPENVDS_NO_AZURE_SDK_FOR_CPP_IOMANAGER
#include "IOManagerAzureSdkForCpp.h"
#endif
#ifndef OPENVDS_NO_GCP_IOMANAGER
#include "IOManagerGoogle.h"
#endif
#ifndef OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER
#include "IOManagerAzurePresigned.h"
#endif
#ifndef OPENVDS_NO_HTTP_IOMANAGER
#include "IOManagerHttp.h"
#endif

#include "IOManagerDmsFactory.h"

#include "VDS/Env.h"

#include "VDS/GlobalStateImpl.h"

namespace OpenVDS
{

IOManager* IOManager::CreateIOManager(const OpenOptions& options, IOManager::AccessPattern accessPattern, Error &error)
{
  Logger logger(static_cast<GlobalStateImpl*>(OpenVDS::GetGlobalState())->logInterface, options.logLevel);
  switch(options.connectionType)
  {
  case OpenOptions::AWS:
  {
#ifndef OPENVDS_NO_AWS_IOMANAGER
    return IOManagerAWSCurl::CreateIOManagerAWSCurl(static_cast<const AWSOpenOptions&>(options), logger, error);
#else
    error.code = -1;
    error.string = "Unknown type for OpenOptions";
    return nullptr;
#endif
  }
  break;
  case OpenOptions::Azure:
  {
#ifndef OPENVDS_NO_AZURE_SDK_FOR_CPP_IOMANAGER
    return IOManagerAzureSdkForCpp::CreateIOManagerAzureSdkForCpp(static_cast<const AzureOpenOptions&>(options), error);
#else
    error.code = -1;
    error.string = "Unknown type for OpenOptions";
    return nullptr;
#endif
  }
#ifndef OPENVDS_NO_AZURE_PRESIGNED_IOMANAGER
  case OpenOptions::AzurePresigned:
    return IOManagerAzurePresigned::CreateIOManagerAzurePresigned(static_cast<const AzurePresignedOpenOptions&>(options).baseUrl, static_cast<const AzurePresignedOpenOptions&>(options).urlSuffix, logger, error);
#endif
#ifndef OPENVDS_NO_GCP_IOMANAGER
  case OpenOptions::GoogleStorage:
    return IOManagerGoogle::CreateIOManagerGoogle(static_cast<const GoogleOpenOptions &>(options), logger, error);
#endif
#ifndef OPENVDS_NO_HTTP_IOMANAGER
  case OpenOptions::Http:
    return IOManagerHttp::CreateIOManagerHttp(static_cast<const HttpOpenOptions &>(options), logger, error);
#endif
#ifndef OPENVDS_NO_DMS_IOMANAGER
  case OpenOptions::DMS:
    return CreateDMSIOManager(static_cast<const DMSOpenOptions&>(options), accessPattern, logger, error);
#endif
  case OpenOptions::InMemory:
    return IOManagerInMemory::CreateIOManagerInMemory(static_cast<const InMemoryOpenOptions &>(options), error);
  default:
    error.code = -1;
    error.string = "Unknown type for OpenOptions";
    return nullptr;
  }
  return nullptr;
}

bool ParseJSONFromBuffer(const std::vector<unsigned char> &json, Json::Value &root, Error &error)
{
  try
  {
    Json::CharReaderBuilder rbuilder;
    rbuilder["collectComments"] = false;

    std::unique_ptr<Json::CharReader> reader(rbuilder.newCharReader());
    const char *json_begin = reinterpret_cast<const char *>(json.data());
    reader->parse(json_begin, json_begin + json.size(), &root, &error.string);

    return true;
  }
  catch(Json::Exception &e)
  {
    error.code = -1;
    error.string = e.what() + std::string(" : ") + error.string;
  }

  return false;
}

// This function is defined in OpenVDS.cpp, it might make more sense to move it here
OpenOptions* CreateOpenOptions(StringWrapper urlWrapper, StringWrapper connectionStringWrapper, Error& error);

IOManager *IOManager::CreateIOManager(const StringWrapper &url, const StringWrapper &connectionString, IOManager::AccessPattern accessPattern, Error& error)
{
  std::unique_ptr<OpenOptions> openOptions(CreateOpenOptions(url, connectionString, error));
  if (error.code || !openOptions)
    return nullptr;
  return CreateIOManager(*(openOptions.get()), accessPattern, error);
}

}
