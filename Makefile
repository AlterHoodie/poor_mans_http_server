CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

TARGET = server
SRC = ./src/main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean: 
	rm -f $(TARGET)