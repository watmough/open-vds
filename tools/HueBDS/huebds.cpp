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

#include <cassert>
#include <sstream>

#include "Commands.h"
#include "HueBulkDataStoreFileTypes.h"
#include "ExtentAllocator.h"
#include "VDSObjectParser.h"

#ifdef _WIN32
#include <WindowsUnicodeMain.h>
#endif

bool ShowHelp(int argumentCount, char *arguments[]);

bool ParseVDSFile(int argumentCount, char *arguments[])
{
  Parser *parser;
  InitializeParser(arguments[0], &parser,(char *) "");
  if (!ParseVDS(&parser))
  {
    printf("Error parsing VDS file!\n");
    return false;
  }
  size_t size = GetVolumeDataLayoutJsonString(parser, nullptr, 0);
  std::vector<char> jsonString;
  jsonString.resize(size,0);
  GetVolumeDataLayoutJsonString(parser, jsonString.data(), jsonString.size());
  printf("%s\n", std::string(jsonString.data(), jsonString.data() + jsonString.size()).c_str());
  DeInitializeParser(&parser);
  return true;
}

Command commands[] =
{
  {"help",         Command::HelpCommand,      0, 1, {(void *)&ShowHelp}      , "Seriously?\n"},
  {"check",        Command::DataStoreCommand, 0, 0, {(void *)&CheckDataStore}, "check dataStorePath\n"},
  {"scan",         Command::FileCommand,      0, 0, {(void *)&ScanVDSFile}   , "scan dataStorePath fileName\n"},
  {"list",         Command::DataStoreCommand, 0, 0, {(void *)&ListDataStore} , "list dataStorePath\n"},
  {"export",       Command::FileCommand,      0, 1, {(void *)&ExportFile}    , "export dataStorePath fileName [outputFilePath]\n"},
  {"exportchunks", Command::FileCommand,      1, 1,{ (void *)&ExportChunks}  , "exportchunks dataStorePath fileName chunk[-endchunk] [outputFilePath]\n" },
  {"import",       Command::FileCommand,      0, 1,{ (void *)&ImportFile}    , "import dataStorePath fileName [inputFilePath]\n" },
  {"importchunks", Command::FileCommand,      1, 1,{ (void *)&ImportChunks}  , "importchunks dataStorePath fileName chunk[-endchunk] [inputFilePath]\n" },
  {"rename",       Command::FileCommand,      1, 0, {(void *)&RenameFile}    , "rename dataStorePath fileName newFileName\n"},
  {"add",          Command::DataStoreCommand, 5, 1, {(void *)&AddFile}       , "add dataStorePath fileName chunkCount indexPageEntryCount fileType chunkMetadataLength [fileMetadata]\n"},
  {"remove",       Command::DataStoreCommand, 1, 0, {(void *)&RemoveFile}    , "remove dataStorePath fileName\n"},
  {"parsevds",     Command::HelpCommand,      0, 1,{ (void *)&ParseVDSFile } , "parsevds fileName.vds\n" }
};

bool ShowHelp(int argumentCount, char *arguments[])
{
  if (argumentCount == 0)
  {
    printf("A tool to manipulate Hue bulk data store files\n");
    printf("Commands:\n");
    printf("  help          Prints this message\n");
    printf("  check         Checks the integrity of the specified Hue bulk data store\n");
    printf("  scan          Scans the chunk metadata of a file inside the specified Hue bulk data store\n");
    printf("  list          Lists the files inside the specified Hue bulk data store\n");
    printf("  export        Exports chunks from a file inside the specified Hue bulk data store to a data file and a chunk description file\n");
    printf("  import        Imports chunks from a data file and a chunk description file to a file inside the specified Hue bulk data store\n");
    printf("  add           Adds a new empty file to the specified Hue bulk data store\n");
    printf("  remove        Removes a file from the specified Hue bulk data store\n");
    printf("  rename        Renames a file inside the specified Hue bulk data store\n");
    printf("  parsevds      Parse VDSObject datastore to extract metadata. Dumps JSON representation.\n");
    return true;
  }
  else
  {
    for (int command = 0; command < sizeof(commands) / sizeof(Command); command++)
    {
      if (strcmp(arguments[0], commands[command].name) == 0)
      {
        if (commands[command].helpText)
        {
          printf("%s", commands[command].helpText);
        }
        else
        {
          printf("No help available for command %s", commands[command].name);
        }
        return true;
      }
    }

    printf("ERROR: unknown command '%s'\n", arguments[0]);
    printf("Try 'huebds help' or 'huebds help `command-name`' for more information\n");
    return false;
  }
}

