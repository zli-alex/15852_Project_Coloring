// bench_c_scaling.cpp
//
// Benchmark the effect of the palette-size multiplier c on DynamicGraphColorCbyD.
//
// Workload: RMAT 2^18 graph, single full-graph insert batch.
// Sweeps c ∈ {2, 3, 4, 6, 8}.
// Each (c, run) combination is timed independently to avoid warm-up skew.
//
// Output format (CSV):
//   c,num_colors,n,m,Delta,run,time_s
//
// Usage:
//   ./bench_c_scaling [--n N] [--runs R] [--header]
//
//   --n N       Number of vertices (default: 2^18 = 262144)
//   --runs R    Number of timed repetitions per c value (default: 5)
//   --header    Print CSV header line and exit

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color_cbyd.h"
#include "helper/graph_utils.h"

using utils = graph_utils<vertex>;

static edges slice(const edges& E, long s, long e) {
    return parlay::tabulate(e - s, [&](long i) { return E[s + i]; });
}

int main(int argc, char* argv[]) {
    long n0   = 1L << 18;
    int  runs = 5;
    bool header_only = false;

    for (int i = 1; i < argc; i++) {
        std::string a(argv[i]);
        if (a == "--n"     && i+1 < argc) n0   = std::stol(argv[++i]);
        if (a == "--runs"  && i+1 < argc) runs = std::stoi(argv[++i]);
        if (a == "--header") header_only = true;
    }

    if (header_only) {
        std::cout << "c,num_colors,n,m,Delta,run,time_s\n";
        return 0;
    }

    size_t num_workers = parlay::num_workers();
    std::cerr << "[bench_c_scaling] workers=" << num_workers
              << "  n0=" << n0 << "  runs=" << runs << "\n";

    // ── Build graph once ────────────────────────────────────────────────
    auto G = utils::rmat_symmetric_graph(n0, 20*n0);
    long n = (long)G.size();

    int Delta = (int)parlay::reduce(
        parlay::map(G, [](const auto& nb) { return nb.size(); }),
        parlay::maximum<size_t>());

    auto all_dir = utils::to_edges(G);
    auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
    long m = (long)E.size();

    std::cerr << "[bench_c_scaling] n=" << n << "  m=" << m
              << "  Delta=" << Delta << "\n";

    // ── c sweep ─────────────────────────────────────────────────────────
    const std::vector<int> c_vals = {2, 3, 4, 6, 8};

    for (int c : c_vals) {
        int num_colors = c * Delta;
        for (int r = 0; r < runs; r++) {
            DynamicGraphColorCbyD dgc(n, Delta, c);
            parlay::internal::timer t("", false);
            t.start();
            dgc.add_edge_batch(E);
            double sec = t.stop();
            std::cout << c << "," << num_colors << "," << n << ","
                      << m << "," << Delta << "," << r << "," << sec << "\n";
        }
    }

    // ── Also sweep c with a 10-batch dynamic workload ────────────────────
    // Demonstrates how c affects per-batch recoloring cost.
    int nb = 10;
    long batch_size = (m + nb - 1) / nb;

    std::cerr << "[bench_c_scaling] Running 10-batch sweep...\n";

    for (int c : c_vals) {
        int num_colors = c * Delta;
        for (int r = 0; r < runs; r++) {
            DynamicGraphColorCbyD dgc(n, Delta, c);
            parlay::internal::timer t("", false);
            t.start();
            for (int b = 0; b < nb; b++) {
                long lo = b * batch_size;
                long hi = std::min(lo + batch_size, m);
                dgc.add_edge_batch(slice(E, lo, hi));
            }
            double sec = t.stop();
            // prefix batch label with "10b_" in c column to distinguish
            std::cout << "10b_" << c << "," << num_colors << "," << n << ","
                      << m << "," << Delta << "," << r << "," << sec << "\n";
        }
    }

    return 0;
}
