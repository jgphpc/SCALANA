SRC_ROOT := .

CXX := g++
CXXFLAGS := -std=c++11 -fno-rtti -UNDEBUG -g -O0 -fno-omit-frame-pointer -fstack-protector-all 
PLUGIN_CXXFLAGS := -fpic

LLVM_CXXFLAGS := `llvm-config --cxxflags`
LLVM_LDFLAGS := `llvm-config --ldflags --libs --system-libs`

LLVM_LDFLAGS_NOLIBS := `llvm-config --ldflags`
PLUGIN_LDFLAGS := -shared

AR := ar
ARTAG := rcs

MPI_INCLUDE := -I/opt/intel/impi/5.0.3.048/intel64/include

all: irs libsampler analyze
#libtrace

irs: irs.o
	$(CXX) $(CXXFLAGS) $(PLUGIN_CXXFLAGS) $(PLUGIN_LDFLAGS) \
		irs.o -o $@.so

irs.o: $(SRC_ROOT)/IRStruct.cpp
	$(CXX) $(LLVM_CXXFLAGS) $(CXXFLAGS) $(PLUGIN_CXXFLAGS) $(SRC_ROOT)/IRStruct.cpp -c -o $@

libtrace: libtrace.o
	$(AR) $(ARTAG) $@.a $<

libtrace.o: $(SRC_ROOT)/trace.cpp
	$(CXX) $(CXXFLAGS) $(MPI_INCLUDE) -c -o $@ $<


#libsampler: libsampler.o
#	$(AR) $(ARTAG) $@.a $<

#libsampler.o: $(SRC_ROOT)/sampler.cpp
#	$(CXX) $(CXXFLAGS) $(MPI_INCLUDE) -ldl -Wl,--no-undefined  -fPIC -lmpi -lpapi -c -o $@ $<


libsampler: $(SRC_ROOT)/sampler.cpp
	mpicxx $(CXXFLAGS) $(MPI_INCLUDE) -ldl -Wl,--no-undefined  -lunwind -shared -fPIC -lmpi -lpapi -o $@.so $<
	#$(CXX) $(CXXFLAGS) $(MPI_INCLUDE) -ldl -Wl,--no-undefined -lunwind -shared -fPIC -lmpi -lpapi -o $@.so $<

analyze:log2stat.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf *.o libsampler.so  analyze
