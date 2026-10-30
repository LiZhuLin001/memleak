// ============================================================================
// memleak.cpp - core implementation.
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
// memleak.cpp - core implementation.
//
// Interception channels:
//   A) operator new/delete overloads (C++17, all 20 forms) - all platforms.
//   B) malloc/free/calloc/realloc:
//        - Linux / MinGW : --wrap linker technique (see memleak.pri). The
//          real allocators are reached via __real_* symbols; everything in
//          this file deliberately uses __real_* on those platforms so the
//          wrappers never see the detector's own calls.
//        - MSVC (debug)  : _CrtSetAllocHook CRT allocation hook.
//
// Re-entrancy: a thread_local flag is set around every record path. Any
// allocation performed internally (QHash growth, QString, ...) then goes
// straight to the raw allocator and is never recorded, which prevents
// infinite recursion.
//
// Idempotency: AllocationTable::insert/remove are idempotent, so if both
// channels observe one allocation (e.g. MSVC: operator new -> malloc ->
// CRT hook) the record is still written exactly once.
// ============================================================================

#include "memleak.h"
#include "memleak_config.h"
#include "memleak_record.h"
#include "memleak_report.h"
#include "memleak_stacktrace.h"

#include <cstdlib>
#include <new>
#include <thread>

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>

#ifdef MEMLEAK_PLATFORM_MSVC
    #include <malloc.h>      // _aligned_malloc / _aligned_free
    #include <crtdbg.h>      // _CrtSetAllocHook (debug CRT)
    #include <windows.h>     // HeapAlloc / HeapFree
#endif

#ifdef MEMLEAK_PLATFORM_MINGW
    #include <malloc.h>
    #include <windows.h>
#endif

#ifdef MEMLEAK_PLATFORM_LINUX
    #include <stdlib.h>      // posix_memalign
#endif

// ---- Compiler language-standard detection ----------------------------------
// C++17 brings aligned new/delete (std::align_val_t); C++14 brings sized
// deallocation. Detection is compiler-generic: GCC/Clang expose __cplusplus,
// MSVC keeps __cplusplus at 199711L without /Zc:__cplusplus and needs
// _MSVC_LANG instead. Override either macro to force a level.
#if defined(_MSVC_LANG)
    #define MEMLEAK_CPLUSPLUS _MSVC_LANG
#else
    #define MEMLEAK_CPLUSPLUS __cplusplus
#endif
#if MEMLEAK_CPLUSPLUS >= 201703L
    #ifndef MEMLEAK_HAS_ALIGNED_NEW
        #define MEMLEAK_HAS_ALIGNED_NEW
    #endif
#endif
#if MEMLEAK_CPLUSPLUS >= 201402L
    #ifndef MEMLEAK_HAS_SIZED_DELETE
        #define MEMLEAK_HAS_SIZED_DELETE
    #endif
#endif

// True allocators. On --wrap platforms every raw call goes through __real_*;
// on MSVC plain allocations use HeapAlloc so channel A does not trip the
// CRT hook (channel B is responsible for CRT allocations). Declared outside
// MEMLEAK_ENABLED so the --wrap shims at the end of this file can forward
// even when the detector is compiled out.
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
extern "C" {
    void *__real_malloc(std::size_t size);
    void  __real_free(void *ptr);
    void *__real_calloc(std::size_t count, std::size_t size);
    void *__real_realloc(void *ptr, std::size_t size);
}
#endif

#ifdef MEMLEAK_ENABLED

namespace memleak
{
    // ---------------------------------------------------------------------------
    // Internal plumbing
    // ---------------------------------------------------------------------------
    void *doAllocate(std::size_t size, std::size_t align);
    void  doDeallocate(void *ptr, std::size_t size, std::size_t align);

    // Set around every recording path; internal allocations made while set are
    // served raw and never recorded.
    thread_local bool gInHook = false;

    // Global enable switch (atomically readable from any thread).
    static QAtomicInt gEnabled(1);

    namespace
    {
        bool isOverAligned(std::size_t align)
        {
            return align > alignof(std::max_align_t);
        }

