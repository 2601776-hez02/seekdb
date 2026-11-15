CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread -DNDEBUG -DMVDB_WANT_AVX2 -mavx2 -mfma
LDFLAGS ?= -pthread
DEPFLAGS = -MMD -MP

SRCS = src/main.cpp src/engine.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

minivectordb: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -f minivectordb $(OBJS) $(DEPS)

.PHONY: clean
