CXX ?= g++
TARGET := main
SRC := main.cpp

CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -Wpedantic
FT_CFLAGS := $(shell pkg-config --cflags freetype2)
FT_LIBS := $(shell pkg-config --libs freetype2)

.PHONY: all clean run debug

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(FT_CFLAGS) $(SRC) -o $(TARGET) $(FT_LIBS)

run: $(TARGET)
	./$(TARGET) --mode art --fps 60

debug:
	$(CXX) -O0 -g -std=c++17 -Wall -Wextra -Wpedantic \
		$(FT_CFLAGS) $(SRC) -o $(TARGET) $(FT_LIBS)

clean:
	rm -f $(TARGET)
