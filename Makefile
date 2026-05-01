# ParlayLib (header-only) + parlaylib/examples graph helpers
#
# Usage:
#   make                         # build both drivers
#   make dynamic_graph_color_dplus1
#   make dynamic_graph_color_cbyd
#   make CXX=clang++            # different compiler
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -pthread -Wall -Wextra

PARLAY_ROOT  := parlaylib
PARLAY_INC   := $(PARLAY_ROOT)/include
EXAMPLES_INC := $(PARLAY_ROOT)/examples

INCLUDES := -I. -I$(PARLAY_INC) -I$(EXAMPLES_INC)

TARGETS := dynamic_graph_color_dplus1 dynamic_graph_color_cbyd

.PHONY: all clean

all: $(TARGETS)

dynamic_graph_color_dplus1: dynamic_graph_color_dplus1.cpp dynamic_graph_color_dplus1.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ dynamic_graph_color_dplus1.cpp

dynamic_graph_color_cbyd: dynamic_graph_color_cbyd.cpp dynamic_graph_color_cbyd.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ dynamic_graph_color_cbyd.cpp

clean:
	rm -f $(TARGETS)
