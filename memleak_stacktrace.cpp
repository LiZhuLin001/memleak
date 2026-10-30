// ============================================================================
// memleak_stacktrace.cpp - stack capture & symbolization.
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
// memleak_stacktrace.cpp - platform implementations of capture/symbolize.
//
//   Capture  : CaptureStackBackTrace (Windows, dbghelp) / backtrace (Linux).
//   Symbols  : preferred path calls addr2line in-process (real function
//              names + file:line, requires debug info), with fallbacks:
//              Windows dbghelp + UnDecorateSymbolName (demangled names),
//              Linux dladdr + abi::__cxa_demangle (names only).
// ============================================================================

#include "memleak_stacktrace.h"
#include "memleak_config.h"

#include <cstring>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStringList>

#include <cxxabi.h>    // abi::__cxa_demangle (Linux + MinGW)

#ifdef MEMLEAK_PLATFORM_LINUX
    #include <dlfcn.h>
    #include <execinfo.h>
#endif

#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)
    #include <windows.h>
    #include <dbghelp.h>
#endif

namespace memleak
{

#ifdef MEMLEAK_ENABLED

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
    CallStack CallStack::capture(int maxFrames, int skipFrames)
    {
        QVector<void *> frames;
        if (maxFrames <= 0)
            return CallStack();

        // Stack array, no heap involved (safe inside the re-entrancy guard).
        void *buffer[64];
        const int cap = (maxFrames + skipFrames < 64) ? (maxFrames + skipFrames) : 64;

#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)
        const USHORT n = CaptureStackBackTrace(static_cast<DWORD>(skipFrames),
                                               static_cast<DWORD>(cap),
                                               buffer, nullptr);
        for (USHORT i = 0; i < n; ++i)
            frames.append(buffer[i]);
#elif defined(MEMLEAK_PLATFORM_LINUX)
        const int n = backtrace(buffer, cap);
        for (int i = skipFrames; i < n; ++i)
            frames.append(buffer[i]);
#endif

        return CallStack(frames);
    }

// ---------------------------------------------------------------------------
// Aggregation key
// ---------------------------------------------------------------------------
    QByteArray CallStack::hashKey() const
    {
        QByteArray key;
        key.resize(mFrames.size() * static_cast<int>(sizeof(void *)));
        char *dst = key.data();
        for (void *frame : mFrames)
        {
            std::memcpy(dst, &frame, sizeof(void *));
            dst += sizeof(void *);
        }
        return key;
    }

// ---------------------------------------------------------------------------
// Symbolization (report time only)
// ---------------------------------------------------------------------------
    namespace
    {

// Path of the running executable: Linux /proc/self/exe, Windows module file.
        QByteArray executablePath()
        {
#if defined(MEMLEAK_PLATFORM_LINUX)
            return QFile::symLinkTarget(QStringLiteral("/proc/self/exe")).toUtf8();
#else
            wchar_t buf[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return QByteArray();
            const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                                nullptr, 0, nullptr, nullptr);
            QByteArray out(len, Qt::Uninitialized);
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                    out.data(), len, nullptr, nullptr);
            return out;
#endif
        }

// Lazy one-shot probe: is "addr2line" available on PATH?
        bool addr2lineAvailable()
        {
            static const bool available = []
            {
                QProcess p;
                p.start(QString::fromUtf8(MemLeakConfig::addr2linePath()),
                {QStringLiteral("--version")});
                if (!p.waitForStarted(1500))
                    return false;
                if (!p.waitForFinished(3000))
                    return false;
                return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
            }();
            return available;
        }

// Load address of the executable image. backtrace()/CaptureStackBackTrace
// return runtime addresses (PIE base + link-time offset); addr2line expects
// the link-time (file) addresses, so every address is offset by this base.
        quintptr imageBase()
        {
#if defined(MEMLEAK_PLATFORM_LINUX)
            QFile f(QStringLiteral("/proc/self/maps"));
            if (!f.open(QIODevice::ReadOnly))
                return 0;
            const QByteArray first = f.readLine().trimmed();
            const int dash = first.indexOf('-');
            if (dash <= 0)
                return 0;
            bool ok = false;
            const quintptr base = first.left(dash).toULongLong(&ok, 16);
            return ok ? base : 0;
#else
            // The module handle of the main executable equals its load base.
            return reinterpret_cast<quintptr>(GetModuleHandleW(nullptr));
#endif
        }

#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)

// Static ImageBase recorded in the PE header. addr2line (BFD) resolves PE
// addresses in that static address space (ImageBase + RVA), whereas runtime
// addresses come from the ASLR-relocated load base. The file address of a
// frame is therefore ImageBase + (runtime - actualBase).
        quintptr peImageBase()
        {
            const HMODULE h = GetModuleHandleW(nullptr);
            if (!h)
                return 0;
            const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(h);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return 0;
            const quint8 *raw = reinterpret_cast<const quint8 *>(h);
            const IMAGE_NT_HEADERS *nt =
                reinterpret_cast<const IMAGE_NT_HEADERS *>(raw + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return 0;
            return static_cast<quintptr>(nt->OptionalHeader.ImageBase);
        }

#endif // MSVC || MINGW

// Batch-resolve frame addresses via one addr2line invocation per chunk
// (keeps the command line well below ARG_MAX). Fills symbol + file:line.
        bool symbolizeWithAddr2line(const QVector<void *> &frames, QVector<StackFrame> &out)
        {
            const QByteArray exe = executablePath();
            if (exe.isEmpty())
                return false;

            const quintptr base = imageBase();
            const QString exeStr = QString::fromUtf8(exe);
            const QString moduleName = QFileInfo(exeStr).fileName();
            constexpr int kChunk = 200;

            for (int baseIdx = 0; baseIdx < frames.size(); baseIdx += kChunk)
            {
                const int n = qMin(kChunk, frames.size() - baseIdx);
                QStringList args;
                args << QStringLiteral("--basenames")
                     << QStringLiteral("-f") << QStringLiteral("-C")
                     << QStringLiteral("-e") << exeStr;
                for (int i = 0; i < n; ++i)
                {
#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)
                    // PE: map the runtime address back into the file's static
                    // address space expected by BFD (see peImageBase()).
                    const quintptr linkAddr =
                        peImageBase() +
                        (reinterpret_cast<quintptr>(frames[baseIdx + i]) - base);
#else
                    // ELF: subtract the PIE load base to get the link-time address.
                    const quintptr linkAddr =
                        reinterpret_cast<quintptr>(frames[baseIdx + i]) - base;
#endif
                    args << QStringLiteral("0x") + QString::number(linkAddr, 16);
                }

                QProcess p;
                p.start(QString::fromUtf8(MemLeakConfig::addr2linePath()), args);
                if (!p.waitForStarted(1500) || !p.waitForFinished(10000))
                {
                    out.clear();
                    return false;
                }
                if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
                {
                    out.clear();
                    return false;
                }

                const QList<QByteArray> lines =
                    p.readAllStandardOutput().split('\n');
                // addr2line emits two lines per address: function, then file:line.
                for (int i = 0; i < n; ++i)
                {
                    const QByteArray func =
                        (i * 2 < lines.size()) ? lines[i * 2] : QByteArray();
                    const QByteArray loc =
                        (i * 2 + 1 < lines.size()) ? lines[i * 2 + 1] : QByteArray();

                    StackFrame f;
                    f.address = frames[baseIdx + i];
                    f.module = moduleName.toUtf8();

                    const QByteArray fTrim = func.trimmed();
                    if (!fTrim.isEmpty() && fTrim != "??")
                    {
                        // addr2line may emit GCC Itanium mangled names on PE targets
                        // (MinGW, e.g. "Z17leakViaNewNothrowv") when its -C flag is
                        // ineffective; demangle defensively so reports show real
                        // function names on every platform.
                        QByteArray in = fTrim;
                        if (in.startsWith("_Z") || in.startsWith("Z"))
                        {
                            if (!in.startsWith('_'))
                                in.prepend('_');   // 32-bit MinGW drops the leading _
                            int status = 0;
                            char *d = abi::__cxa_demangle(
                                          in.constData(), nullptr, nullptr, &status);
                            if (status == 0 && d)
                            {
                                f.symbol = d;
                                std::free(d);   // guarded: dump runs under gInHook
                            }
                            else
                            {
                                f.symbol = fTrim;
                            }
                        }
                        else
                        {
                            f.symbol = fTrim;
                        }
                    }
                    const QByteArray lTrim = loc.trimmed();
                    if (!lTrim.isEmpty() && !lTrim.startsWith("??"))
                        f.fileLine = lTrim;
                    out.append(f);
                }
            }
            return true;
        }

#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)

// One-shot symbol engine initialization. C++11 guarantees thread-safe
// static initialization; SymInitialize must run once per process.
        bool initSymbolEngine()
        {
            return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }

#endif

    } // namespace

