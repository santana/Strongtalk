PROGRAMS = strongtalk stest

stest_DIRS = $(TEST_DIR) $(EASYUNIT_DIR)
stest_INCLUDEDIRS = $(stest_DIRS)
stest_SO = strongtalk.so stest.so

strongtalk_DIRS = $(VM_DIR)
strongtalk_INCLUDEDIRS = $(strongtalk_DIRS)
strongtalk_SO = strongtalk.so
strongtalk_LDLIBS = -lpthread -ldl -lrt

WRK=$(shell pwd)

#ifneq ($(WRK),$(BUILD_DIR))
#$(warning "Running in $(WRK)")
#$(error "Must be run from build directory: $(BUILD_DIR)")
#endif

.PHONY: vm
vm: strongtalk

.PHONY: test
test: stest
	LD_LIBRARY_PATH=$(WRK) ./$< -b ../strongtalk.bst

include ../build.linux/strongtalk.mk
