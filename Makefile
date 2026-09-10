# Portable make rules for the Strongtalk VM (Linux and macOS).
# The platform is auto-detected. ROOT_DIR is derived from this file's
# location, so the build works from any checkout.
#
# Every build is fully described by a (compiler, OS, architecture) triple
# and lands in its own out-of-tree build directory:
#
#     BUILD_DIR default:  build/<arch>-<os>-<compiler>
#
# Examples:
#     make                          # native: e.g. build/arm64-macos-clang
#     make ARCH=x86_64              # forced x86-64 on macOS: build/x86_64-macos-clang
#     make CXX=clang++ build/...    # alternate compiler
#     make BUILD_DIR=/tmp/cfg ...   # fully custom location
#
# Different configs never share objects, so switching configs needs no clean.

ROOT_DIR	:= $(abspath $(dir $(realpath $(firstword $(MAKEFILE_LIST)))))
TEST_DIR 	:= $(ROOT_DIR)/test
VM_DIR 		:= $(ROOT_DIR)/vm
EASYUNIT_DIR	:= $(ROOT_DIR)/easyunit

CC		?= cc
CXX		?= c++

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
OS		= macos
# On Apple Silicon the VM builds natively for arm64 using the AArch64 assembler
# backend (the MAP_JIT runtime handles the W+X restriction). x86-64 is still
# available by overriding ARCH=x86_64; on Intel hosts the JIT emits x86-64
# machine code.
SHLIB_FLAG	= -dynamiclib -undefined dynamic_lookup
LIBRARY_PATH_VAR = DYLD_LIBRARY_PATH
else
OS		= linux
SHLIB_FLAG	= -shared
LIBRARY_PATH_VAR = LD_LIBRARY_PATH
endif

# Target architecture: explicit ARCH wins, otherwise the legacy ARCH_FLAGS
# override, otherwise the native host architecture.
ifeq ($(ARCH),)
ifeq ($(findstring x86_64,$(ARCH_FLAGS)),x86_64)
ARCH		= x86_64
else ifeq ($(findstring arm64,$(ARCH_FLAGS)),arm64)
ARCH		= arm64
else
UNAME_M		:= $(shell uname -m)
ifeq ($(UNAME_M),aarch64)
ARCH		= arm64
else
ARCH		= $(UNAME_M)
endif
endif
endif

# Compiler token for the build-directory name (clang vs gcc, else basename).
COMPILER_VERSION := $(shell $(CXX) --version 2>/dev/null | head -1)
COMPILER	:= $(shell echo "$(COMPILER_VERSION)" | grep -qi clang && echo clang || \
		    (echo "$(COMPILER_VERSION)" | grep -qiE 'gcc|g\+\+|GCC' && echo gcc || basename $(CXX)))

# Out-of-tree build directory; mkdir in case it does not exist yet.
BUILD_DIR	?= $(ROOT_DIR)/build/$(ARCH)-$(OS)-$(COMPILER)
$(shell mkdir -p $(BUILD_DIR))

# Select the assembler/mapping backend by the *target* architecture.
ifeq ($(ARCH),x86_64)
TARGET_ARCH_X86_64 = 1
else
TARGET_ARCH_AARCH64 = 1
endif

# macOS -arch flag (Linux compiles for the host via the toolchain directly).
ifeq ($(UNAME),Darwin)
ifeq ($(origin ARCH_FLAGS),command line)
else
ARCH_FLAGS	= -arch $(ARCH)
endif
ifeq ($(ARCH),arm64)
DEFINES		+= -DDELTA_ASSEMBLER_BACKEND_AARCH64
endif
endif

PROGRAMS = strongtalk stest

strongtalk_DIRS = $(VM_DIR)
strongtalk_INCLUDEDIRS = $(strongtalk_DIRS)
strongtalk_SO = $(BUILD_DIR)/strongtalk.so
strongtalk_LDLIBS = -lpthread -ldl
ifneq ($(UNAME),Darwin)
strongtalk_LDLIBS += -lrt
endif

stest_DIRS = $(TEST_DIR) $(EASYUNIT_DIR)
stest_INCLUDEDIRS = $(stest_DIRS)
stest_SO = $(BUILD_DIR)/strongtalk.so $(BUILD_DIR)/stest.so

.PHONY: all vm test clean pristine format format-check
all: $(addprefix $(BUILD_DIR)/,$(PROGRAMS))

vm: $(BUILD_DIR)/strongtalk

test: $(BUILD_DIR)/stest
	$(LIBRARY_PATH_VAR)=$(BUILD_DIR) $(BUILD_DIR)/stest -b $(ROOT_DIR)/strongtalk.bst

