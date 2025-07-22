/****************************************************************************
** Copyright 2025 The Open Group
** Copyright 2025 Bluware, Inc.
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

#pragma once

#ifdef _WIN32

// Define UNICODE_MAIN to be main_utf8 for Windows Unicode handling
#define UNICODE_MAIN main_utf8

// Forward declaration of the UTF-8 main function
int main_utf8(int argc, char** argv);

// Windows wmain wrapper that converts wide arguments to UTF-8
int wmain(int argc, wchar_t* argv[]);

#endif