        void *rawAlloc(std::size_t size, std::size_t align)
        {
            if (!isOverAligned(align))
            {
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
                return __real_malloc(size);
#else
                return HeapAlloc(GetProcessHeap(), 0, size);
#endif
            }
#ifdef MEMLEAK_PLATFORM_LINUX
            void *p = nullptr;
            return (posix_memalign(&p, align, size) == 0) ? p : nullptr;
#else
            return _aligned_malloc(size, align);   // MSVC / MinGW
#endif
        }

        void rawFree(void *ptr, std::size_t align)
        {
            if (!isOverAligned(align))
            {
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
                __real_free(ptr);
#else
                HeapFree(GetProcessHeap(), 0, ptr);
#endif
            }
            else
            {
#ifdef MEMLEAK_PLATFORM_LINUX
                __real_free(ptr);                  // posix_memalign blocks free() cleanly
#else
                _aligned_free(ptr);
#endif
            }
        }
    } // namespace
// ---------------------------------------------------------------------------
// MSVC CRT allocation hook (channel B on MSVC debug builds)
// ---------------------------------------------------------------------------
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)

    static int __cdecl crtAllocHook(int allocType, void *userData, std::size_t size,
                                    int blockType, long requestNumber,
                                    const unsigned char *filename, int lineNumber);

#endif
    // ---------------------------------------------------------------------------
    // MemLeakDetector facade
    // ---------------------------------------------------------------------------
    struct MemLeakDetector::Impl
    {
        Impl()
            : table(MemLeakConfig::maxRecords())
        {
#ifndef MEMLEAK_NO_ELAPSED_TIMER
            timer.start();
#endif
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)
            oldHook = nullptr;
            hookInstalled = false;
#endif
        }

        ~Impl()
        {
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)
            if (hookInstalled)
            {
                _CrtSetAllocHook(oldHook);
                hookInstalled = false;
            }
#endif
        }

        AllocationTable table;
#ifndef MEMLEAK_NO_ELAPSED_TIMER
        QElapsedTimer timer;
#endif
        QMutex reportMutex;

#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)
        _CrtAllocHookFn oldHook = nullptr;
        bool hookInstalled = false;
#endif
    };

    // Internal accessor: the interception channels reach the private Pimpl data
    // only through this friend (see memleak.h).
    struct DetectorCore
    {
        static MemLeakDetector::Impl *impl()
        {
            return MemLeakDetector::instance().d;
        }
    };

    MemLeakDetector &MemLeakDetector::instance()
    {
        static MemLeakDetector inst;
        return inst;
    }

    MemLeakDetector::MemLeakDetector()
        : d(new Impl)
    {
        start();
    }

    MemLeakDetector::~MemLeakDetector()
    {
        stop();
        delete d;
    }

    void MemLeakDetector::start()
    {
        gEnabled.storeRelease(1);
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)
        if (!d->hookInstalled)
        {
            d->oldHook = _CrtSetAllocHook(&crtAllocHook);
            d->hookInstalled = true;
        }
#endif
    }

    void MemLeakDetector::stop()
    {
        gEnabled.storeRelease(0);
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)
        if (d->hookInstalled)
        {
            _CrtSetAllocHook(d->oldHook);
            d->hookInstalled = false;
        }
#endif
    }

    bool MemLeakDetector::isEnabled() const
    {
        return gEnabled.loadAcquire() != 0;
    }

    void MemLeakDetector::dump()
    {
        QMutexLocker locker(&d->reportMutex);

        // Protect the whole report path (symbolization, QString building,
        // fwrite) from being recorded on this thread.
        gInHook = true;

        const QVector<AllocationRecord> leaked = d->table.snapshot();
        // Aggregated mode is preferred when both report macros are defined.
        const bool aggregated = MemLeakConfig::reportAggregated();

        const QString text = ReportGenerator::generate(
                                 leaked, aggregated, d->table.droppedCount());

        if (MemLeakConfig::outputConsole())
        {
            ConsoleSink sink;
            sink.write(text);
        }
        if (MemLeakConfig::outputFile())
        {
            FileSink sink(QString::fromUtf8(MemLeakConfig::logFilePath()));
            sink.write(text);
        }
        gInHook = false;
    }

    void MemLeakDetector::reset()
    {
        QMutexLocker locker(&d->reportMutex);
        d->table.clear();
#ifndef MEMLEAK_NO_ELAPSED_TIMER
        d->timer.restart();
#endif
    }

