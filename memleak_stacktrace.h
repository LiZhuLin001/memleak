// ============================================================================
// memleak_stacktrace.h - stack capture & symbolization.
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

#ifndef MEMLEAK_STACKTRACE_H
#define MEMLEAK_STACKTRACE_H

// ============================================================================
// memleak_stacktrace - Cross-platform call-stack capture and (lazy)
// symbolization.
//
// Capture happens on the allocation hot path and stores only raw return
// addresses (cheap). Symbolization (function names / module / line info)
// is deferred to report time because it is comparatively expensive.
// ============================================================================

#include <QByteArray>
#include <QVector>

namespace memleak
{

// One captured frame; symbol/module/offset are filled in by symbolize().
    struct StackFrame
    {
        void *address = nullptr;   // raw return address
        QByteArray symbol;         // resolved function name (report time)
        QByteArray module;         // module / object file name
        QByteArray fileLine;       // "file:line" when available (MSVC)
        quintptr offset = 0;       // address - function start
    };

// Lightweight captured call stack (addresses only at allocation time).
    class CallStack
    {
    public:
        CallStack() = default;
        explicit CallStack(const QVector<void *> &frames) : mFrames(frames) {}

        // Captures up to maxFrames frames, skipping the first skipFrames
        // frames (interception machinery) so frame 0 is the caller site.
        static CallStack capture(int maxFrames, int skipFrames);

        int size() const
        {
            return mFrames.size();
        }
        bool isEmpty() const
        {
            return mFrames.isEmpty();
        }
        const QVector<void *> &frames() const
        {
            return mFrames;
        }

        // Stable binary key for aggregation (same allocation site == same key).
        QByteArray hashKey() const;

        // Resolves symbols for every frame (report time only).
        QVector<StackFrame> symbolize() const;

    private:
        QVector<void *> mFrames;
    };

} // namespace memleak

#endif // MEMLEAK_STACKTRACE_H
