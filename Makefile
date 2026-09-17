CXX      ?= c++
CXXFLAGS  = -std=c++17 -Wall -Wextra -g -O2 -pthread -Iinclude
PYTHON   ?= python3

HEADER   = include/guarded.hpp
EXAMPLES = bin/factory_guarded bin/factory_condvar bin/philosophers bin/queues

all: $(EXAMPLES) bin/test_guarded

bin/%: examples/%.cpp $(HEADER) | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin/test_guarded: tests/test_guarded.cpp $(HEADER) | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin/test_guarded_tsan: tests/test_guarded.cpp $(HEADER) | bin
	$(CXX) $(CXXFLAGS) -fsanitize=thread $< -o $@

bin/test_guarded_asan: tests/test_guarded.cpp $(HEADER) | bin
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined $< -o $@

bin:
	mkdir -p bin

# The C++ tests, every example, and the Python tests.
test: all
	bin/test_guarded
	bin/queues
	bin/philosophers --meals 2 > /dev/null
	bin/factory_guarded --trips 1 > /dev/null
	bin/factory_condvar --trips 1 > /dev/null
	$(PYTHON) -m unittest discover -s python

# The C++ tests under ThreadSanitizer, then AddressSanitizer and UBSan.
sanitize: bin/test_guarded_tsan bin/test_guarded_asan
	bin/test_guarded_tsan
	bin/test_guarded_asan

clean:
	rm -rf bin python/__pycache__

.PHONY: all test sanitize clean