// ---------------------------------------------------------------------------
// Channel A kernel: operator new/delete
// ---------------------------------------------------------------------------
    void *doAllocate(std::size_t size, std::size_t align)
    {
        if (gInHook || !gEnabled.loadAcquire() || !MemLeakConfig::trackNew())
            return rawAlloc(size, align);

        gInHook = true;
        void *p = rawAlloc(size, align);
        if (p)
        {
            AllocationRecord rec;
            rec.ptr = p;
            rec.size = size;
            rec.alignment = align;
            rec.source = AllocSource::New;
            rec.threadId = std::this_thread::get_id();
#ifndef MEMLEAK_NO_ELAPSED_TIMER
            rec.timestampMs = DetectorCore::impl()->timer.elapsed();
#endif
            rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
            DetectorCore::impl()->table.insert(rec);
        }
        gInHook = false;
        return p;
    }

    void doDeallocate(void *ptr, std::size_t size, std::size_t align)
    {
        (void)size;
        if (!ptr)
            return;
        if (gInHook || !gEnabled.loadAcquire())
        {
            rawFree(ptr, align);
            return;
        }

        gInHook = true;
        if (MemLeakConfig::trackNew())
            DetectorCore::impl()->table.remove(ptr);
        rawFree(ptr, align);
        gInHook = false;
    }

// ---------------------------------------------------------------------------
// Channel B kernel: malloc/free/calloc/realloc (Linux / MinGW --wrap)
// ---------------------------------------------------------------------------
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
    namespace
    {
        void *mallocEntry(std::size_t size)
        {
            if (gInHook || !gEnabled.loadAcquire() || !MemLeakConfig::trackMalloc())
                return __real_malloc(size);

            gInHook = true;
            void *p = __real_malloc(size);
            if (p)
            {
                AllocationRecord rec;
                rec.ptr = p;
                rec.size = size;
                rec.source = AllocSource::Malloc;
                rec.threadId = std::this_thread::get_id();
#ifndef MEMLEAK_NO_ELAPSED_TIMER
                rec.timestampMs = DetectorCore::impl()->timer.elapsed();
#endif
                rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
                DetectorCore::impl()->table.insert(rec);
            }
            gInHook = false;
            return p;
        }

        void freeEntry(void *ptr)
        {
            if (!ptr)
                return;
            if (gInHook || !gEnabled.loadAcquire())
            {
                __real_free(ptr);
                return;
            }

            gInHook = true;
            if (MemLeakConfig::trackMalloc())
                DetectorCore::impl()->table.remove(ptr);
            __real_free(ptr);
            gInHook = false;
        }

        void *callocEntry(std::size_t count, std::size_t elemSize)
        {
            if (gInHook || !gEnabled.loadAcquire() || !MemLeakConfig::trackMalloc())
                return __real_calloc(count, elemSize);

            gInHook = true;
            void *p = __real_calloc(count, elemSize);
            if (p)
            {
                // p != nullptr implies the multiplication did not overflow.
                AllocationRecord rec;
                rec.ptr = p;
                rec.size = count * elemSize;
                rec.source = AllocSource::Malloc;
                rec.threadId = std::this_thread::get_id();
#ifndef MEMLEAK_NO_ELAPSED_TIMER
                rec.timestampMs = DetectorCore::impl()->timer.elapsed();
#endif
                rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
                DetectorCore::impl()->table.insert(rec);
            }
            gInHook = false;
            return p;
        }

        void *reallocEntry(void *oldPtr, std::size_t size)
        {
            if (gInHook || !gEnabled.loadAcquire() || !MemLeakConfig::trackMalloc())
                return __real_realloc(oldPtr, size);

            gInHook = true;
            void *newPtr = __real_realloc(oldPtr, size);
            if (newPtr)
            {
                auto impl = DetectorCore::impl();
                if (oldPtr)
                    impl->table.remove(oldPtr);
                if (size > 0)
                {
                    AllocationRecord rec;
                    rec.ptr = newPtr;
                    rec.size = size;
                    rec.source = AllocSource::Malloc;
                    rec.threadId = std::this_thread::get_id();
#ifndef MEMLEAK_NO_ELAPSED_TIMER
                    rec.timestampMs = impl->timer.elapsed();
#endif
                    rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
                    impl->table.insert(rec);
                }
            }
            else if (oldPtr && size == 0)
            {
                // realloc(p, 0): implementation-defined; glibc frees p and returns
                // nullptr. Best effort: drop the record.
                DetectorCore::impl()->table.remove(oldPtr);
            }
            gInHook = false;
            return newPtr;
        }
    } // namespace
