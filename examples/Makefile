CXX ?= c++

TFDB_ROOT := ..
BUILD_DIR := build
LIBRARY := $(TFDB_ROOT)/build/libtfdb.a
CPPFLAGS := -I$(TFDB_ROOT)/include
CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++14 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wformat=2 -Wnull-dereference -Wdouble-promotion -pthread
LDFLAGS += -pthread

PROGRAMS := $(BUILD_DIR)/tfdb_quickstart $(BUILD_DIR)/tfdb_async_writer

.PHONY: all check clean

all: $(PROGRAMS)

$(LIBRARY):
	$(MAKE) -C $(TFDB_ROOT) all

$(BUILD_DIR)/tfdb_quickstart: quickstart.cpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

$(BUILD_DIR)/tfdb_async_writer: async_writer.cpp $(LIBRARY)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) -o $@

check: all
	@tmp_dir=$$(mktemp -d); \
	trap 'rm -rf "$$tmp_dir"' EXIT; \
	$(BUILD_DIR)/tfdb_quickstart "$$tmp_dir/quickstart.tfdb"; \
	$(BUILD_DIR)/tfdb_async_writer "$$tmp_dir/async.tfdb"

clean:
	rm -rf $(BUILD_DIR)
