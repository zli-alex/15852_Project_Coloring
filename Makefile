# Dynamic graph coloring driver + ParlayLib (header-only) + parlaylib/examples helpers
#
# Usage:
#   make              # build ./dynamic_graph_color_dplus1
#   make CXX=clang++  # use a different compiler
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -pthread -Wall -Wextra

PARLAY_ROOT  := parlaylib
PARLAY_INC   := $(PARLAY_ROOT)/include
EXAMPLES_INC := $(PARLAY_ROOT)/examples

INCLUDES := -I. -I$(PARLAY_INC) -I$(EXAMPLES_INC)

TARGET := dynamic_graph_color_dplus1
SRC    := dynamic_graph_color_dplus1.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) dynamic_graph_color_dplus1.h dynamic_graph_color.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SRC)

clean:
	rm -f $(TARGET)