#endif // Linux / MinGW wrap platform

// ---------------------------------------------------------------------------
// MSVC CRT allocation hook implementation
// ---------------------------------------------------------------------------
#if defined(MEMLEAK_PLATFORM_MSVC) && defined(_DEBUG) && defined(MEMLEAK_TRACK_MALLOC)

// Sees every CRT allocation in debug builds. Channel A already covers
// operator new; this hook catches plain malloc/free/calloc/realloc calls.
// Notes:
//   * _HOOK_REALLOC carries only the new block pointer; the old pointer is
//     unrecoverable here, so a stale record may remain (documented limit).
//   * All recording happens under the re-entrancy guard; internal
//     allocations of the table are ignored by this very hook.
    static int __cdecl crtAllocHook(int allocType, void *userData, std::size_t size,
                                    int blockType, long requestNumber,
                                    const unsigned char *filename, int lineNumber)
    {
        (void)blockType;
        (void)requestNumber;
        (void)filename;
        (void)lineNumber;

        if (gInHook || !gEnabled.loadAcquire())
            return TRUE;

        gInHook = true;
        auto impl = DetectorCore::impl();

        if (allocType == _HOOK_ALLOC)
        {
            AllocationRecord rec;
            rec.ptr = userData;
            rec.size = size;
            rec.source = AllocSource::Malloc;
            rec.threadId = std::this_thread::get_id();
            rec.timestampMs = impl->timer.elapsed();
            rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
            impl->table.insert(rec);          // idempotent vs channel A
        }
        else if (allocType == _HOOK_FREE)
        {
            impl->table.remove(userData);
        }
        else if (allocType == _HOOK_REALLOC)
        {
            if (size > 0)
            {
                AllocationRecord rec;
                rec.ptr = userData;
                rec.size = size;
                rec.source = AllocSource::Malloc;
                rec.threadId = std::this_thread::get_id();
                rec.timestampMs = impl->timer.elapsed();
                rec.stack = CallStack::capture(MemLeakConfig::maxStackFrames(), 3);
                impl->table.insert(rec);      // old block record may linger
            }
        }
        gInHook = false;
        return TRUE;
    }

#endif // MSVC debug + MEMLEAK_TRACK_MALLOC

} // namespace memleak

// ---------------------------------------------------------------------------
// Channel A: global operator new/delete overloads (C++17, all forms)
// ---------------------------------------------------------------------------
#if defined(MEMLEAK_ENABLED) && defined(MEMLEAK_TRACK_NEW)

void *operator new (std::size_t size)
{
    void *p = memleak::doAllocate(size, 0);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void *operator new[](std::size_t size)
{
    void *p = memleak::doAllocate(size, 0);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void *operator new (std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return memleak::doAllocate(size, 0);
    }
    catch (...)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return memleak::doAllocate(size, 0);
    }
    catch (...)
    {
        return nullptr;
    }
}

#ifdef MEMLEAK_HAS_ALIGNED_NEW

void *operator new (std::size_t size, std::align_val_t align)
{
    void *p = memleak::doAllocate(size, static_cast<std::size_t>(align));
    if (!p)
        throw std::bad_alloc();
    return p;
}

void *operator new[](std::size_t size, std::align_val_t align)
{
    void *p = memleak::doAllocate(size, static_cast<std::size_t>(align));
    if (!p)
        throw std::bad_alloc();
    return p;
}

