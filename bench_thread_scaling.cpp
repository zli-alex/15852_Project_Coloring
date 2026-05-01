// bench_thread_scaling.cpp
//
// Thread-count / speedup scaling benchmark for both algorithms.
//
// Workload: RMAT 2^18 graph, single full-graph insert batch, repeated 5 times.
// Timing is reported per-run and averaged.
//
// Designed to be called by run_benchmarks.sh in a loop:
//   for T in 1 2 4 8 16 32; do
//       PARLAY_NUM_THREADS=$T ./bench_thread_scaling >> results/thread_scaling.csv
//   done
//
// Output format (CSV, one row per algorithm per invocation):
//   algo,num_workers,n,m,run,time_s
//
// Usage:
//   ./bench_thread_scaling [--n N] [--runs R] [--header]
//
//   --n N       Number of vertices (default: 2^18 = 262144)
//   --runs R    Number of timed repetitions (default: 5)
//   --header    Print CSV header line and exit

#include <cmath>
#include <iostream>
#include <string>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color_dplus1.h"
#include "dynamic_graph_color_cbyd.h"
#include "helper/graph_utils.h"

// Use dplus1 types (both headers share the same typedefs, but we include both
// only for the struct names; redefining types would collide, so use dplus1's).
using utils = graph_utils<vertex>;

static edges slice(const edges& E, long s, long e) {
    return parlay::tabulate(e - s, [&](long i) { return E[s + i]; });
}

int main(int argc, char* argv[]) {
    long n0    = 1L << 18;
    int  runs  = 5;
    bool header_only = false;

    for (int i = 1; i < argc; i++) {
        std::string a(argv[i]);
        if (a == "--n"     && i+1 < argc) n0   = std::stol(argv[++i]);
        if (a == "--runs"  && i+1 < argc) runs = std::stoi(argv[++i]);
        if (a == "--header") header_only = true;
    }

    if (header_only) {
        std::cout << "algo,num_workers,n,m,run,time_s\n";
        return 0;
    }

    size_t T = parlay::num_workers();

    // ── Build graph once ────────────────────────────────────────────────
    auto G = utils::rmat_symmetric_graph(n0, 20*n0);
    long n = (long)G.size();

    int Delta = (int)parlay::reduce(
        parlay::map(G, [](const auto& nb) { return nb.size(); }),
        parlay::maximum<size_t>());

    auto all_dir = utils::to_edges(G);
    auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
    long m = (long)E.size();

    // ── dplus1 ──────────────────────────────────────────────────────────
    for (int r = 0; r < runs; r++) {
        DynamicGraphColorDplus1 dgc(n, Delta);
        parlay::internal::timer t("", false);
        t.start();
        dgc.add_edge_batch(E);
        double sec = t.stop();
        std::cout << "dplus1," << T << "," << n << "," << m
                  << "," << r << "," << sec << "\n";
    }

    // ── cbyd (c=2) ───────────────────────────────────────────────────────
    for (int r = 0; r < runs; r++) {
        DynamicGraphColorCbyD dgc(n, Delta, 2);
        parlay::internal::timer t("", false);
        t.start();
        dgc.add_edge_batch(E);
        double sec = t.stop();
        std::cout << "cbyd_c2," << T << "," << n << "," << m
                  << "," << r << "," << sec << "\n";
    }

    // ── Also time a 10-batch dynamic workload (insert 10 sequential batches) ──
    // This stresses the dynamic (incremental) code path more than single-batch.
    int nb = 10;
    long batch_size = (m + nb - 1) / nb;

    for (int r = 0; r < runs; r++) {
        DynamicGraphColorDplus1 dgc(n, Delta);
        parlay::internal::timer t("", false);
        t.start();
        for (int b = 0; b < nb; b++) {
            long lo = b * batch_size;
            long hi = std::min(lo + batch_size, m);
            dgc.add_edge_batch(slice(E, lo, hi));
        }
        double sec = t.stop();
        std::cout << "dplus1_10batch," << T << "," << n << "," << m
                  << "," << r << "," << sec << "\n";
    }

    for (int r = 0; r < runs; r++) {
        DynamicGraphColorCbyD dgc(n, Delta, 2);
        parlay::internal::timer t("", false);
        t.start();
        for (int b = 0; b < nb; b++) {
            long lo = b * batch_size;
            long hi = std::min(lo + batch_size, m);
            dgc.add_edge_batch(slice(E, lo, hi));
        }
        double sec = t.stop();
        std::cout << "cbyd_c2_10batch," << T << "," << n << "," << m
                  << "," << r << "," << sec << "\n";
    }

    return 0;
}
