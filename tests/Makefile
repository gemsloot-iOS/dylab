# Linux host tests for Path C transforms / packaging / link smoke.
# Requires: g++, zlib. Link smoke also needs libtcc built for ARM64 ELF target.

CXX ?= g++
CC ?= gcc
ROOT := $(abspath ..)
BRIDGE := $(ROOT)/DyLabApp/Bridge
TCC_LIBDIR ?= /tmp/tcc-arm64-lib

CXXFLAGS ?= -std=c++17 -O2 -Wall -I$(BRIDGE) -I$(TCC_LIBDIR)
CFLAGS ?= -O2 -Wall -I$(BRIDGE)
LDFLAGS_Z ?= -lz
LDFLAGS_TCC ?= -L$(TCC_LIBDIR) -ltcc -ldl -lm

.PHONY: all clean lowering deb link e2e

all: lowering deb

lowering: test_objc_lowering
	./test_objc_lowering

deb: test_deb_packager
	./test_deb_packager

link: test_link_smoke
	./test_link_smoke

e2e:
	bash ./test_e2e_pipeline.sh

test_objc_lowering: test_objc_lowering.cpp $(BRIDGE)/ObjCLowering.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_objc_lowering.cpp $(BRIDGE)/ObjCLowering.cpp

test_deb_packager: test_deb_packager.c $(BRIDGE)/DebPackager.c
	$(CC) $(CFLAGS) -o $@ test_deb_packager.c $(BRIDGE)/DebPackager.c $(LDFLAGS_Z)

test_link_smoke: test_link_smoke.cpp $(BRIDGE)/MachODylibLinker.cpp $(BRIDGE)/MachOSigner.cpp
	$(CXX) $(CXXFLAGS) -o $@ test_link_smoke.cpp \
	  $(BRIDGE)/MachODylibLinker.cpp $(BRIDGE)/MachOSigner.cpp \
	  $(LDFLAGS_TCC)

clean:
	rm -f test_objc_lowering test_deb_packager test_link_smoke
