CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
LDFLAGS =
LDLIBS =

# -g - adds debug symbols Ex: Function Names, Variable Mappings, Source Locations

# -O0 - No form of compiler optimizations
# -O1 - Some form of compiler optimizations
# -O2 - Standard form of Compiler Optimizations
# -O3 - Aggressive Optimizations
# -Ofast - Unsafe Math Optimizations
# -march=native - Optimize for whatever CPU code is running on

# -pthread - Required for threading

# -fno-omit-frame-pointer - prevents compilers from removing frame pointers, so that profiles can go through 
# the stack traces
# -fsanitize=address - helps detecting buffer overflows, invalid memory accesses etc, nice debug flag
# -fsanitize=undefined - Helps find UB, invalid shifts, bad casts , again nice debug flag

TARGET = server
SRC = ./src/main.cpp ./src/http/parser.cpp ./src/router/router.cpp ./src/net/socket.cpp
OBJ = $(SRC:.cpp=.o)

.PHONY: all debug profiling run clean

all: $(TARGET)

# Switching between all / debug / profiling: run `make clean` first so .o files are not reused with wrong flags.

debug: CXXFLAGS += -g -O0 -fsanitize=address
debug: LDFLAGS += -fsanitize=address
debug: $(TARGET)

profiling: CXXFLAGS += -g -O2 -fno-omit-frame-pointer
profiling: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)
