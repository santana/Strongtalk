include ../build.linux/Makefile.inc

TEST_EXECUTABLE = $(BUILD_DIR)/strongtalk-test
TEST_SHARED_LIB = $(BUILD_DIR)/strongtalk-test.so
EXECUTABLE = $(BUILD_DIR)/strongtalk
SHARED_LIB = $(BUILD_DIR)/strongtalk.so
WRK=$(shell pwd)

TEST_DIRECTORIES=                 \
	$(TEST_SRC_DIR)/utilities \
	$(TEST_SRC_DIR)/runtime   \
	$(TEST_SRC_DIR)/prims     \
	$(TEST_SRC_DIR)/main      \
	$(EASYUNIT_DIR)/src

ifneq ($(WRK),$(BUILD_DIR))
$(warning "Running in $(WRK)")
$(error "Must be run from build directory: $(BUILD_DIR)")
endif

all: dependencies objects shlib vm test

test: $(TEST_EXECUTABLE)
	export LD_LIBRARY_PATH=$(WRK)
	$(TEST_EXECUTABLE) -b ../strongtalk.bst

shlib: $(SHARED_LIB)

$(SHARED_LIB): $(OBJS)
	$(CXX) -v -shared -o $@ -L/usr/lib $$(find $(VM_SRC_DIR) -name "*.o"|grep -v main.o) -lpthread -ldl -lrt

vm: $(EXECUTABLE)

$(EXECUTABLE): $(SHARED_LIB) $(VM_SRC_DIR)/runtime/main.o
	$(CXX) -v $(LDFLAGS) -o $@ -L/usr/lib $(VM_SRC_DIR)/*/main.o $(SHARED_LIB)

$(TEST_SHARED_LIB):
	$(CXX) -v $(LDFLAGS) -shared -o $@ -L/usr/lib $$(find $(TEST_SRC_DIR) -name "*.o"|grep -v main.o) $(EASYUNIT_DIR)/src/*.o

$(TEST_EXECUTABLE):
	$(CXX) -v $(LDFLAGS) -o $@ $(SHARED_LIB) $(TEST_SHARED_LIB) $(TEST_SRC_DIR)/main/main.o

clean: test-clean vm-clean

vm-clean:
	rm -f $(SHARED_LIB) $(EXECUTABLE)
	find $(VM_SRC_DIR) -name '*.[od]' -exec rm -f {} \;

test-clean:
	rm -f $(TEST_SHARED_LIB) $(TEST_EXECUTABLE)
	find $(TEST_SRC_DIR) -name '*.[od]' -exec rm -f {} \;
