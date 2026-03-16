# Makefile — libmembus C++17
#
# Builds libmembus.a and test binaries.
# Usage:
#   make              Build library
#   make test         Build and run all tests
#   make install      Install library and header (sudo)
#   make uninstall    Remove installed files (sudo)
#   make clean        Remove build artifacts

PREFIX   = /usr/local
LIBDIR   = $(PREFIX)/lib
INCDIR   = $(PREFIX)/include/membus

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -Iinclude
LDLIBS   = -lrt -lpthread
ARFLAGS  = rcs

SRCDIR   = src
BUILDDIR = build
TESTDIR  = tests

# ── Library ──

SRCS = $(wildcard $(SRCDIR)/*.cpp)
OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
LIB  = $(BUILDDIR)/libmembus.a

# ── Test binaries ──

TEST_MEMBUS = $(BUILDDIR)/test_membus

TESTS = $(TEST_MEMBUS)

# ── Targets ──

.PHONY: all clean test install uninstall help

all: $(LIB)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp include/membus.hpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	ar $(ARFLAGS) $@ $^

# ── Test build rules ──

$(TEST_MEMBUS): $(TESTDIR)/test_membus.cpp $(LIB) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -I$(TESTDIR) -DTEST_MAIN_FN=test_membus_run -o $@ $< -L$(BUILDDIR) -lmembus $(LDLIBS)

# ── Test runner ──

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; ./$$t || exit 1; done

# ── Install / Uninstall ──

install: $(LIB)
	install -d $(LIBDIR)
	install -m 644 $(LIB) $(LIBDIR)/libmembus.a
	install -d $(INCDIR)
	install -m 644 include/membus.hpp $(INCDIR)/
	@echo "libmembus installed to $(LIBDIR)/libmembus.a"

uninstall:
	rm -f $(LIBDIR)/libmembus.a
	rm -rf $(INCDIR)
	@echo "libmembus uninstalled"

# ── Clean ──

clean:
	rm -rf $(BUILDDIR)

# ── Help ──

help:
	@echo "libmembus C++17 — available targets:"
	@echo "  all       Build libmembus.a (default)"
	@echo "  test      Build and run all tests"
	@echo "  install   Install library and header to PREFIX (default: /usr/local)"
	@echo "  uninstall Remove installed library and header"
	@echo "  clean     Remove build artifacts"
