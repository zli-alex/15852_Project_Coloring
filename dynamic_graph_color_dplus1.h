#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include <random>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/random.h>
#include <parlay/parallel.h>

#include "graph_types.h"

/**
 * @brief Sample from geom distr for level assignment
 * 
 * @param rng 
 * @param max_level 
 * @return int 
 */
static int sample_geometric(parlay::random_generator& rng, int max_level) {
    for (int i = 0; i < max_level; i++) {
        if (rng() & 1ULL) return i;
    }
    return max_level;
}

struct DynamicGraphColorDplus1 {
    long n; // n vertices
    int Delta; // max degree
    int max_level;

    graph adj; // adjacency list
    parlay::sequence<int> level; // level of each vertex
    parlay::sequence<int> color; // color of each vertex
    parlay::sequence<long> timestamp; // last recolor time
    std::atomic<long> global_time;

    long last_recolor_rounds = 0; // diagonostic: should be log(n) whp

    /**
     * @brief Construction: paper 3.1
     * 
     * @param n 
     * @param Delta 
     */
    DynamicGraphColorDplus1(long n, int Delta)
        : n(n), Delta(Delta), max_level(log2(n)),
          adj(n), level(n), color(n), timestamp(n, 0L), global_time(1L) {
        auto ceil_log2 = [](int x) -> int {
            if (x <= 1) return 0;
            int r = 0;
            for (int i = x; i > 1; i >>= 1) r++;
            return r;
        };
        max_level = std::max(2, ceil_log2(Delta) + 1);

        parlay::random_generator gen(0);
        parlay::parallel_for(0, n, [&](long v) {
            // initial level and color assignment
            auto rng = gen[(size_t)v];
            level[v] = sample_geometric(rng, max_level);
            std::uniform_int_distribution<int> dis(0, Delta);
            color[v] = dis(rng);
        });

    }

    /**
     * @brief Sample a color for a vertex (algo 1)
     * 
     * vertex v: color[v] = -1 before sampling
     * palette: no lo/eq-neighbors with same color + hi-neighbors at most once before assignment
     * 
     * @param v 
     * @param rng 
     */
    void sample_from_palette(vertex v, parlay::random_generator& rng) {
        int l = level[v];
        const auto& neighbors = adj[v];

        // 1. collect colors used by lower/equal level neighbors
        std::vector<bool> used(Delta + 1, false);
        for (vertex w: neighbors) {
            if (level[w] <= l) {
                int cw = color[w];
                if (cw >= 0) used[cw] = true;
            }
        }
        std::vector<int> available_colors;
        available_colors.reserve(Delta + 1);
        for (int c = 0; c <= Delta; c++) {
            if (!used[c]) available_colors.push_back(c);
        }

        // 2. sample from available colors
        if (available_colors.empty()) {
            // no available colors, use random color
            std::uniform_int_distribution<int> dis(0, Delta);
            color[v] = dis(rng);
            return;
        }

        // 3. count higher level neighbors per free color
        std::fill(used.begin(), used.end(), false);
        std::vector<int> hi_count(Delta + 1, 0);
        for (vertex w: neighbors) {
            if (level[w] > l) {
                int cw = color[w];
                if (cw >= 0 && hi_count[cw] < 2) hi_count[cw]++;
            }
        }
        
        // 4. sample the color: used color at most 2 times
        // observation 3.7: expect 2 trails
        while (true) {
            int c = available_colors[rng() % available_colors.size()];
            if (hi_count[c] < 2) {
                color[v] = c;
                return;
            }
        }
    }

    /**
     * @brief recolor batch (algo 7)
     *
     * expected O(log n) rounds
     * 
     * @param S 
     */
    void recolor_batch(parlay::sequence<vertex> S) {
        if (S.empty()) return;

        // mark vertices in S
        parlay::sequence<uint8_t> in_S(n, 0);
        parlay::parallel_for(0, S.size(), [&](long i) {
            in_S[S[i]] = 1;
        });

        parlay::random_generator gen((size_t)global_time.load());
        long rounds = 0;

        while (!S.empty()) {
            long size = S.size();
            
            // 1. uncolor all
            parlay::parallel_for(0, size, [&](long i) {
                color[S[i]] = -1;
            });

            // 2. sample new colors
            parlay::parallel_for(0, size, [&](long i) {
                auto rng = gen[(size_t)(rounds * n + S[i])];
                sample_from_palette(S[i], rng);
            });

            // 3. detect conflicts
            auto conflicts = parlay::tabulate(size, [&](long i) -> parlay::sequence<vertex> {
                vertex v = S[i];
                int c = color[v];
                int l = level[v];

                parlay::sequence<vertex> out;
                bool flagged_self = false;
                for (vertex w: adj[v]) {
                    if (in_S[w] && level[w] >= l && color[w] == c) {
                        if (!flagged_self) {
                            out.push_back(v);
                            flagged_self = true;
                        }
                        out.push_back(w);
                    }
                }
                return out;
            });
            auto conflict_sorted = sort_dedup_vertices(parlay::flatten(conflicts));

            // 4. uncolor the conflicts
            parlay::parallel_for(0, (long)conflict_sorted.size(), [&](long i) {
                color[conflict_sorted[i]] = -1;
            });
            auto S_ok = parlay::filter(S, [&](vertex v) {
                return !std::binary_search(conflict_sorted.begin(), conflict_sorted.end(), v);
            });


            // 5. For each v in S_ok, find the neighbor
            // w in V\S with level[w] > level[v] and color[w] == color[v].
            auto conflict_w_raw = parlay::map(S_ok, [&](vertex v) -> vertex {
                int c = color[v];
                int l = level[v];
                for (vertex w: adj[v]) {
                    if (!in_S[w] && level[w] > l && color[w] == c) return w;
                }
                return -1;
            });
            auto conflict_w_raw2 = parlay::filter(conflict_w_raw, [](vertex w) { return w >= 0; });
            auto conflict_w = sort_dedup_vertices(conflict_w_raw2);

            // 6. finalize S_ok
            long t_base = global_time.fetch_add((long)S_ok.size());
            parlay::parallel_for(0, (long)S_ok.size(), [&](long i) {
                timestamp[S_ok[i]] = t_base + i;
            });
            parlay::parallel_for(0, (long)S_ok.size(), [&](long i) {
                in_S[S_ok[i]] = 0;
            });
            parlay::parallel_for(0, (long)conflict_w.size(), [&](long i) {
                in_S[conflict_w[i]] = 1;
            });

            // 7. update states
            S = parlay::append(conflict_sorted, conflict_w);
            rounds++;
        }
        last_recolor_rounds = rounds;
    }

