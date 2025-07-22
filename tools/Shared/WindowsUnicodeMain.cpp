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

#ifdef _WIN32

#include "WindowsUnicodeMain.h"
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>

namespace {
    // Convert wide string to UTF-8
    std::string WideToUtf8(const wchar_t* wide_str) {
        if (!wide_str || !*wide_str) {
            return std::string();
        }

        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide_str, -1, nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) {
            return std::string();
        }

        std::string result(size_needed - 1, 0); // -1 to exclude null terminator
        WideCharToMultiByte(CP_UTF8, 0, wide_str, -1, &result[0], size_needed, nullptr, nullptr);
        return result;
    }
}

int wmain(int argc, wchar_t* argv[]) {
    // Convert wide arguments to UTF-8
    std::vector<std::string> utf8_args(argc);
    std::vector<char*> utf8_argv(argc + 1); // +1 for null terminator

    for (int i = 0; i < argc; ++i) {
        utf8_args[i] = WideToUtf8(argv[i]);
        utf8_argv[i] = const_cast<char*>(utf8_args[i].c_str());
    }
    utf8_argv[argc] = nullptr;

    // Call the actual main function with UTF-8 arguments
    return main_utf8(argc, utf8_argv.data());
}

#endif // _WIN32