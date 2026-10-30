// ============================================================================
// memleak_record.cpp - AllocationRecord & AllocationTable.
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
// memleak_record.cpp - AllocationTable implementation.
// All container operations may allocate internally (QHash growth); callers
// always invoke them with the thread_local re-entrancy flag set, so those
// internal allocations are served raw and never recorded.
// ============================================================================

#include "memleak_record.h"

namespace memleak
{

#ifdef MEMLEAK_ENABLED

    AllocationTable::AllocationTable(int maxRecords)
        : mMaxRecords(maxRecords)
        , mDropped(0)
        , mSeq(0)
    {
    }

    void AllocationTable::insert(const AllocationRecord &rec)
    {
        QMutexLocker locker(&mMutex);
        if (mRecords.contains(rec.ptr))
        {
            // Idempotent: both channels may report the same allocation.
            return;
        }
        if (mMaxRecords > 0 && mRecords.size() >= mMaxRecords)
        {
            ++mDropped;
            return;
        }
        AllocationRecord copy = rec;
        copy.seq = ++mSeq;
        mRecords.insert(copy.ptr, copy);
    }

    void AllocationTable::remove(void *ptr)
    {
        QMutexLocker locker(&mMutex);
        mRecords.remove(ptr);   // QHash::remove on a missing key is a no-op.
    }

    void AllocationTable::clear()
    {
        QMutexLocker locker(&mMutex);
        mRecords.clear();
    }

    int AllocationTable::size() const
    {
        QMutexLocker locker(&mMutex);
        return mRecords.size();
    }

    quint64 AllocationTable::droppedCount() const
    {
        QMutexLocker locker(&mMutex);
        return mDropped;
    }

    QVector<AllocationRecord> AllocationTable::snapshot() const
    {
        QMutexLocker locker(&mMutex);
        const QList<AllocationRecord> values = mRecords.values();
        return QVector<AllocationRecord>(values.begin(), values.end());
    }

#endif // MEMLEAK_ENABLED

} // namespace memleak
