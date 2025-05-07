ROOT_DIR	= $(HOME)/Strongtalk
TEST_DIR 	:= $(ROOT_DIR)/test
VM_DIR 		:= $(ROOT_DIR)/vm
EASYUNIT_DIR	:= $(ROOT_DIR)/easyunit
MAKE_DIR	:= $(ROOT_DIR)/build.linux
BUILD_DIR	:= $(ROOT_DIR)/build

ASM		= $(CC)

DEFINES		= -DDELTA_COMPILER -DASSERT -DDEBUG
DEPFLAGS        = -MT $@ -MMD -MP -MF $*.d
CXXFLAGS	= -fno-rtti -Wno-write-strings -fno-operator-names \
		  -O0 -fPIC -g \
		  $(DEFINES) $(DEPFLAGS) $(INCLUDES)

.PHONY: all
all: $(PROGRAMS)

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
	$$(CXX) -shared -o $$@ $$$$(find $($(1)_DIRS) -name "*.o"|grep -v main.o) $$($(1)_LDFLAGS) $$($(1)_LDLIBS)

$(1): $$($(1)_SO)
	$$(CXX) $$(LDFLAGS) -o $$@ $$$$(find $($(1)_DIRS) -name main.o) $$($(1)_SO)

$$($(1)_DEPFILES):

ALL_OBJS	+= $$($(1)_OBJS)
ALL_DEPFILES	+= $$($(1)_DEPFILES)
ALL_SHLIBS	+= $(1).so

include $(wildcard $$($(1)_DEPFILES))
endef

$(foreach prog,$(PROGRAMS),$(eval $(call PROGRAM_template,$(prog))))

.PHONY: clean
clean:
	rm -f $(ALL_OBJS) $(ALL_SHLIBS) $(PROGRAMS)

.PHONY: pristine
pristine:
	rm -f $(ALL_DEPFILES)
