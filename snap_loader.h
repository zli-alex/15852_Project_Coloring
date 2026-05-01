#pragma once

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "graph_types.h"

// ---------------------------------------------------------------------------
// load_snap_edges
//
// Reads a SNAP-format undirected edge list:
//   - Lines beginning with '#' are skipped (comment / header).
//   - Each data line has two whitespace-separated integers  u  v.
//   - Self-loops are discarded.
//   - Duplicate and reversed edges are removed (each pair kept once with u<v).
//
// n_out is set to  max_node_id + 1  (covers all observed vertex IDs).
// Returns the de-duplicated, directed-once edge sequence.
// ---------------------------------------------------------------------------
inline edges load_snap_edges(const std::string& path, long& n_out) {
    std::ifstream fin(path);
    if (!fin.is_open()) {
        std::cerr << "[snap_loader] Cannot open file: " << path << "\n";
        n_out = 0;
        return {};
    }

    // Read all non-comment lines into a raw edge list.
    std::vector<std::pair<vertex, vertex>> raw;
    raw.reserve(1 << 20);
    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        vertex u, v;
        if (!(ss >> u >> v)) continue;
        if (u == v) continue;           // skip self-loops
        if (u > v) std::swap(u, v);     // canonical form: u < v
        raw.emplace_back(u, v);
    }
    fin.close();

    if (raw.empty()) {
        std::cerr << "[snap_loader] No edges found in " << path << "\n";
        n_out = 0;
        return {};
    }

    // Build parlay sequence and sort+dedup.
    auto E = parlay::tabulate(raw.size(), [&](size_t i) { return raw[i]; });

    // Sort by (u,v).
    parlay::sort_inplace(E, [](const edge& a, const edge& b) {
        return a < b;
    });

    // Remove duplicates.
    auto keep = parlay::tabulate(E.size(), [&](size_t i) -> bool {
        return i == 0 || E[i] != E[i - 1];
    });
    E = parlay::pack(E, keep);

    // Infer n as max vertex id + 1.
    vertex max_v = parlay::reduce(
        parlay::map(E, [](const edge& e) { return std::max(e.first, e.second); }),
        parlay::maximum<vertex>());
    n_out = (long)max_v + 1;

    return E;
}
