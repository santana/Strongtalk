ROOT_DIR	= $(HOME)/Strongtalk
TEST_SRC_DIR 	:= $(ROOT_DIR)/test
VM_SRC_DIR 	:= $(ROOT_DIR)/vm
EASYUNIT_DIR	:= $(ROOT_DIR)/easyunit
MAKE_DIR	:= $(ROOT_DIR)/build.linux
BUILD_DIR	:= $(ROOT_DIR)/build
SRCS            := $(wildcard $(VM_SRC_DIR)/*/*.cpp)
TEST_SRCS	:= $(wildcard $(TEST_SRC_DIR)/*/*.cpp)
OBJS		:= $(SRCS:%.cpp=%.o)
DEPFILES	:= $(SRCS:%.cpp=%.d)

INCLUDES	= -I$(VM_SRC_DIR)

TEST_INCLUDES	=                   \
	-I$(EASYUNIT_DIR)/easyunit  \
	-I$(TEST_SRC_DIR)/runtime   \
	-I$(TEST_SRC_DIR)/utilities \
	-I$(TEST_SRC_DIR)/memory

ASM		= $(CC)

DEFINES		= -DDELTA_COMPILER -DASSERT -DDEBUG -D__LINUX__
CXXFLAGS	= -fno-rtti -Wno-write-strings -fno-operator-names \
		  -O0 -fPIC -g \
		  $(DEFINES)
DEPFLAGS        = -MT $@ -MMD -MP -MF $*.d
LDFLAGS		=

COMPILE.cpp = $(CXX) $(DEPFLAGS) $(CFLAGS) $(CXXFLAGS) $(INCLUDES) $(TARGET_ARCH) -c

%.o : %.cpp %.d
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

$(DEPFILES):

objs: $(OBJS)

include $(wildcard $(DEPFILES))
