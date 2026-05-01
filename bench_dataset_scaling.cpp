// bench_dataset_scaling.cpp
//
// Dataset-size scaling benchmark for both algorithms.
//
// Two modes (selected by --mode <mode>):
//
//   vertex   Vary graph size (n) using RMAT:
//            n ∈ {2^12, 2^13, …, 2^20}, 20n edges, single full-graph insert.
//
//   batch    Fix RMAT 2^18, vary the dynamic batch size:
//            batches cover {1%, 5%, 10%, 25%, 50%, 100%} of all edges,
//            applied sequentially until the full graph is inserted.
//
//   snap     Load every SNAP dataset in --snap-dir and run a single
//            full-graph insert for both algorithms.
//
// Any combination of modes can be requested (e.g. --mode vertex --mode snap).
//
// Output format (CSV):
//   mode,algo,dataset,n,m,Delta,batch_edges,run,time_s
//
// Usage:
//   ./bench_dataset_scaling [--mode vertex|batch|snap]... [--snap-dir <dir>]
//                           [--runs R] [--header] [--heavy]
//
//   --mode      Which benchmark mode(s) to run (can repeat; default: all three)
//   --snap-dir  Path to SNAP data directory (default: ./data)
//   --runs R    Timed repetitions per data point (default: 3)
//   --header    Print CSV header and exit
//   --heavy     Include com-youtube in SNAP tests

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color_dplus1.h"
#include "dynamic_graph_color_cbyd.h"
#include "helper/graph_utils.h"
#include "snap_loader.h"

using utils = graph_utils<vertex>;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

static edges slice(const edges& E, long s, long e) {
    return parlay::tabulate(e - s, [&](long i) { return E[s + i]; });
}

// Time the full-insert workload for a single (algo, graph) combination.
// Returns wall-clock seconds.
static double time_dplus1_insert(long n, int Delta, const edges& E) {
    DynamicGraphColorDplus1 dgc(n, Delta);
    parlay::internal::timer t("", false);
    t.start();
    dgc.add_edge_batch(E);
    return t.stop();
}

static double time_cbyd_insert(long n, int Delta, const edges& E, int c = 2) {
    DynamicGraphColorCbyD dgc(n, Delta, c);
    parlay::internal::timer t("", false);
    t.start();
    dgc.add_edge_batch(E);
    return t.stop();
}

// Print one CSV data row.
static void emit(const std::string& mode,
                 const std::string& algo,
                 const std::string& dataset,
                 long n, long m, int Delta,
                 long batch_edges,
                 int run,
                 double time_s) {
    std::cout << mode << "," << algo << "," << dataset
              << "," << n << "," << m << "," << Delta
              << "," << batch_edges << "," << run
              << "," << time_s << "\n";
}

