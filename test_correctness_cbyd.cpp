// test_correctness_cbyd.cpp
//
// Comprehensive correctness test suite for DynamicGraphColorCbyD.
//
// Tests:
//   A. Small hand-verifiable graphs (path, star, K5, bipartite K3,3)
//      — run for c = 2, 3, 4 to exercise palette-width correctness
//   B. RMAT synthetic graphs at n = 2^12, 2^14, 2^16, 2^18
//      — single-batch, 5-batch, 10-batch, 20-batch insertion
//   C. Interleaved insert/delete workload on RMAT 2^16
//   D. Real-world SNAP datasets (email-Enron, com-dblp, com-amazon; com-youtube optional)
//
// Usage:
//   ./test_correctness_cbyd [--snap-dir <dir>] [--heavy]
//
//   --snap-dir  Path to SNAP data directory (default: ./data)
//   --heavy     Also run com-youtube (large; ~1.1 M nodes)

#include <iostream>
#include <string>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color_cbyd.h"
#include "helper/graph_utils.h"
#include "snap_loader.h"

using utils = graph_utils<vertex>;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;

static void report(bool ok, const std::string& label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << "\n";
    if (ok) ++g_pass; else ++g_fail;
}

// Check: each vertex colored in [0, num_colors), no adjacent pair same color.
// num_colors = c * Delta for cbyd.
static bool check_coloring(const graph& G,
                            const parlay::sequence<int>& colors,
                            int num_colors) {
    long n = (long)G.size();
    auto ok = parlay::tabulate(n, [&](long u) -> bool {
        if (colors[u] < 0 || colors[u] >= num_colors) return false;
        for (vertex w : G[u])
            if (colors[u] == colors[w]) return false;
        return true;
    });
    return parlay::count_if(ok, [](bool b) { return !b; }) == 0;
}

static edges slice(const edges& E, long s, long e) {
    return parlay::tabulate(e - s, [&](long i) { return E[s + i]; });
}

