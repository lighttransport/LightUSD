# Simple Makefile for standalone build
CXX ?= clang++
CXXFLAGS = -std=c++14 -O3 -Wall -Wextra -pthread -DLIGHTUSD_ENABLE_THREAD=1
INCLUDES = -I../../src
LDFLAGS = -pthread

# Library path (adjust if needed)
LIBDIR = ../../build
LIBS = -L$(LIBDIR) -llightusd_static

TARGET = pprint_benchmark
SOURCES = pprint_benchmark.cc

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

help:
	@echo "Usage:"
	@echo "  make          - Build the benchmark"
	@echo "  make clean    - Remove build artifacts"
	@echo ""
	@echo "Note: Make sure to build the main lightusd library first:"
	@echo "  cd ../../build && make"
