// test_correctness_dplus1.cpp
//
// Comprehensive correctness test suite for DynamicGraphColorDplus1.
//
// Tests:
//   A. Small hand-verifiable graphs (path, star, K5, bipartite K3,3)
//   B. RMAT synthetic graphs at n = 2^12, 2^14, 2^16, 2^18
//      - Single-batch, 5-batch, 10-batch, 20-batch insertion
//   C. Interleaved insert/delete workload on RMAT 2^16
//   D. Real-world SNAP datasets (email-Enron, com-dblp; com-youtube optional)
//
// Usage:
//   ./test_correctness_dplus1 [--snap-dir <dir>] [--heavy]
//
//   --snap-dir  Path to SNAP data directory (default: ./data)
//   --heavy     Also run com-youtube (large; ~1.1 M nodes)

#include <iostream>
#include <string>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color_dplus1.h"
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

// Check: each vertex colored in [0, Delta], no adjacent pair same color.
static bool check_coloring(const graph& G,
                            const parlay::sequence<int>& colors,
                            int Delta) {
    long n = (long)G.size();
    auto ok = parlay::tabulate(n, [&](long u) -> bool {
        if (colors[u] < 0 || colors[u] > Delta) return false;
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
// A. Small hand-verifiable graphs
// -------------------------------------------------------------------------
static void test_small_graphs() {
    std::cout << "\n--- A. Small hand-verifiable graphs ---\n";

    // Path P6
    {
        int n = 6, D = 2;
        edges e = {{0,1},{1,2},{2,3},{3,4},{4,5}};
        auto G = utils::symmetrize(e, n);
        DynamicGraphColorDplus1 dgc(n, D);
        dgc.add_edge_batch(e);
        report(check_coloring(G, dgc.get_coloring(), D), "Path P6");
    }
    // Star S5 (center 0, leaves 1-4)
    {
        int n = 5, D = 4;
        edges e = {{0,1},{0,2},{0,3},{0,4}};
        auto G = utils::symmetrize(e, n);
        DynamicGraphColorDplus1 dgc(n, D);
        dgc.add_edge_batch(e);
        report(check_coloring(G, dgc.get_coloring(), D), "Star S5");
    }
    // Complete graph K5
    {
        int n = 5, D = 4;
        edges e;
        for (int u = 0; u < n; u++)
            for (int v = u+1; v < n; v++)
                e.push_back({u, v});
        auto G = utils::symmetrize(e, n);
        DynamicGraphColorDplus1 dgc(n, D);
        dgc.add_edge_batch(e);
        report(check_coloring(G, dgc.get_coloring(), D), "Complete K5");
    }
    // Bipartite K3,3
    {
        int n = 6, D = 3;
        edges e = {{0,3},{0,4},{0,5},{1,3},{1,4},{1,5},{2,3},{2,4},{2,5}};
        auto G = utils::symmetrize(e, n);
        DynamicGraphColorDplus1 dgc(n, D);
        dgc.add_edge_batch(e);
        report(check_coloring(G, dgc.get_coloring(), D), "Bipartite K3,3");
    }
    // Cycle C8 built edge-by-edge
    {
        int n = 8, D = 2;
        edges all = {{0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{6,7},{7,0}};
        auto G = utils::symmetrize(all, n);
        DynamicGraphColorDplus1 dgc(n, D);
        for (auto& ed : all) dgc.add_edge_batch(edges{ed});
        report(check_coloring(G, dgc.get_coloring(), D), "Cycle C8 (edge-by-edge)");
    }
    // Empty graph (no edges, n=10)
    {
        int n = 10, D = 1;
        DynamicGraphColorDplus1 dgc(n, D);
        dgc.add_edge_batch({});
        auto G = utils::symmetrize({}, n);
        report(check_coloring(G, dgc.get_coloring(), D), "Empty graph n=10");
    }
}

// -------------------------------------------------------------------------
// B. RMAT graphs: multi-batch insertion at multiple scales
// -------------------------------------------------------------------------
static void test_rmat_multibatch() {
    std::cout << "\n--- B. RMAT multi-batch insertion ---\n";

    const long sizes[]     = {1L<<12, 1L<<14, 1L<<16, 1L<<18};
    const int  batch_cnts[] = {1, 5, 10, 20};

    for (long n0 : sizes) {
        auto G_full = utils::rmat_symmetric_graph(n0, 20*n0);
        long n = (long)G_full.size();

        int Delta = (int)parlay::reduce(
            parlay::map(G_full, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        auto all_dir = utils::to_edges(G_full);
        auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
        long m = (long)E.size();

        for (int nb : batch_cnts) {
            DynamicGraphColorDplus1 dgc(n, Delta);

            long batch_size = (m + nb - 1) / nb;
            bool all_ok = true;

            for (int b = 0; b < nb; b++) {
                long lo = b * batch_size;
                long hi = std::min(lo + batch_size, m);
                dgc.add_edge_batch(slice(E, lo, hi));
            }

            // Final check against full graph
            bool ok = check_coloring(G_full, dgc.get_coloring(), Delta);
            if (!ok) all_ok = false;

            std::string label = "RMAT n=2^" + std::to_string((int)std::log2(n))
                              + " batches=" + std::to_string(nb);
            report(all_ok, label);
        }
    }
}

// -------------------------------------------------------------------------
// C. Interleaved insert/delete workload
// -------------------------------------------------------------------------
static void test_insert_delete() {
    std::cout << "\n--- C. Interleaved insert / delete (RMAT 2^16) ---\n";

    long n0 = 1L << 16;
    auto G_full = utils::rmat_symmetric_graph(n0, 20*n0);
    long n = (long)G_full.size();

    int Delta = (int)parlay::reduce(
        parlay::map(G_full, [](const auto& nb) { return nb.size(); }),
        parlay::maximum<size_t>());

    auto all_dir = utils::to_edges(G_full);
    auto E = parlay::filter(all_dir, [](edge e) { return e.first < e.second; });
    long m = (long)E.size();

    // Phase 1: insert all
    DynamicGraphColorDplus1 dgc(n, Delta);
    dgc.add_edge_batch(E);
    auto G1 = G_full;
    report(check_coloring(G1, dgc.get_coloring(), Delta), "Insert all (100%)");

    // Phase 2: delete first 25%
    long del_cnt = m / 4;
    auto E_del  = slice(E, 0, del_cnt);
    auto E_keep = slice(E, del_cnt, m);
    dgc.delete_edge_batch(E_del);
    auto G2 = utils::symmetrize(E_keep, n);
    report(check_coloring(G2, dgc.get_coloring(), Delta), "After delete 25%");

    // Phase 3: re-insert the deleted edges
    dgc.add_edge_batch(E_del);
    report(check_coloring(G1, dgc.get_coloring(), Delta), "Re-insert deleted 25%");

    // Phase 4: delete 50%, check
    long del2 = m / 2;
    auto E_del2  = slice(E, 0, del2);
    auto E_keep2 = slice(E, del2, m);
    DynamicGraphColorDplus1 dgc2(n, Delta);
    dgc2.add_edge_batch(E);
    dgc2.delete_edge_batch(E_del2);
    auto G4 = utils::symmetrize(E_keep2, n);
    report(check_coloring(G4, dgc2.get_coloring(), Delta), "Delete 50% from fresh");

    // Phase 5: many small delete batches (10 batches of 5% each = 50%)
    {
        DynamicGraphColorDplus1 dgc3(n, Delta);
        dgc3.add_edge_batch(E);
        int del_rounds = 10;
        long chunk = m / (del_rounds * 2);  // 5% each
        for (int r = 0; r < del_rounds; r++) {
            long lo = (long)r * chunk;
            long hi = lo + chunk;
            dgc3.delete_edge_batch(slice(E, lo, hi));
        }
        long del_total = del_rounds * chunk;
        auto E_rem = slice(E, del_total, m);
        auto G5 = utils::symmetrize(E_rem, n);
        report(check_coloring(G5, dgc3.get_coloring(), Delta),
               "10 sequential delete batches (5% each)");
    }
}

// -------------------------------------------------------------------------
// D. Real-world SNAP datasets
// -------------------------------------------------------------------------
static void test_snap(const std::string& snap_dir, bool heavy) {
    std::cout << "\n--- D. SNAP real-world datasets ---\n";

    struct Dataset { std::string file; std::string name; };
    std::vector<Dataset> datasets = {
        { snap_dir + "/email-Enron.txt",     "email-Enron"  },
        { snap_dir + "/com-dblp.ungraph.txt","com-dblp"     },
        { snap_dir + "/com-amazon.ungraph.txt","com-amazon"  },
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

        // Build the symmetrized adjacency list for checking.
        auto G = utils::symmetrize(E, n);

        int Delta = (int)parlay::reduce(
            parlay::map(G, [](const auto& nb) { return nb.size(); }),
            parlay::maximum<size_t>());

        std::cout << "  " << ds.name
                  << "  n=" << n << "  m=" << m
                  << "  Delta=" << Delta << "\n";

        // Single-batch insert
        {
            parlay::internal::timer t("", false); t.start();
            DynamicGraphColorDplus1 dgc(n, Delta);
            dgc.add_edge_batch(E);
            double sec = t.stop();
            bool ok = check_coloring(G, dgc.get_coloring(), Delta);
            report(ok, ds.name + " single-batch insert (" + std::to_string(sec).substr(0,5) + "s)");
        }

        // 10-batch insert, check after every batch
        {
            int nb = 10;
            long batch_size = (m + nb - 1) / nb;
            DynamicGraphColorDplus1 dgc(n, Delta);
            bool all_ok = true;
            for (int b = 0; b < nb; b++) {
                long lo = b * batch_size;
                long hi = std::min(lo + batch_size, m);
                auto Eb = slice(E, lo, hi);
                dgc.add_edge_batch(Eb);
                // Build subgraph for edges seen so far
                auto E_so_far = slice(E, 0, hi);
                auto Gsub = utils::symmetrize(E_so_far, n);
                if (!check_coloring(Gsub, dgc.get_coloring(), Delta)) {
                    all_ok = false;
                    break;
                }
            }
            report(all_ok, ds.name + " 10-batch insert (check after each)");
        }

        // Insert then delete 10%
        {
            DynamicGraphColorDplus1 dgc(n, Delta);
            dgc.add_edge_batch(E);
            long del_cnt = m / 10;
            dgc.delete_edge_batch(slice(E, 0, del_cnt));
            auto E_rem = slice(E, del_cnt, m);
            auto G_rem = utils::symmetrize(E_rem, n);
            bool ok = check_coloring(G_rem, dgc.get_coloring(), Delta);
            report(ok, ds.name + " insert-all then delete 10%");
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
    std::cout << "  DynamicGraphColorDplus1 — Correctness Test Suite\n";
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