# Files clang-format should operate on (sorted, excludes build artifacts).
# *.inl/.ixx patterns are not used here; add them if introduced.
FORMAT_SRCS	:= $(sort $(wildcard $(VM_DIR)/*/*.cpp $(VM_DIR)/*/*.hpp \
						$(TEST_DIR)/*/*.cpp $(TEST_DIR)/*/*.hpp \
						$(EASYUNIT_DIR)/src/*.cpp $(EASYUNIT_DIR)/src/*.h \
						$(EASYUNIT_DIR)/easyunit/*.h \
						$(sort $(wildcard $(EASYUNIT_DIR)/src/*.hpp))))

# Hand-aligned / generated tables that clang-format cannot reproduce and must
# not rewrite. Excluded from both `make format` and `make format-check`.
FORMAT_EXCLUDES	:= $(VM_DIR)/interpreter/dispatchTable.cpp
FORMAT_SRCS	:= $(filter-out $(FORMAT_EXCLUDES),$(FORMAT_SRCS))

CLANG_FORMAT	:= $(shell command -v clang-format 2>/dev/null)
CLANG_FORMAT_STYLE := $(ROOT_DIR)/.clang-format

# `make format` rewrites every source file in place using the repo's
# .clang-format. If clang-format is not installed it prints a hint and does
# nothing (so the target is safe to run anywhere).
format: $(CLANG_FORMAT_STYLE)
	@if [ -z "$(CLANG_FORMAT)" ]; then \
		echo "clang-format not found; install it or use 'brew install clang-format'."; \
	else \
		$(CLANG_FORMAT) -i --style=file $(FORMAT_SRCS); \
		echo "Formatted $$(echo $(FORMAT_SRCS) | wc -w | tr -d ' ') files."; \
	fi

# `make format-check` verifies that sources already obey .clang-format
# without modifying them (useful as a CI gate / pre-commit check).
format-check: $(CLANG_FORMAT_STYLE)
	@if [ -z "$(CLANG_FORMAT)" ]; then \
		echo "clang-format not found; install it or use 'brew install clang-format'."; \
	else \
		$(CLANG_FORMAT) --dry-run --Werror --style=file $(FORMAT_SRCS) \
			> /tmp/strongtalk-format-check.log 2>&1; \
		if [ $$? -ne 0 ]; then \
			echo "clang-format violations found:"; \
			cat /tmp/strongtalk-format-check.log; \
			exit 1; \
		else \
			echo "All .cpp/.hpp/.h files are correctly formatted."; \
		fi; \
	fi

# Objects and dependency files live in the build directory, mirroring the
# source tree under $(BUILD_DIR)/obj. mkdir each needed subdirectory.
define PROGRAM_template
$(1)_SRCS       := $$(foreach dir,$$($(1)_DIRS),$$(wildcard $$(dir)/*/*.cpp))
$(1)_SRCS	:= $$(if $$(filter stest,$(1)),$$(filter-out $(TEST_DIR)/assembler/%,$$($(1)_SRCS)),$$($(1)_SRCS))
ifeq ($(TARGET_ARCH_X86_64),1)
$(1)_SRCS	:= $$(filter-out %/mapping_aarch64.cpp %/assembler_aarch64.cpp,$$($(1)_SRCS))
else
$(1)_SRCS	:= $$(filter-out %/mapping_x86.cpp %/assembler_x86.cpp,$$($(1)_SRCS))
endif
$(1)_OBJS	:= $$(patsubst $(ROOT_DIR)/%.cpp,$(BUILD_DIR)/obj/%.o,$$($(1)_SRCS))
$(1)_DEPFILES	:= $$(patsubst $(ROOT_DIR)/%.cpp,$(BUILD_DIR)/obj/%.d,$$($(1)_SRCS))
$(1)_INCLUDES	:= $$($(1)_INCLUDEDIRS:%=-I%)
INCLUDES += $$($(1)_INCLUDES)

.PHONY: $(1)-objs
$(1)-objs: $$($(1)_OBJS)

$(BUILD_DIR)/$(1).so: $$($(1)_OBJS)
	$$(CXX) $(SHLIB_FLAG) $(ARCH_FLAGS) -o $$@ $$(filter-out %/main.o,$$($(1)_OBJS)) $$($(1)_LDFLAGS) $$($(1)_LDLIBS)

$(BUILD_DIR)/$(1): $$($(1)_SO)
	$$(CXX) $(LDFLAGS) $(ARCH_FLAGS) -o $$@ $$(filter %/main.o,$$($(1)_OBJS)) $$($(1)_SO)

$$($(1)_DEPFILES):

ALL_OBJS	+= $$($(1)_OBJS)
ALL_DEPFILES	+= $$($(1)_DEPFILES)
ALL_SHLIBS	+= $$($(1)_SO)
ALL_BINS	+= $(BUILD_DIR)/$(1)

include $$(wildcard $$($(1)_DEPFILES))
endef

$(foreach prog,$(PROGRAMS),$(eval $(call PROGRAM_template,$(prog))))

# mkdir every object directory inside the build dir (single parse-time pass).
$(shell mkdir -p $(sort $(dir $(ALL_OBJS))))

# Compile rule: each object mirrors a source under $(BUILD_DIR)/obj.
$(ALL_OBJS): $(BUILD_DIR)/obj/%.o: $(ROOT_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

pristine:
	rm -f $(ALL_DEPFILES)