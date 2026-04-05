RGB_LIB_DIR=../hub75-matrix/rpi-rgb-led-matrix
CXXFLAGS=-O2 -Wall -I$(RGB_LIB_DIR)/include
LDFLAGS=-L$(RGB_LIB_DIR)/lib -lrgbmatrix -lpthread -lm

all: snake storm5000

snake: snake.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

storm5000: storm5000.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f snake storm5000
