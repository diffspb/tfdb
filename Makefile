CXX ?= c++
AR ?= ar

CPPFLAGS := -Iinclude -Isrc
CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++14 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wformat=2 -Wnull-dereference -Wdouble-promotion -pthread
LDFLAGS += -pthread

BUILD_DIR := build
LIB_SOURCES := \
	src/status.cpp \
	src/storage_posix.cpp \
	src/memory_storage.cpp \
	src/counting_storage.cpp \
	src/codec.cpp \
	src/internal_format.cpp \
	src/ring_store.cpp \
	src/record_profile.cpp \
	src/async_writer.cpp
LIB_OBJECTS := $(LIB_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
LIBRARY := $(BUILD_DIR)/libtfdb.a
TOOL_NAMES := tfdb_format tfdb_inspect tfdb_verify tfdb_dump tfdb_loadgen
TOOL_BINARIES := $(TOOL_NAMES:%=$(BUILD_DIR)/tools/%)

.PHONY: all clean test tools tool-test check sanitize tsan coverage crash-matrix benchmark

all: $(LIBRARY)

TEST_BINARY := $(BUILD_DIR)/tests/tfdb_tests

$(TEST_BINARY): tests/test_main.cpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

test: $(TEST_BINARY)
	$(TEST_BINARY)

$(BUILD_DIR)/tools/%: tools/%.cpp tools/common.hpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -Itools $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

tools: $(TOOL_BINARIES)

tool-test: tools
	TOOL_DIR=$(abspath $(BUILD_DIR)/tools) bash tests/tool_smoke.sh

check: test tool-test

sanitize:
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) BUILD_DIR=build/sanitize \
		CXXFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="-pthread -fsanitize=address,undefined" test tool-test

tsan:
	$(MAKE) BUILD_DIR=build/tsan \
		CXXFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=thread" \
		LDFLAGS="-pthread -fsanitize=thread" test

coverage:
	rm -rf build/coverage
	$(MAKE) BUILD_DIR=build/coverage CXXFLAGS="-O0 -g --coverage" \
		LDFLAGS="-pthread --coverage" test tool-test
	OBJECT_DIR=$(abspath build/coverage/src) bash tests/coverage_summary.sh

crash-matrix: $(TEST_BINARY)
	timeout 120 $(TEST_BINARY) --filter torn_tail
	timeout 120 $(TEST_BINARY) --filter compressed_block_torn
	timeout 120 $(TEST_BINARY) --filter multiple_dirty_blocks
	timeout 120 $(TEST_BINARY) --filter failed_flush_may_persist
	timeout 120 $(TEST_BINARY) --filter backend_write_before_writer_state
	timeout 120 $(TEST_BINARY) --filter writer_incarnation_chain
	timeout 120 $(TEST_BINARY) --filter rotation_commit_failpoints
	timeout 120 $(TEST_BINARY) --filter dirty_rotation_failpoints
	timeout 120 $(TEST_BINARY) --filter footer_persisted_without_index
	timeout 120 $(TEST_BINARY) --filter seal_index_footer_subsets
	timeout 120 $(TEST_BINARY) --filter new_partition_header_torn
	timeout 120 $(TEST_BINARY) --filter reused_partition_header_torn
	timeout 120 $(TEST_BINARY) --filter memory_fault_model
	timeout 120 $(TEST_BINARY) --filter deterministic_crash_model

benchmark: tools
	TOOL_DIR=$(abspath $(BUILD_DIR)/tools) bash benchmarks/run_profiles.sh

$(LIBRARY): $(LIB_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(LIB_OBJECTS:.o=.d)
