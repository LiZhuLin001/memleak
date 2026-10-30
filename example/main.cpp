// ============================================================================
// memleak example - multi-threaded leak demo.
//
// Verifies on every platform:
//   * channel A (operator new/delete, incl. nothrow and aligned forms),
//   * channel B (malloc / calloc / realloc),
//   * multi-thread recording with per-thread leak sites,
//   * DIRECT vs AGGREGATED reports, console vs file output (via macros).
// ============================================================================

#include "memleak.h"

#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

using memleak::MemLeakDetector;

// Compiler-defeater: keeps allocation results observable so -O2 cannot
// eliminate "allocated but never used" leak sites.
volatile void* gSink = nullptr;

// ---------------------------------------------------------------------------
// Leak sites at different call depths (exercise stack capture).
// ---------------------------------------------------------------------------

struct alignas(64) AlignedBlock {
    char data[64];
};

void leakViaNew(int depth)
{
    if (depth > 0) {
        leakViaNew(depth - 1);
        return;
    }
    int* p = new int[128];               // intentional leak (channel A)
    gSink = p;
}

void leakViaNewNothrow()
{
    char* p = new (std::nothrow) char[256];   // intentional leak (channel A)
    gSink = p;
}

void leakViaAlignedNew()
{
    AlignedBlock* p = new AlignedBlock;  // intentional leak (channel A, aligned)
    gSink = p;
}

void leakViaMalloc(int depth)
{
    if (depth > 0) {
        leakViaMalloc(depth - 1);
        return;
    }
    void* p = std::malloc(64);           // intentional leak (channel B)
    gSink = p;
}

void leakViaCalloc()
{
    void* p = std::calloc(4, 8);         // intentional leak (channel B)
    gSink = p;
}

void leakViaRealloc()
{
    void* p = std::malloc(16);
    gSink = p;
    p = std::realloc(p, 1024);           // intentional leak (channel B, realloc)
    gSink = p;
}

// Clean worker: allocates and frees everything (must not appear in report).
void cleanWork()
{
    for (int i = 0; i < 50; ++i) {
        std::vector<int> v(16, i);       // C++ allocations, freed here
        int* p = new int(i);
        delete p;
        void* m = std::malloc(32);
        std::free(m);
    }
}

void worker(int id)
{
    cleanWork();
    if (id % 2 == 0) {
        leakViaNew(2);                   // deeper stack on even workers
        leakViaMalloc(1);
    } else {
        leakViaNewNothrow();
        leakViaCalloc();
    }
    leakViaAlignedNew();                 // every worker leaks one aligned block
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // The detector singleton starts recording on its first use; the global
    // operator new/delete overloads are active from process start.
    {
        // Scope the thread container: destroyed before dumpReport() so
        // live-but-valid containers are not reported as leaks.
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back(worker, i);
        for (auto& t : threads)
            t.join();
    }

    // Main-thread leaks with different depths.
    leakViaNew(3);
    leakViaMalloc(2);
    leakViaRealloc();

    MemLeakDetector::instance().dump();
    return 0;
}
