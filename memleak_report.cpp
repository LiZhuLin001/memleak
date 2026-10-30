// ============================================================================
// memleak_report.cpp - report strategies & output sinks.
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
// memleak_report.cpp - report text generation and output sinks.
// All code here runs with the thread_local re-entrancy flag set by
// MemLeakDetector::dumpReport(), so QString/QByteArray allocations inside
// the report path are never recorded.
// ============================================================================

#include "memleak_report.h"

#include <algorithm>
#include <functional>
#include <numeric>

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QTextStream>

namespace memleak
{

#ifdef MEMLEAK_ENABLED

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------
    void ConsoleSink::write(const QString &text)
    {
        const QByteArray utf8 = text.toUtf8();
        std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
        std::fflush(stdout);
    }

    FileSink::FileSink(const QString &path)
        : mPath(path)
    {
    }

    FileSink::~FileSink()
    {
        if (mFile)
            std::fclose(mFile);
    }

    void FileSink::write(const QString &text)
    {
        if (!mFile)
        {
            const QByteArray nativePath = mPath.toLocal8Bit();
            mFile = std::fopen(nativePath.constData(), "ab");
        }
        if (!mFile)
            return;
        const QByteArray utf8 = text.toUtf8();
        std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), mFile);
        std::fflush(mFile);
    }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
    QString ReportGenerator::sourceText(AllocSource source)
    {
        return source == AllocSource::New ? QStringLiteral("New") : QStringLiteral("Malloc");
    }

    QString ReportGenerator::frameText(const StackFrame &frame)
    {
        QString out;
        if (!frame.symbol.isEmpty())
            out += QString::fromUtf8(frame.symbol);
        else
            out += QStringLiteral("<unknown>");
        if (!frame.fileLine.isEmpty())
            out += QStringLiteral(" [") + QString::fromUtf8(frame.fileLine) + QStringLiteral("]");
        if (!frame.module.isEmpty())
            out += QStringLiteral(" (") + QString::fromUtf8(frame.module) + QStringLiteral(")");
        return out;
    }

    QString ReportGenerator::formatStack(const CallStack &stack)
    {
        const QVector<StackFrame> frames = stack.symbolize();
        QString out;
        int index = 0;
        for (const StackFrame &frame : frames)
        {
            out += QStringLiteral("          #%1  %2  +0x%3\n")
                   .arg(index++)
                   .arg(frameText(frame),
                        QString::number(reinterpret_cast<quintptr>(frame.address), 16));
        }
        return out;
    }

    QString ReportGenerator::formatRecord(const AllocationRecord &rec, int index)
    {
        const QString line = QStringLiteral("  #%1  [%2]  size=%3  align=%4  seq=%5 "
#ifndef MEMLEAK_NO_ELAPSED_TIMER
                                            " t=%8ms  "
#endif
                                            "thread=%6  ptr=0x%7\n")
                             .arg(index)
                             .arg(sourceText(rec.source))
                             .arg(rec.size)
                             .arg(rec.alignment)
                             .arg(rec.seq)
                             .arg(QString::number(std::hash<std::thread::id> {}(rec.threadId)),
                                  QString::number(reinterpret_cast<quintptr>(rec.ptr), 16));
#ifndef MEMLEAK_NO_ELAPSED_TIMER
        .arg(rec.timestampMs)
#endif
        return line + formatStack(rec.stack);
    }

    QString ReportGenerator::formatDirect(const QVector<AllocationRecord> &leaked,
                                          quint64 dropped)
    {
        const quint64 totalBytes = std::accumulate(
                                       leaked.begin(), leaked.end(), quint64(0),
                                       [](quint64 sum, const AllocationRecord & r)
        {
            return sum + r.size;
        });

        QString out;
        QTextStream ts(&out);
        ts << QStringLiteral("==================== memleak report ====================\n")
           << QStringLiteral("mode      : DIRECT\n")
           << QStringLiteral("leaked    : %1 block(s), %2 byte(s)\n")
           .arg(leaked.size())
           .arg(totalBytes)
           << QStringLiteral("dropped   : %1 (rejected at MAX_RECORDS cap)\n\n")
           .arg(dropped);
        if (leaked.isEmpty())
        {
            ts << QStringLiteral("  (no leaks detected)\n");
        }
        else
        {
            int index = 0;
            for (const AllocationRecord &rec : leaked)
                ts << formatRecord(rec, index++);
        }
        ts << QStringLiteral("========================================================\n");
        return out;
    }

    QString ReportGenerator::formatAggregated(const QVector<AllocationRecord> &leaked,
            quint64 dropped)
    {
        // Group by call-stack key + source so that the same site allocated via
        // new vs malloc stays distinguishable.
        struct Group
        {
            int count = 0;
            quint64 bytes = 0;
            QSet<quint64> threadIds;
            CallStack sample;
            AllocSource source = AllocSource::Malloc;
        };
        QHash<QByteArray, Group> groups;

        for (const AllocationRecord &rec : leaked)
        {
            QByteArray key = rec.stack.hashKey();
            key.append(static_cast<char>(rec.source == AllocSource::New ? 1 : 0));
            Group &g = groups[key];
            if (g.count == 0)
            {
                g.sample = rec.stack;
                g.source = rec.source;
            }
            ++g.count;
            g.bytes += rec.size;
            g.threadIds.insert(std::hash<std::thread::id> {}(rec.threadId));
        }

        // Order groups by block count (descending), then by bytes.
        QList<QByteArray> keys = groups.keys();
        std::sort(keys.begin(), keys.end(),
                  [&groups](const QByteArray & a, const QByteArray & b)
        {
            if (groups[a].count != groups[b].count)
                return groups[a].count > groups[b].count;
            return groups[a].bytes > groups[b].bytes;
        });

        const quint64 totalBytes = std::accumulate(
                                       leaked.begin(), leaked.end(), quint64(0),
                                       [](quint64 sum, const AllocationRecord & r)
        {
            return sum + r.size;
        });

        QString out;
        QTextStream ts(&out);
        ts << QStringLiteral("==================== memleak report ====================\n")
           << QStringLiteral("mode      : AGGREGATED\n")
           << QStringLiteral("groups    : %1 group(s)\n").arg(groups.size())
           << QStringLiteral("leaked    : %1 block(s), %2 byte(s)\n")
           .arg(leaked.size())
           .arg(totalBytes)
           << QStringLiteral("dropped   : %1 (rejected at MAX_RECORDS cap)\n\n")
           .arg(dropped);
        if (groups.isEmpty())
        {
            ts << QStringLiteral("  (no leaks detected)\n");
        }
        else
        {
            int gIndex = 0;
            for (const QByteArray &key : keys)
            {
                const Group &g = groups[key];
                ts << QStringLiteral("  Group #%1  blocks=%2  bytes=%3  source=%4  threads=%5\n")
                   .arg(gIndex++)
                   .arg(g.count)
                   .arg(g.bytes)
                   .arg(sourceText(g.source))
                   .arg(g.threadIds.size());
                ts << QStringLiteral("      stack (first allocation):\n");
                ts << formatStack(g.sample);
                ts << QStringLiteral("\n");
            }
        }
        ts << QStringLiteral("========================================================\n");
        return out;
    }

    QString ReportGenerator::generate(const QVector<AllocationRecord> &leaked,
                                      bool aggregated, quint64 dropped)
    {
        return aggregated ? formatAggregated(leaked, dropped)
               : formatDirect(leaked, dropped);
    }

#endif // MEMLEAK_ENABLED

} // namespace memleak
