# ============================================================================
# memleak.pri - Qt pri for the memleak memory leak detector.
#
# Usage: include(memleak.pri) in your .pro — nothing else is required.
#
# Compile-time switches (defaults are injected here when absent; see
# memleak.h for the full list and semantics):
#   MEMLEAK_ENABLED            master switch (undef to compile the tool out)
#   MEMLEAK_TRACK_NEW          channel A: operator new/delete overloads
#   MEMLEAK_TRACK_MALLOC       channel B: malloc/free/calloc/realloc
#   MEMLEAK_OUTPUT_CONSOLE     report to stdout
#   MEMLEAK_OUTPUT_FILE        report to MEMLEAK_LOG_FILE ("memleak.log")
#   MEMLEAK_REPORT_DIRECT      one entry per leaked block
#   MEMLEAK_REPORT_AGGREGATED  grouped by call stack (preferred if both set)
#   MEMLEAK_MAX_STACK_FRAMES   frames captured per allocation (default 16)
#   MEMLEAK_MAX_RECORDS        allocation table cap (default 100000)
#   MEMLEAK_AUTO_DUMP_ON_EXIT  automatically print report on exit
#   MEMLEAK_NO_ELAPSED_TIMER   hide elapsed time
#
# Switching behaviour:
#   * Set DEFINES BEFORE include() to override (injection is skipped for
#     macros already present).
#   * Or adjust DEFINES AFTER include(), e.g.
#       DEFINES -= MEMLEAK_OUTPUT_CONSOLE
#       DEFINES += MEMLEAK_OUTPUT_FILE
#     (for channel switches prefer deciding before include so the --wrap
#      link options follow the macros).
# ============================================================================

MEMLEAK_ROOT = $$PWD
DEFINES += MEMLEAK_VERSION=\\\"1.0.0\\\"

INCLUDEPATH += $$MEMLEAK_ROOT

HEADERS += \
    $$MEMLEAK_ROOT/memleak.h \
    $$MEMLEAK_ROOT/memleak_config.h \
    $$MEMLEAK_ROOT/memleak_stacktrace.h \
    $$MEMLEAK_ROOT/memleak_record.h \
    $$MEMLEAK_ROOT/memleak_report.h

SOURCES += \
    $$MEMLEAK_ROOT/memleak.cpp \
    $$MEMLEAK_ROOT/memleak_config.cpp \
    $$MEMLEAK_ROOT/memleak_stacktrace.cpp \
    $$MEMLEAK_ROOT/memleak_record.cpp \
    $$MEMLEAK_ROOT/memleak_report.cpp

# ---- Default macros (injected only when absent) -----------------------------
# NOTE: qmake's contains() is an exact per-item match, so value-carrying
# macros are configured through the qmake variables below (set them BEFORE
# include to override the defaults). Plain on/off macros can also be given
# via DEFINES before include; adjust DEFINES after include to switch off.
# IMPORTANT: in qmake 3.x a "cond:" followed by an indented line on the next
# line is NOT a conditional block (it always runs). Conditional statements
# must sit on the same line or inside braces, as done here.
!contains(DEFINES, MEMLEAK_AUTO_DUMP_ON_EXIT): {
    DEFINES += MEMLEAK_AUTO_DUMP_ON_EXIT
}
!contains(DEFINES, MEMLEAK_ENABLED): {
    DEFINES += MEMLEAK_ENABLED
}
!contains(DEFINES, MEMLEAK_TRACK_NEW): {
    DEFINES += MEMLEAK_TRACK_NEW
}
!contains(DEFINES, MEMLEAK_TRACK_MALLOC): {
    DEFINES += MEMLEAK_TRACK_MALLOC
}
!contains(DEFINES, MEMLEAK_OUTPUT_CONSOLE): {
    DEFINES += MEMLEAK_OUTPUT_CONSOLE
}
!contains(DEFINES, MEMLEAK_REPORT_DIRECT): {
    DEFINES += MEMLEAK_REPORT_DIRECT
}
!contains(DEFINES, MEMLEAK_USE_ADDR2LINE): {
    DEFINES += MEMLEAK_USE_ADDR2LINE
}
!contains(DEFINES, MEMLEAK_NO_ELAPSED_TIMER): {
    DEFINES += MEMLEAK_NO_ELAPSED_TIMER
}

# Stack-frame count per allocation and record-table cap.
#   MEMLEAK_STACK_FRAMES = 8     (default 16)
#   MEMLEAK_RECORDS      = 50000 (default 100000)
!defined(MEMLEAK_STACK_FRAMES, var): {
    MEMLEAK_STACK_FRAMES = 16
}
!defined(MEMLEAK_RECORDS, var): {
    MEMLEAK_RECORDS = 100000
}
!contains(DEFINES, "MEMLEAK_MAX_STACK_FRAMES=$$MEMLEAK_STACK_FRAMES"): {
    DEFINES += MEMLEAK_MAX_STACK_FRAMES=$$MEMLEAK_STACK_FRAMES
}
!contains(DEFINES, "MEMLEAK_MAX_RECORDS=$$MEMLEAK_RECORDS"): {
    DEFINES += MEMLEAK_MAX_RECORDS=$$MEMLEAK_RECORDS
}

# addr2line executable path. When unset, the library falls back to
# resolving "addr2line" on PATH (same as the default).
#   MEMLEAK_ADDR2LINE = /opt/toolchain/bin/addr2line
defined(MEMLEAK_ADDR2LINE, var): {
    DEFINES += MEMLEAK_ADDR2LINE_PATH=\\\"$${MEMLEAK_ADDR2LINE}\\\"
}

# ---- Platform branches ------------------------------------------------------
win32-msvc* {
    DEFINES += MEMLEAK_PLATFORM_MSVC
    # Symbol resolution (CaptureStackBackTrace / SymFromAddr) needs dbghelp.
    LIBS += -ldbghelp
    # The CRT allocation hook is enabled in code for debug builds
    # (MEMLEAK_TRACK_MALLOC); no extra link options required.
}

win32-g++* {
    DEFINES += MEMLEAK_PLATFORM_MINGW
    LIBS += -ldbghelp
    contains(DEFINES, MEMLEAK_TRACK_MALLOC) {
        QMAKE_LFLAGS += -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
    }
}

linux* {
    DEFINES += MEMLEAK_PLATFORM_LINUX
    # -rdynamic exposes dynamic symbols to backtrace/dladdr so function
    # names appear in reports (otherwise only addresses are available).
    QMAKE_LFLAGS += -rdynamic
    LIBS += -ldl
    contains(DEFINES, MEMLEAK_TRACK_MALLOC) {
        QMAKE_LFLAGS += -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
    }
}
