// ============================================================================
// memleak_report.h - report strategies & output sinks.
//
// memleak - Cross-platform memory leak detector for Qt/C++ projects.
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

#ifndef MEMLEAK_REPORT_H
#define MEMLEAK_REPORT_H

// ============================================================================
// memleak_report - Report generation and output sinks.
//
// Two report modes (compile-time selected):
//   * DIRECT     : one entry per leaked block (most detail, largest output)
//   * AGGREGATED : group leaked blocks by call-stack key (count + total
//                  bytes + source + threads); preferred when both macros
//                  are defined.
//
// Output targets (strategy pattern, both may be active):
//   * ConsoleSink : stdout, using fwrite (no printf) to avoid hidden
//                   allocations inside the report path.
//   * FileSink    : append-mode file (MEMLEAK_LOG_FILE).
// ============================================================================

#include <cstdio>

#include <QtCore/QString>
#include <QtCore/QVector>

#include "memleak_record.h"

namespace memleak
{

// Abstract output target (strategy pattern).
    class OutputSink
    {
    public:
        virtual ~OutputSink() = default;
        virtual void write(const QString &text) = 0;
    };

// Writes to stdout.
    class ConsoleSink : public OutputSink
    {
    public:
        void write(const QString &text) override;
    };

// Writes to a file opened in append mode on first write.
    class FileSink : public OutputSink
    {
    public:
        explicit FileSink(const QString &path);
        ~FileSink() override;
        void write(const QString &text) override;

    private:
        QString mPath;
        FILE *mFile = nullptr;
    };

// Builds the report text from a snapshot of leaked records.
    class ReportGenerator
    {
    public:
        // leaked: records still present in the table (i.e. leaked blocks).
        // dropped: records rejected at the MAX_RECORDS cap.
        static QString generate(const QVector<AllocationRecord> &leaked,
                                bool aggregated, quint64 dropped);

    private:
        static QString formatDirect(const QVector<AllocationRecord> &leaked,
                                    quint64 dropped);
        static QString formatAggregated(const QVector<AllocationRecord> &leaked,
                                        quint64 dropped);
        static QString formatStack(const CallStack &stack);
        static QString formatRecord(const AllocationRecord &rec, int index);
        static QString sourceText(AllocSource source);
        static QString frameText(const StackFrame &frame);
    };

} // namespace memleak

#endif // MEMLEAK_REPORT_H