// -------------------------------------------------------------------------
// Mode: vertex — scale n with RMAT
// -------------------------------------------------------------------------
static void bench_vertex(int runs) {
    std::cerr << "[bench_dataset_scaling] mode=vertex\n";

    for (int logn = 12; logn <= 20; logn++) {
        long n0 = 1L << logn;
        auto G = utils::rmat_symmetric_graph(n0, 20*n0);
        long n = (long)G.size();

        int Delta = (int)parlay::reduce(
            parlay::map(G, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        auto all_dir = utils::to_edges(G);
        auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
        long m = (long)E.size();

        std::string ds = "rmat_2^" + std::to_string(logn);
        std::cerr << "  " << ds << " n=" << n << " m=" << m
                  << " Delta=" << Delta << "\n";

        for (int r = 0; r < runs; r++) {
            emit("vertex", "dplus1", ds, n, m, Delta, m, r,
                 time_dplus1_insert(n, Delta, E));
            emit("vertex", "cbyd_c2", ds, n, m, Delta, m, r,
                 time_cbyd_insert(n, Delta, E, 2));
        }
    }
}

// -------------------------------------------------------------------------
// Mode: batch — scale batch size on RMAT 2^18
// -------------------------------------------------------------------------
static void bench_batch(int runs) {
    std::cerr << "[bench_dataset_scaling] mode=batch\n";

    long n0 = 1L << 18;
    auto G = utils::rmat_symmetric_graph(n0, 20*n0);
    long n = (long)G.size();

    int Delta = (int)parlay::reduce(
        parlay::map(G, [](const auto& nb) { return nb.size(); }),
        parlay::maximum<size_t>());

    auto all_dir = utils::to_edges(G);
    auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
    long m = (long)E.size();

    std::string ds = "rmat_2^18";

    // Batch fractions: 1%, 5%, 10%, 25%, 50%, 100%
    const std::vector<double> fracs = {0.01, 0.05, 0.10, 0.25, 0.50, 1.00};

    for (double frac : fracs) {
        long batch_edges = std::max(1L, (long)(m * frac));
        // Number of batches needed to cover all edges
        long num_batches = (m + batch_edges - 1) / batch_edges;

        std::cerr << "  frac=" << frac << " batch_edges=" << batch_edges
                  << " num_batches=" << num_batches << "\n";

        for (int r = 0; r < runs; r++) {
            // dplus1
            {
                DynamicGraphColorDplus1 dgc(n, Delta);
                parlay::internal::timer t("", false);
                t.start();
                for (long b = 0; b < num_batches; b++) {
                    long lo = b * batch_edges;
                    long hi = std::min(lo + batch_edges, m);
                    dgc.add_edge_batch(slice(E, lo, hi));
                }
                double sec = t.stop();
                emit("batch", "dplus1", ds, n, m, Delta, batch_edges, r, sec);
            }
            // cbyd c=2
            {
                DynamicGraphColorCbyD dgc(n, Delta, 2);
                parlay::internal::timer t("", false);
                t.start();
                for (long b = 0; b < num_batches; b++) {
                    long lo = b * batch_edges;
                    long hi = std::min(lo + batch_edges, m);
                    dgc.add_edge_batch(slice(E, lo, hi));
                }
                double sec = t.stop();
                emit("batch", "cbyd_c2", ds, n, m, Delta, batch_edges, r, sec);
            }
        }
    }
}

// -------------------------------------------------------------------------
// Mode: snap — load SNAP datasets
// -------------------------------------------------------------------------
static void bench_snap(const std::string& snap_dir, bool heavy, int runs) {
    std::cerr << "[bench_dataset_scaling] mode=snap  dir=" << snap_dir << "\n";

    struct DS { std::string file; std::string name; };
    std::vector<DS> datasets = {
        { snap_dir + "/email-Enron.txt",       "email-Enron"  },
        { snap_dir + "/com-dblp.ungraph.txt",  "com-dblp"     },
        { snap_dir + "/com-amazon.ungraph.txt","com-amazon"   },
        { snap_dir + "/roadNet-CA.txt",        "roadNet-CA"   },
    };
    if (heavy)
        datasets.push_back({ snap_dir + "/com-youtube.ungraph.txt", "com-youtube" });

    for (auto& ds : datasets) {
        long n = 0;
        auto E = load_snap_edges(ds.file, n);
        if (E.empty()) {
            std::cerr << "  Skipping " << ds.name << " (load failed)\n";
            continue;
        }
        long m = (long)E.size();

        auto G = utils::symmetrize(E, n);
        int Delta = (int)parlay::reduce(
            parlay::map(G, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        std::cerr << "  " << ds.name << " n=" << n << " m=" << m
                  << " Delta=" << Delta << "\n";

        for (int r = 0; r < runs; r++) {
            emit("snap", "dplus1", ds.name, n, m, Delta, m, r,
                 time_dplus1_insert(n, Delta, E));
            emit("snap", "cbyd_c2", ds.name, n, m, Delta, m, r,
                 time_cbyd_insert(n, Delta, E, 2));
        }
    }
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::set<std::string> modes;
    std::string snap_dir = "data";
    int runs  = 3;
    bool header_only = false;
    bool heavy = false;

    for (int i = 1; i < argc; i++) {
        std::string a(argv[i]);
        if (a == "--mode"     && i+1 < argc) modes.insert(argv[++i]);
        if (a == "--snap-dir" && i+1 < argc) snap_dir = argv[++i];
        if (a == "--runs"     && i+1 < argc) runs = std::stoi(argv[++i]);
        if (a == "--header")   header_only = true;
        if (a == "--heavy")    heavy = true;
    }

    if (header_only) {
        std::cout << "mode,algo,dataset,n,m,Delta,batch_edges,run,time_s\n";
        return 0;
    }

    // Default: run all modes
    if (modes.empty()) { modes.insert("vertex"); modes.insert("batch"); modes.insert("snap"); }

    std::cerr << "[bench_dataset_scaling] workers=" << parlay::num_workers()
              << "  runs=" << runs << "\n";

    if (modes.count("vertex")) bench_vertex(runs);
    if (modes.count("batch"))  bench_batch(runs);
    if (modes.count("snap"))   bench_snap(snap_dir, heavy, runs);

    return 0;
}
