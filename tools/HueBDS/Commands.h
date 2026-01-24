/****************************************************************************
** Copyright 2022 The Open Group
** Copyright 2022 Bluware, Inc.
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

#ifndef COMMANDS_H
#define COMMANDS_H

#include "HueBulkDataStoreFileTypes.h"
#include "HueBulkDataStoreFormat.h"
#include "Util.h"
#include "ParserHelper.h"
#include "ExtentAllocator.h"
#include <vector>

#include <string>
#include <cassert>
#include <sstream>


#ifdef _WIN32
#define ftello   _ftelli64
#endif

#include "huebds_exports.h"

struct Command
{
  enum CommandType
  {
    HelpCommand,
    DataStoreCommand,
    FileCommand
  };

  static int
    HUEBDS_EXPORTS argumentCount[];

  const char
    *name;

  CommandType
    commandType;

  int  additionalArgumentCount;
  int  optionalArgumentCount;

  union
  {
    void *initializer;
    bool(*helpFunction)(int argumentCount, char *arguments[]);
    bool(*dataStoreFunction)(HueBulkDataStore *dataStore, int argumentCount, char *arguments[]);
    bool(*fileFunction)(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
  }    handler;

  const char *
    helpText;
};

struct Range_c {
  int32_t
    _iRangeStart,
    _iRangeEnd;

  Range_c() :
    _iRangeStart(-1),
    _iRangeEnd(-1)
  {
  }
};


extern "C" bool HUEBDS_EXPORTS ScanVDSFile(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS RenameFile(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS AddFile(HueBulkDataStore *dataStore, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS RemoveFile(HueBulkDataStore *dataStore, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS CheckDataStore(HueBulkDataStore *dataStore, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS ListDataStore(HueBulkDataStore *dataStore, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS Export(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, const char *outputFileName, int startChunk, int endChunk);
extern "C" bool HUEBDS_EXPORTS ExportFile(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS ExportChunks(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS Import(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, const char *inputFileName, int startChunk, int endChunk);
extern "C" bool HUEBDS_EXPORTS ImportFile(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);
extern "C" bool HUEBDS_EXPORTS ImportChunks(HueBulkDataStore *dataStore, HueBulkDataStore::FileInterface *file, int argumentCount, char *arguments[]);

#endif 

