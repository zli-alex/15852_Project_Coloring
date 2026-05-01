#pragma once
// graph_types.h — shared type aliases and utilities used by both
// DynamicGraphColorDplus1 and DynamicGraphColorCbyD.

#include <utility>
#include <parlay/primitives.h>
#include <parlay/sequence.h>

using vertex = int;
using edge   = std::pair<vertex, vertex>;
using edges  = parlay::sequence<edge>;
using graph  = parlay::sequence<parlay::sequence<vertex>>;

/**
 * @brief Sort and deduplicate a sequence of vertices
 */
static parlay::sequence<vertex> sort_dedup_vertices(parlay::sequence<vertex> S) {
    if (S.empty()) return S;
    parlay::integer_sort_inplace(S, [](vertex v) { return (size_t)v; });
    auto first_appear = parlay::tabulate(S.size(), [&](size_t i) -> bool {
        return (i == 0 || S[i] != S[i - 1]);
    });
    return parlay::pack(S, first_appear);
}