#ifdef UNICODE_MAIN
int UNICODE_MAIN(int argc, char **argv)
#else
int
main(int argc, char* argv[])
#endif
{
  if (argc == 1)
  {
    printf("ERROR: No arguments provided\n");
    printf("Try 'huebds help' or 'huebds help `command-name`' for more information\n");
    return EXIT_FAILURE;
  }

  for (int command = 0; command < sizeof(commands) / sizeof(Command); command++)
  {
    if (strcmp(argv[1], commands[command].name) == 0)
    {
      if (argc - 2 < Command::argumentCount[commands[command].commandType] + commands[command].additionalArgumentCount ||
        argc - 2 > Command::argumentCount[commands[command].commandType] + commands[command].additionalArgumentCount + commands[command].optionalArgumentCount)
      {
        printf("ERROR: wrong number of arguments for command '%s'\n", commands[command].name);
        return EXIT_FAILURE;
      }

      HueBulkDataStore *
        dataStore = NULL;

      HueBulkDataStore::FileInterface *
        fileInterface = NULL;

      if (commands[command].commandType == Command::DataStoreCommand ||
        commands[command].commandType == Command::FileCommand)
      {
        dataStore = HueBulkDataStore::Open(argv[2]);

        if (!dataStore->IsOpen())
        {
          if (commands[command].handler.dataStoreFunction == &AddFile)
          {
            HueBulkDataStore::Close(dataStore);
            dataStore = HueBulkDataStore::CreateNew(argv[2], false);

            if (!dataStore->IsOpen())
            {
              printf("ERROR: failed to create datastore '%s': %s\n", argv[2], dataStore->GetErrorMessage());
              return EXIT_FAILURE;
            }
          }
          else
          {
            printf("ERROR: failed to open datastore '%s': %s\n", argv[2], dataStore->GetErrorMessage());
            return EXIT_FAILURE;
          }
        }
        if (commands[command].commandType == Command::FileCommand)
        {
          fileInterface = dataStore->OpenFile(argv[3]);

          if (!fileInterface)
          {
            printf("ERROR: failed to open file '%s'\n", argv[3]);
            return EXIT_FAILURE;
          }
        }
      }

      int
        additonalArgumentCount = argc - 2 - Command::argumentCount[commands[command].commandType];

      bool
        success;

      switch (commands[command].commandType)
      {
      case Command::HelpCommand:
        success = commands[command].handler.helpFunction(additonalArgumentCount, argv + argc - additonalArgumentCount);
        break;
      case Command::DataStoreCommand:
        success = commands[command].handler.dataStoreFunction(dataStore, additonalArgumentCount, argv + argc - additonalArgumentCount);
        break;
      case Command::FileCommand:
        success = commands[command].handler.fileFunction(dataStore, fileInterface, additonalArgumentCount, argv + argc - additonalArgumentCount);
        break;
      }

      if (fileInterface)
      {
        dataStore->CloseFile(fileInterface);
      }

      if (dataStore)
      {
        HueBulkDataStore::Close(dataStore);
      }

      return success ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }

  printf("ERROR: unknown command '%s'\n", argv[1]);
  printf("Try 'huebds help' or 'huebds help `command-name`' for more information\n");
  return EXIT_FAILURE;
}
