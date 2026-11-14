CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread -DNDEBUG -DMVDB_WANT_AVX2 -mavx2 -mfma
LDFLAGS ?= -pthread

SRCS = src/main.cpp src/engine.cpp
OBJS = $(SRCS:.cpp=.o) dist_avx512.o

minivectordb: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# the AVX-512 kernel lives in its own TU so the rest of the binary stays
# AVX2-compatible; dispatch is by runtime cpuid
dist_avx512.o: src/dist_avx512.cpp
	$(CXX) $(CXXFLAGS) -mavx512f -mavx512dq -mavx512vl -c -o $@ $<

clean:
	rm -f minivectordb $(OBJS)

.PHONY: clean
