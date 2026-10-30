# ============================================================================
# memleak example - multi-threaded leak demo, also used to verify the
# detector on each platform.
#
#   qmake && make && ./memleak_example
# ============================================================================

QT += core
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = memleak_example

# ---- Compile-time switch demo ----------------------------------------------
# Defaults (injected by memleak.pri): console output + DIRECT report +
# both channels (new/delete + malloc/free/calloc/realloc) enabled.
#
# Switch examples (uncomment to try, then rebuild):
#
#   # report to file instead of console:
#   # DEFINES -= MEMLEAK_OUTPUT_CONSOLE
#   # DEFINES += MEMLEAK_OUTPUT_FILE
#
#   # both console and file:
#   # DEFINES += MEMLEAK_OUTPUT_FILE
#
#   # aggregated report (grouped by call stack):
#   # DEFINES += MEMLEAK_REPORT_AGGREGATED
#
#   # custom stack-frame count per allocation (qmake variable, default 16):
#   # MEMLEAK_STACK_FRAMES = 8
#
#   # custom record-table cap (default 100000):
#   # MEMLEAK_RECORDS = 50000
#
#   # disable channel B (malloc/free/...): decide BEFORE include so the
#   # --wrap link options in memleak.pri stay consistent:
#   # DEFINES -= MEMLEAK_TRACK_MALLOC
#
#   # disable the whole tool:
#   # DEFINES -= MEMLEAK_ENABLED

MEMLEAK_ADDR2LINE = D:/Qt/Tools/mingw810_32/bin/addr2line
include(../memleak.pri)

SOURCES += main.cpp
