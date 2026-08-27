/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

# Standalone x86 encoder tests.
#
# The same test source is compiled twice against the two x86 encoders:
#   assemblerEncoderTest_64  - the native (64-bit) encoder
#   assemblerEncoderTest_32  - the 32-bit encoder (via -DDELTA_X86_32)
#
# Both encoders emit byte-identical code for the shared instruction set, and
# each must match the golden bytes checked by the test itself.
#
# The test links only the two encoder translation units. Symbols that the
# encoder references but never calls at run time (Universe globals, the
# ostream, print_reg, store_check, ...) are left unresolved; like the VM's
# own shared-library build this uses -undefined dynamic_lookup on macOS and
# --unresolved-symbols=ignore-all on Linux.

ROOT_DIR := $(abspath $(dir $(realpath $(firstword $(MAKEFILE_LIST))))/../..)
VM_DIR   := $(ROOT_DIR)/vm

CXX      ?= c++
CXXFLAGS  = -std=gnu++17 -fno-rtti -Wno-write-strings -fno-operator-names \
            -O0 -g -DDELTA_COMPILER -DASSERT -DDEBUG -I$(VM_DIR)

ENCODER_SRCS = $(VM_DIR)/asm/assembler.cpp $(VM_DIR)/asm/assembler_x86.cpp

ENCODER_SRCS_AARCH64 = $(VM_DIR)/asm/assembler.cpp $(VM_DIR)/asm/assembler_aarch64.cpp

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  UNDEFINED_OK = -undefined dynamic_lookup
else
  UNDEFINED_OK = -Wl,--unresolved-symbols=ignore-all
endif

.PHONY: all test clean

all: assemblerEncoderTest_64 assemblerEncoderTest_32 assemblerEncoderTest_aarch64

assemblerEncoderTest_64: assemblerEncoderTest.cpp $(ENCODER_SRCS)
	$(CXX) $(CXXFLAGS) $(ENCODER_SRCS) assemblerEncoderTest.cpp \
	  $(UNDEFINED_OK) -o $@

assemblerEncoderTest_32: assemblerEncoderTest.cpp $(ENCODER_SRCS)
	$(CXX) $(CXXFLAGS) -DDELTA_X86_32 $(ENCODER_SRCS) assemblerEncoderTest.cpp \
	  $(UNDEFINED_OK) -o $@

assemblerEncoderTest_aarch64: assemblerEncoderTest_aarch64.cpp $(ENCODER_SRCS_AARCH64)
	$(CXX) $(CXXFLAGS) -DDELTA_ASSEMBLER_BACKEND_AARCH64 \
	  $(ENCODER_SRCS_AARCH64) assemblerEncoderTest_aarch64.cpp \
	  $(UNDEFINED_OK) -o $@

test: all
	./assemblerEncoderTest_64
	./assemblerEncoderTest_32
	./assemblerEncoderTest_aarch64

clean:
	rm -f assemblerEncoderTest_64 assemblerEncoderTest_32 assemblerEncoderTest_aarch64
