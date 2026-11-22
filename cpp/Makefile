# Plain-make build for people without CMake.
#
#   make -j            build kvstore, kv_tests, kv_bench
#   make test          build and run the unit tests
#   make clean
#
# Override the compiler with CXX=... (e.g. CXX=clang++).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS  ?=

ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32
else
  LDLIBS += -pthread
endif

BUILD := build

SRCS := $(wildcard src/*.cpp src/*/*.cpp)
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))

# src/main.cpp defines main(); it must not end up inside the shared objects
# linked into the test and bench binaries.
LIB_OBJS := $(filter-out $(BUILD)/src/main.o,$(OBJS))

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SRCS))

.PHONY: all test clean

all: $(BUILD)/kvstore $(BUILD)/kv_tests $(BUILD)/kv_bench

$(BUILD)/kvstore: $(LIB_OBJS) $(BUILD)/src/main.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/kv_tests: $(LIB_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/kv_bench: $(LIB_OBJS) $(BUILD)/bench/bench.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test: $(BUILD)/kv_tests
	./$(BUILD)/kv_tests

clean:
	rm -rf $(BUILD)
