// ============================================================================
// memleak_config.h - compile-time configuration queries.
//
// memleak — Cross-platform memory leak detector for Qt/C++ projects.
//
// Copyright (c) 2026, Li Zhulin
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================

#ifndef MEMLEAK_CONFIG_H
#define MEMLEAK_CONFIG_H

// ============================================================================
// memleak_config - Internal configuration facade.
// Translates the compile-time macros into runtime queries. Values are
// resolved at first use; macros cannot change during runtime by design.
// ============================================================================

namespace memleak
{

    class MemLeakConfig
    {
    public:
        // Channel / output / report switches (from macros).
        static bool trackNew();
        static bool trackMalloc();
        static bool outputConsole();
        static bool outputFile();
        static bool reportDirect();
        static bool reportAggregated();
        static int  maxStackFrames();
        static int  maxRecords();
        static const char *logFilePath();

        // Platform capability queries.
        static bool crtHookAvailable();   // true on MSVC debug builds
        static bool wrapAvailable();      // true on Linux / MinGW

        // Symbolization: prefer the built-in addr2line path (true unless the
        // MEMLEAK_USE_ADDR2LINE macro is removed by the user project).
        static bool addr2lineEnabled();

        // addr2line executable to invoke; configurable via MEMLEAK_ADDR2LINE_PATH
        // (qmake variable MEMLEAK_ADDR2LINE), defaults to "addr2line" on PATH.
        static const char *addr2linePath();
    };

} // namespace memleak

#endif // MEMLEAK_CONFIG_H
