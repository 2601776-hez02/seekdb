CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread -DNDEBUG -DMVDB_WANT_AVX2 -mavx2 -mfma
LDFLAGS ?= -pthread

SRCS = src/main.cpp src/engine.cpp
OBJS = $(SRCS:.cpp=.o)

minivectordb: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f minivectordb $(OBJS)

.PHONY: clean
