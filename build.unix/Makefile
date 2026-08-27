# Portable make rules for the Strongtalk VM (Linux and macOS).
# The platform is auto-detected. ROOT_DIR is derived from this file's
# directory, so the build works from any checkout.

ROOT_DIR	:= $(abspath $(dir $(realpath $(firstword $(MAKEFILE_LIST))))/..)
TEST_DIR 	:= $(ROOT_DIR)/test
VM_DIR 		:= $(ROOT_DIR)/vm
EASYUNIT_DIR	:= $(ROOT_DIR)/easyunit
BUILD_DIR	:= $(ROOT_DIR)/build

ASM		= $(CC)

DEFINES		= -DDELTA_COMPILER -DASSERT -DDEBUG
DEPFLAGS        = -MT $@ -MMD -MP -MF $*.d
# gnu++17 matches the default used by recent g++ (Linux build); needed for
# std::is_same_v / if constexpr in growableArray.hpp
CXXFLAGS	= -std=gnu++17 -fno-rtti -Wno-write-strings -fno-operator-names \
		  -O0 -fPIC -g \
		  $(ARCH_FLAGS) $(DEFINES) $(DEPFLAGS) $(INCLUDES)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
# On Apple Silicon the VM builds natively for arm64 using the AArch64 assembler
# backend (the MAP_JIT runtime handles the W+X restriction). x86-64 is still
# available by overriding ARCH_FLAGS=-arch x86_64 and removing the AArch64
# define; on Intel hosts the JIT emits x86-64 machine code.
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
ARCH_FLAGS	= -arch arm64
DEFINES		+= -DDELTA_ASSEMBLER_BACKEND_AARCH64
else
ARCH_FLAGS	= -arch x86_64
endif
SHLIB_FLAG	= -dynamiclib -undefined dynamic_lookup
LIBRARY_PATH_VAR = DYLD_LIBRARY_PATH
else
SHLIB_FLAG	= -shared
LIBRARY_PATH_VAR = LD_LIBRARY_PATH
endif

PROGRAMS = strongtalk stest

strongtalk_DIRS = $(VM_DIR)
strongtalk_INCLUDEDIRS = $(strongtalk_DIRS)
strongtalk_SO = strongtalk.so
strongtalk_LDLIBS = -lpthread -ldl
ifneq ($(UNAME),Darwin)
strongtalk_LDLIBS += -lrt
endif

stest_DIRS = $(TEST_DIR) $(EASYUNIT_DIR)
stest_INCLUDEDIRS = $(stest_DIRS)
stest_SO = strongtalk.so stest.so

WRK=$(shell pwd)

.PHONY: all vm test clean pristine
all: $(PROGRAMS)

vm: strongtalk

test: stest
	$(LIBRARY_PATH_VAR)=$(WRK) ./$< -b ../strongtalk.bst

%.o : %.cpp %.d

define PROGRAM_template
$(1)_SRCS       := $$(foreach dir,$$($(1)_DIRS),$$(wildcard $$(dir)/*/*.cpp))
$(1)_SRCS	:= $$(if $$(filter stest,$(1)),$$(filter-out $(TEST_DIR)/assembler/%,$$($(1)_SRCS)),$$($(1)_SRCS))
ifeq ($(UNAME_M),arm64)
$(1)_SRCS	:= $$(filter-out %/mapping_x86.cpp %/assembler_x86.cpp,$$($(1)_SRCS))
else
$(1)_SRCS	:= $$(filter-out %/mapping_aarch64.cpp %/assembler_aarch64.cpp,$$($(1)_SRCS))
endif
$(1)_OBJS	:= $$($(1)_SRCS:%.cpp=%.o)
$(1)_DEPFILES	:= $$($(1)_SRCS:%.cpp=%.d)
$(1)_INCLUDES	:= $$($(1)_INCLUDEDIRS:%=-I%)

INCLUDES += $$($(1)_INCLUDES)

.PHONY: $(1)-objs
$(1)-objs: $$($(1)_OBJS)

$(1).so: $$($(1)_OBJS)
	$$(CXX) $(SHLIB_FLAG) $(ARCH_FLAGS) -o $$@ $$(filter-out %/main.o,$$($(1)_OBJS)) $$($(1)_LDFLAGS) $$($(1)_LDLIBS)

$(1): $$($(1)_SO)
	$$(CXX) $(LDFLAGS) $(ARCH_FLAGS) -o $$@ $$(filter %/main.o,$$($(1)_OBJS)) $$($(1)_SO)

$$($(1)_DEPFILES):

ALL_OBJS	+= $$($(1)_OBJS)
ALL_DEPFILES	+= $$($(1)_DEPFILES)
ALL_SHLIBS	+= $(1).so

include $$(wildcard $$($(1)_DEPFILES))
endef

$(foreach prog,$(PROGRAMS),$(eval $(call PROGRAM_template,$(prog))))

clean:
	rm -f $(ALL_OBJS) $(ALL_SHLIBS) $(PROGRAMS)

pristine:
	rm -f $(ALL_DEPFILES)
