CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -g -fno-omit-frame-pointer
# -g and -fno-omit-frame-pointer only add symbols / keep frame pointers so that
# `perf record -g` produces readable call graphs; codegen stays -O2.

bench: src/bench.cpp include/*.hpp src/workload.hpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isrc src/bench.cpp -o bench

verify: bench
	./bench --verify

clean:
	rm -f bench
	rm -rf bench.dSYM

.PHONY: verify clean
