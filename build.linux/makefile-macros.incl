ROOT_DIR	= $(HOME)/Strongtalk
TEST_SRC_DIR 	= $(ROOT_DIR)/test
VM_SRC_DIR 	= $(ROOT_DIR)/vm
EASYUNIT_DIR	= $(ROOT_DIR)/easyunit
TOOLS_SRC_DIR	= $(ROOT_DIR)/tools
MAKE_DIR	= $(ROOT_DIR)/build.linux
BUILD_DIR	= $(ROOT_DIR)/build
BIN_DIR		= $(ROOT_DIR)/bin
INCL_DIR	= $(BUILD_DIR)/incls

INCL_PATH	=                   \
	-I$(BUILD_DIR)              \
	-I$(VM_SRC_DIR)/asm         \
	-I$(VM_SRC_DIR)/code        \
	-I$(VM_SRC_DIR)/compiler    \
	-I$(VM_SRC_DIR)/disasm      \
	-I$(VM_SRC_DIR)/interpreter \
	-I$(VM_SRC_DIR)/lookup      \
	-I$(VM_SRC_DIR)/memory      \
	-I$(VM_SRC_DIR)/oops        \
	-I$(VM_SRC_DIR)/prims       \
	-I$(VM_SRC_DIR)/recompiler  \
	-I$(VM_SRC_DIR)/runtime     \
	-I$(VM_SRC_DIR)/topIncludes \
	-I$(VM_SRC_DIR)/utilities

TEST_INCL_PATH	=                   \
	-I$(EASYUNIT_DIR)/easyunit  \
	-I$(TEST_SRC_DIR)/runtime   \
	-I$(TEST_SRC_DIR)/utilities \
	-I$(TEST_SRC_DIR)/memory

DEFINES		= -DDELTA_COMPILER -DASSERT -DDEBUG -D__LINUX__
CXXFLAGS	= -fno-rtti -Wno-write-strings -fno-operator-names \
		  -O0 -fPIC -g \
		  $(DEFINES) $(INCL_PATH) $(TEST_INCL_PATH)
LDFLAGS		=

ASM		= $(CC)

INCL_DB1	= $(VM_SRC_DIR)/deps/includeDB
INCL_DB2	= $(VM_SRC_DIR)/deps/includeDB2
INCL_PLATFORM	= $(MAKE_DIR)/makedeps-platform.txt
