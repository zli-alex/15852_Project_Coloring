#pragma once

#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/random.h>
#include <parlay/sequence.h>

#include "graph_types.h"

struct DynamicGraphColorCbyD {
    long n; // n vertices
    int Delta; // max degree
    int c; // multiplier
    int num_colors; // number of colors c*Delta

    graph adj; // adjacency list
    parlay::sequence<int> color; // color of each vertex
    parlay::sequence<long> timestamp; // last recolor time
    std::atomic<long> global_time;

    long last_recolor_rounds = 0; // diagonostic: should be log(n) whp

    DynamicGraphColorCbyD(long n, int Delta, int c=2)
        : n(n), Delta(Delta), c(c), num_colors(c*Delta),
          adj(n), color(n), timestamp(n, 0L), global_time(1L) {
        // initial color assignment
        parlay::random_generator gen(0);
        parlay::parallel_for(0, n, [&](long v) {
            auto rng = gen[(size_t)v];
            color[v] = (int)(rng() % (unsigned long long)num_colors);
        });
    }

    /**
     * @brief color sub graph
     * 
     * section 4.1
     *
     * @param H 
     */
    void color_blank_subgraph(parlay::sequence<vertex> H) {
        if (H.empty()) return;

        // mark
        parlay::sequence<uint8_t> in_H(n, 0);
        parlay::parallel_for(0, (long)H.size(), [&](long i) {
            in_H[H[i]] = 1;
        });

        // temp color
        parlay::sequence<int> temp_color(n, -1);

        parlay::random_generator gen((size_t)global_time.load());
        long rounds = 0;

        while (!H.empty()) {
            long size = (long) H.size();
            // 1. sample
            parlay::parallel_for(0, size, [&](long i) {
                vertex u = H[i];
                auto rng = gen[(size_t)(rounds * n + u)];

                //  used color over colored neighbors
                std::vector<bool> used(num_colors, false);
                for (vertex w: adj[u]) {
                    int cw = color[w];
                    if (cw >= 0) used[cw] = true;
                }

                // available color
                std::vector<int> available_colors;
                available_colors.reserve(num_colors);
                for (int c = 0; c < num_colors; c++) {
                    if (!used[c]) available_colors.push_back(c);
                }
                temp_color[u] = available_colors[rng() % available_colors.size()];
            }, /*grain_size=*/1);

            // 2. detect conflicts
            auto conflict_flags = parlay::tabulate(size, [&](long i) -> bool {
                vertex u = H[i];
                int tu = temp_color[u];
                for (vertex w : adj[u]) {
                  if (in_H[w] && temp_color[w] == tu) return true;
                }
                return false;
              });

            // 3. commit non-conflicts
            auto committed = parlay::pack(H, parlay::map(conflict_flags, [](bool b){ return !b; }));
            auto conflicted = parlay::pack(H, conflict_flags);

            // 4. update states on committed vertices
            long t_base = global_time.fetch_add((long)committed.size());
            parlay::parallel_for(0, (long)committed.size(), [&](long i) {
                vertex u = committed[i];
                color[u]     = temp_color[u];
                timestamp[u] = t_base + i;
                in_H[u]      = 0;
            });

            // 5. clear conflicted vertices color
            parlay::parallel_for(0, (long)conflicted.size(), [&](long i) {
                temp_color[conflicted[i]] = -1;
              });
        
            H = std::move(conflicted);
            rounds++;
        }
        last_recolor_rounds = rounds;
    }

    /**
     * @brief Add edge
     *
     * section 2.1.2
     * 
     * @param batch 
     */
    void add_edge_batch(const edges& batch) {
        if (batch.empty()) return;

        // 1. update adjacency list
        auto directed = parlay::flatten(
            parlay::map(batch, [](edge e) -> parlay::sequence<edge> {
              return {{e.first, e.second}, {e.second, e.first}};
        }));
        auto new_neighbors = parlay::group_by_index(directed, n);
        parlay::parallel_for(0, n, [&](long v) {
            if (!new_neighbors[v].empty())
                adj[v] = parlay::append(adj[v], new_neighbors[v]);
        });

        // 2. find conflicts
        auto candidates = parlay::map(batch, [&](edge e) -> vertex {
            auto [u, v] = e;
            if (color[u] >= 0 && color[u] == color[v]) {
              return (timestamp[u] >= timestamp[v]) ? u : v;
            }
            return -1;
        });
        auto V_blank_raw = parlay::filter(candidates, [](vertex v){ return v != -1; });
        auto V_blank = sort_dedup_vertices(std::move(V_blank_raw));

        // 3. recolor V_blank
        parlay::parallel_for(0, (long)V_blank.size(), [&](long i) {
            color[V_blank[i]] = -1;
        });
        color_blank_subgraph(std::move(V_blank));
    }

    /**
     * @brief Delete edge
     *
     * no conflicts detected: just delete edge
     * 
     * @param batch 
     */
    void delete_edge_batch(const edges& batch) {
        if (batch.empty()) return;

        // identify affected vertices
        auto affected_raw = parlay::flatten(
            parlay::map(batch, [](edge e) -> parlay::sequence<vertex> {
                return {e.first, e.second};
            }));
        auto affected = sort_dedup_vertices(std::move(affected_raw));
        
        // build sorted edge keys
        auto del_keys = parlay::sort(
            parlay::map(batch, [](edge e) {
                return std::make_pair(std::min(e.first, e.second), std::max(e.first, e.second));
        }));
        parlay::parallel_for(0, (long)affected.size(), [&](long i) {
            vertex v = affected[i];
            adj[v] = parlay::filter(adj[v], [&](vertex w) {
                auto key = std::make_pair(std::min((vertex)v, w), std::max((vertex)v, w));
                return !std::binary_search(del_keys.begin(), del_keys.end(), key);
            });
        });
    }

    const parlay::sequence<int>& get_colors() const { return color; }
    const parlay::sequence<int>& get_coloring() const { return color; }
};
