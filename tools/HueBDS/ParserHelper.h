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

#ifndef PARSERHELPER_H
#define PARSERHELPER_H

#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <sstream>
#include <string.h>
#include <vector>

#include "HueBulkDataStoreFormat.h"
#include "HueBulkDataStoreFileTypes.h"
#ifdef USE_OPEN_DATA_STORE
#include <OpenDataStore/OpenDataStore.hpp>
using HueBulkDataStore = OpenDataStore;
#else
#include "HueBulkDataStore.h"
#endif
#include "ExtentAllocator.h"

#ifdef _WIN32
static _locale_t C_locale = _create_locale(LC_CTYPE, "C");
#define strtod_l _strtod_l
#define strtof_l _strtof_l
#define strtoll  _strtoi64
#define strtoull _strtoui64
#define ftello   _ftelli64
#else
static locale_t C_locale = newlocale(LC_CTYPE_MASK, "C", NULL);
#endif



static bool  MatchToken(const char **string, const char *token);
static bool  MatchInteger(const char **string, int *result, int base = 10);
static bool  MatchInteger64(const char **string, int64_t *result, int base = 10);
static bool  MatchFloat(const char **string, float *result);
bool         ParseMetadata(void *metadata, int *metadataLength, const char *metadataBuffer, int fileType = 0 /* Chunk metadata doesn't have a fileType */);

#endif
