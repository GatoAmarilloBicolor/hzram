CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -DZRAM_TESTING
INCLUDES = -I../../src/system/kernel/vm/zram

SRCS = zram_test.cpp \
       platform_host.cpp \
       ../../src/system/kernel/vm/zram/zram_core.cpp \
       ../../src/system/kernel/vm/zram/lz4/lz4.c

OBJS = $(SRCS:.cpp=.o)
OBJS := $(OBJS:.c=.o)

TARGET = zram_test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SRCS) -lpthread

test: $(TARGET)
	./$(TARGET)

asan:
	$(CXX) -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -DZRAM_TESTING $(INCLUDES) -o $(TARGET)_asan $(SRCS) -lpthread
	./$(TARGET)_asan

clean:
	rm -f $(TARGET) $(TARGET)_asan *.o
