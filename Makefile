CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

TARGET = server
SRC = ./src/main.cpp ./src/http/parser.cpp ./src/router/router.cpp ./src/net/socket.cpp
OBJ = $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean: 
	rm -f $(TARGET)