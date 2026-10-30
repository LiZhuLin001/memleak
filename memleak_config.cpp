// ============================================================================
// memleak_config.cpp - compile-time configuration queries.
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

// ============================================================================
// memleak_config.cpp - implementation of the compile-time configuration
// facade. Every query maps to one macro (or a sensible fallback).
// ============================================================================

#include "memleak_config.h"
#include "memleak.h"   // MEMLEAK_LOG_FILE default lives here

namespace memleak
{

#ifdef MEMLEAK_ENABLED

    bool MemLeakConfig::trackNew()
    {
#ifdef MEMLEAK_TRACK_NEW
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::trackMalloc()
    {
#ifdef MEMLEAK_TRACK_MALLOC
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::outputConsole()
    {
#ifdef MEMLEAK_OUTPUT_CONSOLE
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::outputFile()
    {
#ifdef MEMLEAK_OUTPUT_FILE
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::reportDirect()
    {
#ifdef MEMLEAK_REPORT_DIRECT
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::reportAggregated()
    {
#ifdef MEMLEAK_REPORT_AGGREGATED
        return true;
#else
        return false;
#endif
    }

    int MemLeakConfig::maxStackFrames()
    {
#ifdef MEMLEAK_MAX_STACK_FRAMES
        return MEMLEAK_MAX_STACK_FRAMES;
#else
        return 16;
#endif
    }

    int MemLeakConfig::maxRecords()
    {
#ifdef MEMLEAK_MAX_RECORDS
        return MEMLEAK_MAX_RECORDS;
#else
        return 100000;
#endif
    }

    const char *MemLeakConfig::logFilePath()
    {
        return MEMLEAK_LOG_FILE;
    }

    bool MemLeakConfig::crtHookAvailable()
    {
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG)
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::wrapAvailable()
    {
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
        return true;
#else
        return false;
#endif
    }

    bool MemLeakConfig::addr2lineEnabled()
    {
#ifdef MEMLEAK_USE_ADDR2LINE
        return true;
#else
        return false;
#endif
    }

    const char *MemLeakConfig::addr2linePath()
    {
#ifdef MEMLEAK_ADDR2LINE_PATH
        return MEMLEAK_ADDR2LINE_PATH;
#else
        return "addr2line";
#endif
    }

#endif // MEMLEAK_ENABLED

} // namespace memleak
