# ParlayLib (header-only) + parlaylib/examples graph helpers
#
# Usage:
#   make                              # build original drivers
#   make test                         # build + run both correctness suites
#   make bench                        # build all benchmark binaries
#   make dynamic_graph_color_dplus1
#   make dynamic_graph_color_cbyd
#   make test_correctness_dplus1
#   make test_correctness_cbyd
#   make bench_thread_scaling
#   make bench_c_scaling
#   make bench_dataset_scaling
#   make CXX=clang++                  # different compiler
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -pthread -Wall -Wextra

PARLAY_ROOT  := parlaylib
PARLAY_INC   := $(PARLAY_ROOT)/include
EXAMPLES_INC := $(PARLAY_ROOT)/examples

INCLUDES := -I. -I$(PARLAY_INC) -I$(EXAMPLES_INC)

SNAP_LOADER_DEP := snap_loader.h graph_types.h

ORIG_TARGETS  := dynamic_graph_color_dplus1 dynamic_graph_color_cbyd
TEST_TARGETS  := test_correctness_dplus1 test_correctness_cbyd
BENCH_TARGETS := bench_thread_scaling bench_c_scaling bench_dataset_scaling

.PHONY: all test bench clean

all: $(ORIG_TARGETS)

test: $(TEST_TARGETS)
	@echo "--- Running DynamicGraphColorDplus1 correctness tests ---"
	./test_correctness_dplus1
	@echo "--- Running DynamicGraphColorCbyD correctness tests ---"
	./test_correctness_cbyd

bench: $(BENCH_TARGETS)

# ── Original drivers ─────────────────────────────────────────────────────────
dynamic_graph_color_dplus1: dynamic_graph_color_dplus1.cpp dynamic_graph_color_dplus1.h graph_types.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ dynamic_graph_color_dplus1.cpp

dynamic_graph_color_cbyd: dynamic_graph_color_cbyd.cpp dynamic_graph_color_cbyd.h graph_types.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ dynamic_graph_color_cbyd.cpp

# ── Correctness test suites ───────────────────────────────────────────────────
test_correctness_dplus1: test_correctness_dplus1.cpp \
                         dynamic_graph_color_dplus1.h graph_types.h $(SNAP_LOADER_DEP)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test_correctness_dplus1.cpp

test_correctness_cbyd: test_correctness_cbyd.cpp \
                       dynamic_graph_color_cbyd.h graph_types.h $(SNAP_LOADER_DEP)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test_correctness_cbyd.cpp

# ── Benchmark binaries ────────────────────────────────────────────────────────
bench_thread_scaling: bench_thread_scaling.cpp \
                      dynamic_graph_color_dplus1.h dynamic_graph_color_cbyd.h graph_types.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ bench_thread_scaling.cpp

bench_c_scaling: bench_c_scaling.cpp dynamic_graph_color_cbyd.h graph_types.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ bench_c_scaling.cpp

bench_dataset_scaling: bench_dataset_scaling.cpp \
                       dynamic_graph_color_dplus1.h dynamic_graph_color_cbyd.h \
                       graph_types.h $(SNAP_LOADER_DEP)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ bench_dataset_scaling.cpp

clean:
	rm -f $(ORIG_TARGETS) $(TEST_TARGETS) $(BENCH_TARGETS)
