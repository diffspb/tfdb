CXX ?= c++
AR ?= ar
CARGO ?= cargo

CPPFLAGS := -Iinclude -Isrc
CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++14 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wformat=2 -Wnull-dereference -Wdouble-promotion -pthread
LDFLAGS += -pthread

BUILD_DIR := build
LIB_SOURCES := \
	src/status.cpp \
	src/version.cpp \
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

.PHONY: all clean test tools tool-test check sanitize tsan coverage crash-matrix benchmark cxx17-check \
	fuzz fuzz-build rust-build rust-test conformance build-system-test

all: $(LIBRARY)

TEST_BINARY := $(BUILD_DIR)/tests/tfdb_tests
TEST_CASES := $(wildcard tests/cases/*.inc)
CONFORMANCE_WRITER := $(BUILD_DIR)/tests/tfdb_make_conformance_volume
CONFORMANCE_CASE_WRITER := $(BUILD_DIR)/tests/tfdb_make_conformance_cases
RUST_MANIFEST := rust/tfdb-reader/Cargo.toml
RUST_TARGET_DIR := $(abspath $(BUILD_DIR)/rust)

$(TEST_BINARY): tests/test_main.cpp $(TEST_CASES) $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

test: $(TEST_BINARY)
	$(TEST_BINARY)

$(CONFORMANCE_WRITER): tests/make_conformance_volume.cpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

$(CONFORMANCE_CASE_WRITER): tests/make_conformance_cases.cpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

$(BUILD_DIR)/tools/%: tools/%.cpp tools/common.hpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -Itools $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

tools: $(TOOL_BINARIES)

tool-test: tools
	TOOL_DIR=$(abspath $(BUILD_DIR)/tools) bash tests/tool_smoke.sh

check: test tool-test

rust-build:
	CARGO_TARGET_DIR=$(RUST_TARGET_DIR) $(CARGO) build --manifest-path $(RUST_MANIFEST) --bins

rust-test:
	CARGO_TARGET_DIR=$(RUST_TARGET_DIR) $(CARGO) test --manifest-path $(RUST_MANIFEST)

conformance: tools $(CONFORMANCE_WRITER) $(CONFORMANCE_CASE_WRITER) rust-build
	CPP_TOOL_DIR=$(abspath $(BUILD_DIR)/tools) \
	CONFORMANCE_WRITER=$(abspath $(CONFORMANCE_WRITER)) \
	CONFORMANCE_CASE_WRITER=$(abspath $(CONFORMANCE_CASE_WRITER)) \
	RUST_BIN_DIR=$(RUST_TARGET_DIR)/debug bash tests/cross_language_conformance.sh

build-system-test:
	bash tests/build_frontends.sh

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

# Media fuzzing. Sanitizers are the detector, so the target always builds its
# own ASan/UBSan binary rather than reusing $(BUILD_DIR).
#
#   make fuzz                          quick regular run
#   make fuzz FUZZ_ITERATIONS=500000   long run
#   make fuzz FUZZ_JOBS=$$(nproc)       shard across independent processes
#   make fuzz FUZZ_SEED=$$(date +%s)    explore a new seed
#
# FUZZ_ITERATIONS is the total across shards, so changing FUZZ_JOBS changes
# wall time rather than how much work is done. Every finding prints its seed
# and iteration and saves the image; replay it with:
#   build/fuzz/tfdb_fuzz_media --image PATH
FUZZ_BUILD_DIR := build/fuzz
FUZZ_BINARY := $(FUZZ_BUILD_DIR)/tfdb_fuzz_media
FUZZ_CORPUS ?= testdata/format-v1/valid-mixed.tfdb \
                testdata/format-v1/codec-lz4-block.tfdb
FUZZ_ITERATIONS ?= 20000
FUZZ_SEED ?= 20260906
FUZZ_ARTIFACTS ?= $(FUZZ_BUILD_DIR)
FUZZ_CXXFLAGS ?= -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
FUZZ_TIMEOUT ?= 0
FUZZ_JOBS ?= 1

$(FUZZ_BINARY): tests/fuzz_media.cpp $(LIB_SOURCES) $(wildcard include/tfdb/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(FUZZ_CXXFLAGS) $< $(LIB_SOURCES) \
		$(LDFLAGS) -fsanitize=address,undefined -o $@

fuzz-build: $(FUZZ_BINARY)

fuzz: $(FUZZ_BINARY)
	FUZZ_BINARY=$(abspath $(FUZZ_BINARY)) FUZZ_CORPUS="$(FUZZ_CORPUS)" \
	FUZZ_ITERATIONS=$(FUZZ_ITERATIONS) FUZZ_SEED=$(FUZZ_SEED) \
	FUZZ_JOBS=$(FUZZ_JOBS) FUZZ_ARTIFACTS=$(FUZZ_ARTIFACTS) \
	FUZZ_TIMEOUT=$(FUZZ_TIMEOUT) \
	bash tests/run_fuzz.sh

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

# The [[nodiscard]] on Status only engages at C++17. Prove the whole tree,
# including tools, examples, and tests, still leaves no Status unchecked.
cxx17-check:
	@for source in $(LIB_SOURCES) tools/*.cpp examples/*.cpp \
	    tests/test_main.cpp tests/make_conformance_volume.cpp \
	    tests/make_conformance_cases.cpp; do \
	  $(CXX) $(CPPFLAGS) -Itools -std=c++17 -Wall -Wextra -Wpedantic -Werror \
	    -fsyntax-only "$$source" || exit 1; \
	done
	@echo "cxx17-check: no unchecked Status and no new warnings"
