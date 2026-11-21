CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
# Must live on a Linux filesystem (not /mnt/c under WSL).
ROOTFS   ?= rootfs
ALPINE_URL ?= https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

LIB_SRC := src/cli.cpp src/sandbox.cpp src/fs.cpp src/cgroup.cpp src/monitor.cpp \
           src/pressure.cpp src/elastic.cpp src/softland.cpp src/recorder.cpp src/image.cpp \
           src/state.cpp
LIB_OBJ := $(LIB_SRC:src/%.cpp=build/%.o)

.PHONY: all test itest eval workloads rootfs pydemo clean

all: crun

crun: build/main.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p build

build/unit_tests: tests/unit_tests.cpp $(LIB_OBJ) | build
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $^

build/integration: tests/integration.cpp tests/proc.hpp | build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/eval: tests/eval.cpp tests/proc.hpp | build
	$(CXX) $(CXXFLAGS) -o $@ $<

# Static so they run inside the musl-based Alpine rootfs.
build/memhog: tests/memhog.cpp | build
	$(CXX) $(CXXFLAGS) -static -o $@ $<

build/phasehog: tests/phasehog.cpp | build
	$(CXX) $(CXXFLAGS) -static -o $@ $<

test: build/unit_tests
	./build/unit_tests

rootfs: $(ROOTFS)/bin/busybox

$(ROOTFS)/bin/busybox:
	mkdir -p $(ROOTFS)
	curl -fsSL $(ALPINE_URL) | tar -xz -C $(ROOTFS)

workloads: build/memhog build/phasehog rootfs
	install -D -m 755 build/memhog $(ROOTFS)/usr/local/bin/memhog
	install -D -m 755 build/phasehog $(ROOTFS)/usr/local/bin/phasehog

itest: crun build/integration workloads
	./build/integration ./crun $(ROOTFS)

eval: crun build/eval workloads
	./build/eval ./crun $(ROOTFS) build/results

# Real-image demo: Docker only fetches the image, crun runs it. Needs root and Docker.
PYIMAGE ?= python:3.12-slim
PYROOT  := /var/lib/crun/images/$(subst @,_,$(subst :,_,$(subst /,_,$(PYIMAGE))))

pydemo: crun
	test -d $(PYROOT) || ./crun pull $(PYIMAGE)
	mkdir -p build/results
	./crun run --memory 256M --record build/results/python-static --rootfs $(PYROOT) -- python3 - < tests/py/footprint.py
	./crun run --elastic --memory 256M --record build/results/python-elastic --rootfs $(PYROOT) -- python3 - < tests/py/footprint.py

clean:
	rm -rf build crun

-include $(wildcard build/*.d)