// -------------------------------------------------------------------------
// A. Small hand-verifiable graphs — varied c
// -------------------------------------------------------------------------
static void test_small_graphs() {
    std::cout << "\n--- A. Small hand-verifiable graphs (c=2,3,4) ---\n";

    for (int c : {2, 3, 4}) {
        // Path P6
        {
            int n = 6, D = 2;
            edges e = {{0,1},{1,2},{2,3},{3,4},{4,5}};
            auto G = utils::symmetrize(e, n);
            DynamicGraphColorCbyD dgc(n, D, c);
            dgc.add_edge_batch(e);
            report(check_coloring(G, dgc.get_coloring(), c*D),
                   "Path P6 c=" + std::to_string(c));
        }
        // Star S5
        {
            int n = 5, D = 4;
            edges e = {{0,1},{0,2},{0,3},{0,4}};
            auto G = utils::symmetrize(e, n);
            DynamicGraphColorCbyD dgc(n, D, c);
            dgc.add_edge_batch(e);
            report(check_coloring(G, dgc.get_coloring(), c*D),
                   "Star S5 c=" + std::to_string(c));
        }
        // Complete K5
        {
            int n = 5, D = 4;
            edges e;
            for (int u = 0; u < n; u++)
                for (int v = u+1; v < n; v++)
                    e.push_back({u, v});
            auto G = utils::symmetrize(e, n);
            DynamicGraphColorCbyD dgc(n, D, c);
            dgc.add_edge_batch(e);
            report(check_coloring(G, dgc.get_coloring(), c*D),
                   "Complete K5 c=" + std::to_string(c));
        }
        // Bipartite K3,3
        {
            int n = 6, D = 3;
            edges e = {{0,3},{0,4},{0,5},{1,3},{1,4},{1,5},{2,3},{2,4},{2,5}};
            auto G = utils::symmetrize(e, n);
            DynamicGraphColorCbyD dgc(n, D, c);
            dgc.add_edge_batch(e);
            report(check_coloring(G, dgc.get_coloring(), c*D),
                   "Bipartite K3,3 c=" + std::to_string(c));
        }
        // Cycle C8 edge-by-edge
        {
            int n = 8, D = 2;
            edges all = {{0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{6,7},{7,0}};
            auto G = utils::symmetrize(all, n);
            DynamicGraphColorCbyD dgc(n, D, c);
            for (auto& ed : all) dgc.add_edge_batch(edges{ed});
            report(check_coloring(G, dgc.get_coloring(), c*D),
                   "Cycle C8 edge-by-edge c=" + std::to_string(c));
        }
    }

    // Empty graph (no edges, n=10), c=2
    {
        int n = 10, D = 1, c = 2;
        DynamicGraphColorCbyD dgc(n, D, c);
        dgc.add_edge_batch({});
        auto G = utils::symmetrize({}, n);
        report(check_coloring(G, dgc.get_coloring(), c*D), "Empty graph n=10 c=2");
    }
}

// -------------------------------------------------------------------------
// B. RMAT graphs: multi-batch insertion at multiple scales
// -------------------------------------------------------------------------
static void test_rmat_multibatch() {
    std::cout << "\n--- B. RMAT multi-batch insertion ---\n";

    const long sizes[]      = {1L<<12, 1L<<14, 1L<<16, 1L<<18};
    const int  batch_cnts[] = {1, 5, 10, 20};
    const int  c_vals[]     = {2, 4};   // two c values for RMAT tests

    for (long n0 : sizes) {
        auto G_full = utils::rmat_symmetric_graph(n0, 20*n0);
        long n = (long)G_full.size();

        int Delta = (int)parlay::reduce(
            parlay::map(G_full, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        auto all_dir = utils::to_edges(G_full);
        auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
        long m = (long)E.size();

        for (int c : c_vals) {
            for (int nb : batch_cnts) {
                DynamicGraphColorCbyD dgc(n, Delta, c);
                long batch_size = (m + nb - 1) / nb;

                for (int b = 0; b < nb; b++) {
                    long lo = b * batch_size;
                    long hi = std::min(lo + batch_size, m);
                    dgc.add_edge_batch(slice(E, lo, hi));
                }

                bool ok = check_coloring(G_full, dgc.get_coloring(), c * Delta);
                std::string label = "RMAT n=2^" + std::to_string((int)std::log2(n))
                                  + " c=" + std::to_string(c)
                                  + " batches=" + std::to_string(nb);
                report(ok, label);
            }
        }
    }
}

// -------------------------------------------------------------------------
// C. Interleaved insert/delete workload
// -------------------------------------------------------------------------
static void test_insert_delete() {
    std::cout << "\n--- C. Interleaved insert / delete (RMAT 2^16, c=2) ---\n";

    long n0 = 1L << 16;
    auto G_full = utils::rmat_symmetric_graph(n0, 20*n0);
    long n = (long)G_full.size();

    int Delta = (int)parlay::reduce(
        parlay::map(G_full, [](const auto& nb) { return nb.size(); }),
        parlay::maximum<size_t>());

    auto all_dir = utils::to_edges(G_full);
    auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
    long m = (long)E.size();

    int c = 2;

    // Phase 1: insert all
    DynamicGraphColorCbyD dgc(n, Delta, c);
    dgc.add_edge_batch(E);
    report(check_coloring(G_full, dgc.get_coloring(), c*Delta), "Insert all (100%)");

    // Phase 2: delete first 25%
    long del_cnt = m / 4;
    dgc.delete_edge_batch(slice(E, 0, del_cnt));
    auto G2 = utils::symmetrize(slice(E, del_cnt, m), n);
    report(check_coloring(G2, dgc.get_coloring(), c*Delta), "After delete 25%");

    // Phase 3: re-insert deleted edges
    dgc.add_edge_batch(slice(E, 0, del_cnt));
    report(check_coloring(G_full, dgc.get_coloring(), c*Delta), "Re-insert deleted 25%");

    // Phase 4: delete 50%, fresh instance
    {
        long del2 = m / 2;
        DynamicGraphColorCbyD dgc2(n, Delta, c);
        dgc2.add_edge_batch(E);
        dgc2.delete_edge_batch(slice(E, 0, del2));
        auto G4 = utils::symmetrize(slice(E, del2, m), n);
        report(check_coloring(G4, dgc2.get_coloring(), c*Delta),
               "Delete 50% from fresh");
    }

    // Phase 5: 10 sequential small delete batches (~5% each)
    {
        DynamicGraphColorCbyD dgc3(n, Delta, c);
        dgc3.add_edge_batch(E);
        int del_rounds = 10;
        long chunk = m / (del_rounds * 2);
        for (int r = 0; r < del_rounds; r++) {
            long lo = (long)r * chunk;
            dgc3.delete_edge_batch(slice(E, lo, lo + chunk));
        }
        long del_total = del_rounds * chunk;
        auto G5 = utils::symmetrize(slice(E, del_total, m), n);
        report(check_coloring(G5, dgc3.get_coloring(), c*Delta),
               "10 sequential delete batches (~5% each)");
    }
}

// -------------------------------------------------------------------------
// D. Real-world SNAP datasets
// -------------------------------------------------------------------------
static void test_snap(const std::string& snap_dir, bool heavy) {
    std::cout << "\n--- D. SNAP real-world datasets (c=2,4) ---\n";

    struct Dataset { std::string file; std::string name; };
    std::vector<Dataset> datasets = {
        { snap_dir + "/email-Enron.txt",      "email-Enron"  },
        { snap_dir + "/com-dblp.ungraph.txt", "com-dblp"     },
        { snap_dir + "/com-amazon.ungraph.txt","com-amazon"   },
    };
    if (heavy)
        datasets.push_back({ snap_dir + "/com-youtube.ungraph.txt", "com-youtube" });

    for (auto& ds : datasets) {
        long n = 0;
        auto E = load_snap_edges(ds.file, n);
        if (E.empty()) {
            report(false, ds.name + " (load failed)");
            continue;
        }
        long m = (long)E.size();
        auto G = utils::symmetrize(E, n);
        int Delta = (int)parlay::reduce(
            parlay::map(G, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        std::cout << "  " << ds.name
                  << "  n=" << n << "  m=" << m
                  << "  Delta=" << Delta << "\n";

        for (int c : {2, 4}) {
            // Single-batch insert
            {
                parlay::internal::timer t("", false); t.start();
                DynamicGraphColorCbyD dgc(n, Delta, c);
                dgc.add_edge_batch(E);
                double sec = t.stop();
                bool ok = check_coloring(G, dgc.get_coloring(), c * Delta);
                report(ok, ds.name + " single-batch c=" + std::to_string(c)
                           + " (" + std::to_string(sec).substr(0,5) + "s)");
            }

            // 10-batch insert, check after every batch
            {
                int nb = 10;
                long batch_size = (m + nb - 1) / nb;
                DynamicGraphColorCbyD dgc(n, Delta, c);
                bool all_ok = true;
                for (int b = 0; b < nb; b++) {
                    long lo = b * batch_size;
                    long hi = std::min(lo + batch_size, m);
                    dgc.add_edge_batch(slice(E, lo, hi));
                    auto Gsub = utils::symmetrize(slice(E, 0, hi), n);
                    if (!check_coloring(Gsub, dgc.get_coloring(), c * Delta)) {
                        all_ok = false; break;
                    }
                }
                report(all_ok, ds.name + " 10-batch c=" + std::to_string(c));
            }

            // Insert then delete 10%
            {
                DynamicGraphColorCbyD dgc(n, Delta, c);
                dgc.add_edge_batch(E);
                long del_cnt = m / 10;
                dgc.delete_edge_batch(slice(E, 0, del_cnt));
                auto G_rem = utils::symmetrize(slice(E, del_cnt, m), n);
                bool ok = check_coloring(G_rem, dgc.get_coloring(), c * Delta);
                report(ok, ds.name + " insert-all + delete 10% c=" + std::to_string(c));
            }
        }
    }
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string snap_dir = "data";
    bool heavy = false;

    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--snap-dir" && i + 1 < argc) snap_dir = argv[++i];
        else if (arg == "--heavy") heavy = true;
    }

    std::cout << "============================================================\n";
    std::cout << "  DynamicGraphColorCbyD — Correctness Test Suite\n";
    std::cout << "============================================================\n";

    test_small_graphs();
    test_rmat_multibatch();
    test_insert_delete();
    test_snap(snap_dir, heavy);

    std::cout << "\n============================================================\n";
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "============================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