    /**
     * @brief insert edge batch (algo 5)
     *
     * recolor conflicts with the larger time stamp
     * 
     * @param batch 
     */
    void add_edge_batch(const edges& batch) {
        if (batch.empty()) return;

        // 1. update adjacency list
        auto directed = parlay::flatten(
            parlay::map(batch, [](edge e) -> parlay::sequence<edge> {
                return {{e.first, e.second}, {e.second, e.first}};
            })
        );
        auto new_neighbors = parlay::group_by_index(directed, n);
        parlay::parallel_for(0, n, [&](long v) {
            if (!new_neighbors[v].empty()) {
                adj[v] = parlay::append(adj[v], new_neighbors[v]);
            }
        });

        // 2. collect conflicts to S
        auto candidates = parlay::map(batch, [&](edge e) -> vertex {
            auto [u, v] = e;
            if (color[u] >= 0 && color[u] == color[v]) {
                if (timestamp[u] >= timestamp[v]) return u;
                return v;
            }
            return -1;
        });

        auto S_raw = parlay::filter(candidates, [](vertex v) { return v >= 0; });
        auto S = sort_dedup_vertices(S_raw);

        // 3. recolor S
        recolor_batch(std::move(S));
    }

    /**
     * @brief delete edge batch (algo 6)
     *
     * delete edge and recolor v whose d_le decreases by k with some probability
     * 
     * @param batch 
     */
    void delete_edge_batch(const edges& batch) {
        if (batch.empty()) return;

        // 1. collect affected vertices
        auto affected_raw = parlay::flatten(
            parlay::map(batch, [](edge e) -> parlay::sequence<vertex> {
                return {e.first, e.second};
            })
        );
        auto affected = sort_dedup_vertices(affected_raw);
        long m_affected = (long)affected.size();

        // 2. compute d_le(v) before deletion
        auto d_le_old = parlay::map(affected, [&](vertex v) -> int {
            int l = level[v];
            int cnt = 0;
            for (vertex w: adj[v]) {
                if (level[w] <= l) cnt++;
            }
            return cnt;
        });

        // 3. update adjacency list
        auto del_keys = parlay::sort(
            parlay::map(batch, [](edge e){
                return std::make_pair(std::min(e.first, e.second), std::max(e.first, e.second));
            })
        );

        parlay::parallel_for(0, m_affected, [&](long i) {
            vertex v = affected[i];
            adj[v] = parlay::filter(adj[v], [&](vertex w) {
                auto key = std::make_pair(std::min(v, w), std::max(v, w));
                return !std::binary_search(del_keys.begin(), del_keys.end(), key);
            });
        });

        // 4. compute d_le(v) after deletion
        auto d_le_new = parlay::map(affected, [&](vertex v) -> int {
            int l = level[v];
            int cnt = 0;
            for (vertex w: adj[v]) {
                if (level[w] <= l) cnt++;
            }
            return cnt;
        });

        // 5. probability build S
        parlay::random_generator gen((size_t)global_time.load()+ 777UL);
        auto in_S_flags = parlay::tabulate(m_affected, [&](long i) -> bool {
            int old_d = d_le_old[i];
            int new_d = d_le_new[i];
            if (old_d <= new_d) return false;

            int k = old_d - new_d;
            int denom = Delta + 1 - new_d;

            if (denom <= 0) return true;

            auto rng = gen[(size_t)i];
            return (int)(rng() % (unsigned long long) denom) < k;
        });

        auto S = parlay::pack(affected, in_S_flags);

        recolor_batch(std::move(S));
    }

    const parlay::sequence<int>& get_colors() const { return color; }
    const parlay::sequence<int>& get_coloring() const { return color; }
};
