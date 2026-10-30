// ============================================================================
// memleak_record.h - AllocationRecord & AllocationTable.
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

#ifndef MEMLEAK_RECORD_H
#define MEMLEAK_RECORD_H

// ============================================================================
// memleak_record - Allocation records and the thread-safe record table.
//
// insert()/remove() are idempotent so the two interception channels
// (new/delete overloads vs malloc/free/calloc/realloc wrappers) may both
// observe a single allocation without double counting.
// ============================================================================

#include <cstddef>
#include <thread>

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QVector>

#include "memleak_stacktrace.h"

namespace memleak
{

// Which interception channel produced this record.
    enum class AllocSource
    {
        New,     // operator new / delete channel
        Malloc   // malloc / free / calloc / realloc channel
    };

// One live allocation.
    struct AllocationRecord
    {
        void *ptr = nullptr;
        size_t size = 0;
        size_t alignment = 0;          // 0 == default alignment
        AllocSource source = AllocSource::Malloc;
        std::thread::id threadId;      // allocating thread
        quint64 seq = 0;               // global allocation sequence
        qint64 timestampMs = 0;        // ms since detector start
        CallStack stack;
    };

// Thread-safe allocation table.
    class AllocationTable
    {
    public:
        explicit AllocationTable(int maxRecords);

        void insert(const AllocationRecord &rec);  // idempotent
        void remove(void *ptr);                    // idempotent
        void clear();
        int size() const;
        quint64 droppedCount() const;              // rejected at MAX_RECORDS cap

        QVector<AllocationRecord> snapshot() const;

    private:
        mutable QMutex mMutex;
        QHash<void *, AllocationRecord> mRecords;
        int mMaxRecords;
        quint64 mDropped;
        quint64 mSeq;
    };

} // namespace memleak

#endif // MEMLEAK_RECORD_H
