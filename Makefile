# MD Glitch: core tests and Mac tools.
#   make          tools (build/t/mdrender, build/t/testpiece)
#   make check    unit and end-to-end tests (MDCT, QMF, engine latency/transparency/CPU)
#   make qlink    Q-Link matrix test against the built plugin (after plugin/MDGlitch/make.sh)
#   make renders  test piece + demo renders in renders/ (tools/render-all.sh)
#   make aarch64  static Key 37 engine bench (Zig from the MPC SDK)
#   make arm32    static Live II engine bench
CXX      ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
HDRS     := $(wildcard core/*.h) tools/wavio.h
MPC_SDK  ?= $(HOME)/dev/mpc-plugin-builder-sdk
ZIG      := $(MPC_SDK)/.sdk-tools/zigenv/bin/python -m ziglang

all: build/t/mdrender build/t/testpiece

build/t/%: tools/%.cpp $(HDRS)
	@mkdir -p build/t
	$(CXX) $(CXXFLAGS) $< -o $@

build/t/test_%: tests/test_%.cpp $(HDRS)
	@mkdir -p build/t
	$(CXX) $(CXXFLAGS) $< -o $@

check: build/t/test_mdct build/t/test_qmf build/t/test_link build/t/test_engine
	build/t/test_mdct
	build/t/test_qmf
	build/t/test_link
	build/t/test_engine

qlink: build/t/qlinktest
	build/t/qlinktest plugin/MDGlitch/build/native/md-glitch.dylib

renders: all
	tools/render-all.sh

aarch64: build/aarch64/test_engine build/aarch64/mdbench

arm32: build/arm32/test_engine build/arm32/mdbench

build/aarch64/mdbench: tools/mdbench.cpp $(HDRS)
	@mkdir -p build/aarch64
	$(ZIG) c++ -target aarch64-linux-musl -mcpu=cortex_a55 $(CXXFLAGS) -static $< -o $@

build/arm32/mdbench: tools/mdbench.cpp $(HDRS)
	@mkdir -p build/arm32
	$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a17 -mfpu=neon-vfpv4 $(CXXFLAGS) -static $< -o $@

build/arm32/test_engine: tests/test_engine.cpp $(HDRS)
	@mkdir -p build/arm32
	$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a17 $(CXXFLAGS) -static $< -o $@

build/aarch64/test_engine: tests/test_engine.cpp $(HDRS)
	@mkdir -p build/aarch64
	$(ZIG) c++ -target aarch64-linux-musl -mcpu=cortex_a76 $(CXXFLAGS) -static $< -o $@

.PHONY: all check renders aarch64 arm32
