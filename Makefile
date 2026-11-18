CXX := clang++
CXXFLAGS := -std=c++20 -O2 -pthread -Iinclude -Wall -Wextra
BUILD := build
BINS := $(BUILD)/snapfsctl $(BUILD)/test_unit $(BUILD)/test_crash $(BUILD)/test_concurrency

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/snapfs.o: src/snapfs.cpp include/snapfs/snapfs.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/snapfsctl: src/main.cpp $(BUILD)/snapfs.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/test_unit: tests/test_unit.cpp $(BUILD)/snapfs.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/test_crash: tests/test_crash.cpp $(BUILD)/snapfs.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/test_concurrency: tests/test_concurrency.cpp $(BUILD)/snapfs.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BINS)
	./$(BUILD)/test_unit
	./$(BUILD)/test_crash
	./$(BUILD)/test_concurrency

tsan: | $(BUILD)
	$(CXX) -std=c++20 -O1 -g -pthread -fsanitize=thread -Iinclude src/snapfs.cpp tests/test_concurrency.cpp -o $(BUILD)/test_concurrency_tsan
	./$(BUILD)/test_concurrency_tsan

bench: $(BUILD)/snapfsctl
	rm -f $(BUILD)/bench.img
	./$(BUILD)/snapfsctl $(BUILD)/bench.img mkfs 256
	./$(BUILD)/snapfsctl $(BUILD)/bench.img bench

clean:
	rm -rf $(BUILD)

.PHONY: all test tsan bench clean