    QVector<StackFrame> CallStack::symbolize() const
    {
        QVector<StackFrame> result;
        result.reserve(mFrames.size());

#if !defined(MEMLEAK_PLATFORM_MSVC) || !defined(_DEBUG)
        // addr2line needs the executable file; on MSVC the PDB-backed dbghelp
        // path is at least as good, so prefer addr2line only where it can read
        // the binary's own debug info (Linux and MinGW builds).
        if (MemLeakConfig::addr2lineEnabled() && addr2lineAvailable())
        {
            if (symbolizeWithAddr2line(mFrames, result))
                return result;
            result.clear();
            result.reserve(mFrames.size());
        }
#endif

        for (void *addr : mFrames)
        {
            StackFrame f;
            f.address = addr;

#if defined(MEMLEAK_PLATFORM_MSVC) || defined(MEMLEAK_PLATFORM_MINGW)
            static const bool symReady = initSymbolEngine();
            if (symReady)
            {
                const DWORD64 daddr = reinterpret_cast<DWORD64>(addr);

                alignas(SYMBOL_INFO) unsigned char nameBuf[sizeof(SYMBOL_INFO) + 512];
                SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(nameBuf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 511;
                if (SymFromAddr(GetCurrentProcess(), daddr, nullptr, sym))
                {
                    // dbghelp returns C++-mangled names; undo the decoration so
                    // reports show real function names.
                    char demangled[512];
                    if (UnDecorateSymbolName(sym->Name, demangled,
                                             sizeof(demangled), 0))
                        f.symbol = demangled;
                    else
                        f.symbol = sym->Name;
                    f.offset = static_cast<quintptr>(daddr - sym->Address);
                }

                IMAGEHLP_LINE64 line;
                DWORD disp = 0;
                if (SymGetLineFromAddr64(GetCurrentProcess(), daddr, &disp, &line))
                {
                    f.module = line.FileName;
                    f.fileLine = QByteArray(line.FileName) + ":" +
                                 QByteArray::number(static_cast<int>(line.LineNumber));
                }
            }
#elif defined(MEMLEAK_PLATFORM_LINUX)
            Dl_info info;
            std::memset(&info, 0, sizeof(info));
            if (dladdr(addr, &info) != 0)
            {
                if (info.dli_fname)
                    f.module = info.dli_fname;
                if (info.dli_sname)
                {
                    int status = 0;
                    char *demangled = abi::__cxa_demangle(
                                          info.dli_sname, nullptr, nullptr, &status);
                    if (status == 0 && demangled)
                    {
                        f.symbol = demangled;
                        std::free(demangled);   // guarded: dump runs under gInHook
                    }
                    else
                    {
                        f.symbol = info.dli_sname;
                    }
                    f.offset = static_cast<quintptr>(
                                   static_cast<char *>(addr) - static_cast<char *>(info.dli_saddr));
                }
            }
#endif

            result.append(f);
        }
        return result;
    }

#endif // MEMLEAK_ENABLED

} // namespace memleak
