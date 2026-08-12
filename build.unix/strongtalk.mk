# Portable make rules for the Strongtalk VM (Linux and macOS).
# This single file is used from build.unix/; the platform is detected
# automatically. ROOT_DIR is derived from the top-level Makefile's location,
# so the build works from any checkout.

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
		  $(DEFINES) $(DEPFLAGS) $(INCLUDES)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
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
$(1)_OBJS	:= $$($(1)_SRCS:%.cpp=%.o)
$(1)_DEPFILES	:= $$($(1)_SRCS:%.cpp=%.d)
$(1)_INCLUDES	:= $$($(1)_INCLUDEDIRS:%=-I%)

INCLUDES += $$($(1)_INCLUDES)

.PHONY: $(1)-objs
$(1)-objs: $$($(1)_OBJS)

$(1).so: $$($(1)_OBJS)
	$$(CXX) $(SHLIB_FLAG) -o $$@ $$$$(find $($(1)_DIRS) -name "*.o"|grep -v main.o) $$($(1)_LDFLAGS) $$($(1)_LDLIBS)

$(1): $$($(1)_SO)
	$$(CXX) $$(LDFLAGS) -o $$@ $$$$(find $($(1)_DIRS) -name main.o) $$($(1)_SO)

$$($(1)_DEPFILES):

ALL_OBJS	+= $$($(1)_OBJS)
ALL_DEPFILES	+= $$($(1)_DEPFILES)
ALL_SHLIBS	+= $(1).so

include $(wildcard $$($(1)_DEPFILES))
endef

$(foreach prog,$(PROGRAMS),$(eval $(call PROGRAM_template,$(prog))))

clean:
	rm -f $(ALL_OBJS) $(ALL_SHLIBS) $(PROGRAMS)

pristine:
	rm -f $(ALL_DEPFILES)
