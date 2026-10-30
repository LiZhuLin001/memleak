// ============================================================================
// memleak.h - public API: MemLeakDetector facade, platform & standard macros.
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

#ifndef MEMLEAK_H
#define MEMLEAK_H

// ============================================================================
// memleak - Cross-platform memory leak detector for Qt/C++ projects.
//
//   * Channel A: global operator new/delete overloads (all platforms,
//     debug and release). Reliable baseline coverage for C++ code.
//   * Channel B: malloc/free/calloc/realloc interception
//       - Linux / MinGW: --wrap linker technique (auto-added by memleak.pri)
//       - MSVC (debug):  _CrtSetAllocHook CRT allocation hook
//                        (MSVC release has no CRT hook; channel A still works)
//
// Usage:
//   * include(memleak.pri) in your .pro (nothing else required),
//   * #include "memleak.h" where you want to trigger reports, e.g.
//       memleak::MemLeakDetector::instance().dumpReport();
//
// Compile-time switches (defaults injected by memleak.pri, see memleak.pri):
//   MEMLEAK_ENABLED            master switch
//   MEMLEAK_TRACK_NEW          channel A
//   MEMLEAK_TRACK_MALLOC       channel B
//   MEMLEAK_OUTPUT_CONSOLE     report to stdout
//   MEMLEAK_OUTPUT_FILE        report to MEMLEAK_LOG_FILE
//   MEMLEAK_LOG_FILE           log file path (default "memleak.log")
//   MEMLEAK_REPORT_DIRECT      one entry per leaked block
//   MEMLEAK_REPORT_AGGREGATED  grouped by call stack
//   MEMLEAK_MAX_STACK_FRAMES   frames captured per allocation (default 16)
//   MEMLEAK_MAX_RECORDS        allocation table cap (default 100000)
//
// Design notes:
//   * Multi-thread safe: the allocation table is guarded by a QMutex,
//     records carry std::thread::id, and a thread_local re-entrancy flag
//     prevents the detector from recording its own internal allocations.
//   * Idempotent insert/remove: both channels may observe one allocation;
//     duplicates are silently ignored.
//   * Stack frames are captured as raw addresses on the hot path; symbol
//     resolution is deferred to report time.
// ============================================================================

// Log file default lives here (pri does not inject string macros to avoid
// quoting pitfalls; users may override via DEFINES += MEMLEAK_LOG_FILE=...).
#ifndef MEMLEAK_LOG_FILE
    #define MEMLEAK_LOG_FILE "memleak.log"
#endif

namespace memleak
{
#ifdef MEMLEAK_ENABLED
    // Singleton facade. All interception channels funnel into this object's
    // allocation table; reports are generated on demand.
    class MemLeakDetector
    {
    public:
        static MemLeakDetector &instance();

        void start();           // start recording (default: active)
        void stop();            // stop recording, keep existing records
        bool isEnabled() const;

        void dump();            // report per compile-time output/report macros
        void reset();           // clear all records and timers (checkpoint)

    private:
        MemLeakDetector();
        ~MemLeakDetector();
        MemLeakDetector(const MemLeakDetector &) = delete;
        MemLeakDetector &operator=(const MemLeakDetector &) = delete;

        struct Impl;
        Impl *d;

        // Internal accessor defined in memleak.cpp; the only route to the
        // private Pimpl data from the interception channels.
        friend struct DetectorCore;
    };

#else // MEMLEAK_ENABLED not defined: no-op stub keeps call sites clean.

    class MemLeakDetector
    {
    public:
        static MemLeakDetector &instance()
        {
            static MemLeakDetector inst;
            return inst;
        }
        void install() {}
        void uninstall() {}
        bool isEnabled() const
        {
            return false;
        }
        void dumpReport() {}
        void reset() {}
    };

#endif // MEMLEAK_ENABLED
} // namespace memleak

#endif // MEMLEAK_H
