include ../build.linux/makefile-macros.incl

TEST_EXECUTABLE = $(BUILD_DIR)/strongtalk-test
TEST_SHARED_LIB = $(BUILD_DIR)/strongtalk-test.so
EXECUTABLE = $(BUILD_DIR)/strongtalk
SHARED_LIB = $(BUILD_DIR)/strongtalk.so
PRECOMPILED_HEADERS = $(BUILD_DIR)/incls/_precompiled.incl.gch
WRK=$(shell pwd)

TEST_DIRECTORIES=                 \
	$(TEST_SRC_DIR)/utilities \
	$(TEST_SRC_DIR)/runtime   \
	$(TEST_SRC_DIR)/prims     \
	$(TEST_SRC_DIR)/main      \
	$(EASYUNIT_DIR)/src

VM_DIRECTORIES	=                 \
	$(VM_SRC_DIR)/asm         \
	$(VM_SRC_DIR)/code        \
	$(VM_SRC_DIR)/compiler    \
	$(VM_SRC_DIR)/disasm      \
	$(VM_SRC_DIR)/interpreter \
	$(VM_SRC_DIR)/lookup      \
	$(VM_SRC_DIR)/memory      \
	$(VM_SRC_DIR)/oops        \
	$(VM_SRC_DIR)/prims       \
	$(VM_SRC_DIR)/recompiler  \
	$(VM_SRC_DIR)/runtime     \
	$(VM_SRC_DIR)/utilities

ifneq ($(WRK),$(BUILD_DIR))
$(warning "Running in $(WRK)")
$(error "Must be run from build directory: $(BUILD_DIR)")
endif

all: dependencies objects shlib vm test

test: $(TEST_EXECUTABLE)
	export LD_LIBRARY_PATH=$(WRK)
	$(TEST_EXECUTABLE) -b ../strongtalk.bst

shlib: $(SHARED_LIB)

$(SHARED_LIB): precompiled-headers
	$(CXX) -v -shared -o $@ -L/usr/lib $$(find $(VM_SRC_DIR) -name "*.o"|grep -v main.o) -lpthread -ldl -lrt

vm: $(EXECUTABLE)

$(EXECUTABLE): $(SHARED_LIB) $(VM_SRC_DIR)/runtime/main.o
	$(CXX) -v $(LDFLAGS) -o $@ -L/usr/lib $(VM_SRC_DIR)/*/main.o $(SHARED_LIB)

$(TEST_SHARED_LIB):
	$(CXX) -v $(LDFLAGS) -shared -o $@ -L/usr/lib $$(find $(TEST_SRC_DIR) -name "*.o"|grep -v main.o) $(EASYUNIT_DIR)/src/*.o

$(TEST_EXECUTABLE):
	$(CXX) -v $(LDFLAGS) -o $@ $(SHARED_LIB) $(TEST_SHARED_LIB) $(TEST_SRC_DIR)/main/main.o

objects: vm-objects test-objects

test-objects: $(PRECOMPILED_HEADERS)
	for i in $(TEST_DIRECTORIES);\
	do \
		CXXFLAGS="$(CXXFLAGS)" $(MAKE) -C $$i $$(ls $$i/*.cpp | sed 's/\.cpp/\.o/g'); \
	done

vm-objects: $(PRECOMPILED_HEADERS)
	for i in $(VM_DIRECTORIES);\
	do \
		CXXFLAGS="$(CXXFLAGS)" $(MAKE) -C $$i $$(ls $$i/*.cpp | sed 's/\.cpp/\.o/g'); \
	done

precompiled-headers: $(PRECOMPILED_HEADERS)

$(PRECOMPILED_HEADERS): $(BUILD_DIR)/includeDB.all
	$(CXX) -c -x c++-header -o $@ $(CXXFLAGS) $(BUILD_DIR)/incls/_precompiled.incl

$(BUILD_DIR)/makedeps: $(TOOLS_SRC_DIR)/makedeps/makeDeps.cpp
	$(CXX) -Wno-write-strings -o $@ $^

dependencies: $(BUILD_DIR)/makedeps $(INCL_PLATFORM) $(BUILD_DIR)/includeDB.all
	$(BUILD_DIR)/makedeps $(INCL_PLATFORM) $(BUILD_DIR)/includeDB.all

$(BUILD_DIR)/includeDB.all: $(INCL_DB1) $(INCL_DB2) 
	rm -rf $(INCL_DIR)
	rm -f $(BUILD_DIR)/Dependencies.hh $(BUILD_DIR)/includeDB.all $(BUILD_DIR)/includeDB.current
	mkdir $(INCL_DIR)
	cat $(INCL_DB1) $(INCL_DB2) >> $(BUILD_DIR)/includeDB.all

$(INCL_PLATFORM):
	touch $@

clean: test-clean vm-clean

vm-clean:
	rm -rf $(INCL_DIR)
	rm -f $(BUILD_DIR)/Dependencies.hh
	rm -f $(BUILD_DIR)/includeDB.all $(BUILD_DIR)/includeDB.current
	rm -f $(SHARED_LIB) $(EXECUTABLE)
	find $(VM_DIRECTORIES) -name '*.o' -exec rm -f {} \;

test-clean:
	rm -f $(TEST_SHARED_LIB) $(TEST_EXECUTABLE)
	find $(TEST_DIRECTORIES) -name '*.o' -exec rm -f {} \;

pristine: clean
	rm -f $(BUILD_DIR)/makedeps