void *operator new (std::size_t size, std::align_val_t align,
                    const std::nothrow_t &) noexcept
{
    try
    {
        return memleak::doAllocate(size, static_cast<std::size_t>(align));
    }
    catch (...)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t &) noexcept
{
    try
    {
        return memleak::doAllocate(size, static_cast<std::size_t>(align));
    }
    catch (...)
    {
        return nullptr;
    }
}

#endif // MEMLEAK_HAS_ALIGNED_NEW

void operator delete (void *ptr) noexcept
{
    memleak::doDeallocate(ptr, 0, 0);
}

void operator delete[](void *ptr) noexcept
{
    memleak::doDeallocate(ptr, 0, 0);
}

void operator delete (void *ptr, const std::nothrow_t &) noexcept
{
    memleak::doDeallocate(ptr, 0, 0);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
    memleak::doDeallocate(ptr, 0, 0);
}

#ifdef MEMLEAK_HAS_SIZED_DELETE

void operator delete (void *ptr, std::size_t size) noexcept
{
    memleak::doDeallocate(ptr, size, 0);
}

void operator delete[](void *ptr, std::size_t size) noexcept
{
    memleak::doDeallocate(ptr, size, 0);
}

#endif // MEMLEAK_HAS_SIZED_DELETE

#ifdef MEMLEAK_HAS_ALIGNED_NEW

void operator delete (void *ptr, std::align_val_t align) noexcept
{
    memleak::doDeallocate(ptr, 0, static_cast<std::size_t>(align));
}

void operator delete[](void *ptr, std::align_val_t align) noexcept
{
    memleak::doDeallocate(ptr, 0, static_cast<std::size_t>(align));
}

void operator delete (void *ptr, std::size_t size, std::align_val_t align) noexcept
{
    memleak::doDeallocate(ptr, size, static_cast<std::size_t>(align));
}

void operator delete[](void *ptr, std::size_t size, std::align_val_t align) noexcept
{
    memleak::doDeallocate(ptr, size, static_cast<std::size_t>(align));
}

void operator delete (void *ptr, std::align_val_t align,
                      const std::nothrow_t &) noexcept
{
    memleak::doDeallocate(ptr, 0, static_cast<std::size_t>(align));
}

void operator delete[](void *ptr, std::align_val_t align,
                       const std::nothrow_t &) noexcept
{
    memleak::doDeallocate(ptr, 0, static_cast<std::size_t>(align));
}

#endif // MEMLEAK_HAS_ALIGNED_NEW

#endif // MEMLEAK_ENABLED && MEMLEAK_TRACK_NEW

#endif // MEMLEAK_ENABLED

// --wrap shims are defined outside MEMLEAK_ENABLED so the linker options in
// memleak.pri remain valid even when the detector is compiled out: in that
// case each shim simply forwards to the real allocator (zero overhead).
#if defined(MEMLEAK_PLATFORM_LINUX) || defined(MEMLEAK_PLATFORM_MINGW)
extern "C" {

#if defined(MEMLEAK_ENABLED) && defined(MEMLEAK_TRACK_MALLOC)
    void *__wrap_malloc(std::size_t size)
    {
        return memleak::mallocEntry(size);
    }
    void  __wrap_free(void *ptr)
    {
        memleak::freeEntry(ptr);
    }
    void *__wrap_calloc(std::size_t count, std::size_t elemSize)
    {
        return memleak::callocEntry(count, elemSize);
    }
    void *__wrap_realloc(void *ptr, std::size_t size)
    {
        return memleak::reallocEntry(ptr, size);
    }
#else
    void *__wrap_malloc(std::size_t size)
    {
        return __real_malloc(size);
    }
    void  __wrap_free(void *ptr)
    {
        __real_free(ptr);
    }
    void *__wrap_calloc(std::size_t count, std::size_t elemSize)
    {
        return __real_calloc(count, elemSize);
    }
    void *__wrap_realloc(void *ptr, std::size_t size)
    {
        return __real_realloc(ptr, size);
    }
#endif // MEMLEAK_ENABLED && MEMLEAK_TRACK_MALLOC

} // extern "C"
#endif // Linux / MinGW wrap platform
