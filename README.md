# SCALANA

Dependence:
1. GCC-4.8.5
2. LLVM-3.3.0 / Dragonegg-3.3.0
3. PAPI-5.2.0
4. libunwind-1.3.1

Compilation:
Use make to compile ScalAna

Usage:
1. clang -g -c -emit-llvm -O0 -o test.bc test.c
#llvm-dis test.bc

2. opt -loop-simplify -mergereturn test.bc -o normalized.bc
#llvm-dis normalized.bc

#extract the Program Structure Graph 
3. opt -load ../scalana.so -scalana -if ../in.txt -of out.txt normalized.bc -o result.bc
#llvm-dis result.bc

4. llc -O0 -filetype=obj result.bc

5. mpicc result.o -L../ -L/opt/intel/impi/5.0.3.048/intel64/lib  -lmpi -lsampler -lstdc++ -rdynamic -o result
rm -f ./LOG* ./SAMPLE* ./stat*

#run and profiling
6.LD_LIBRARY_PATH=./:$LD_LIBRARY_PATH mpirun -np 2 ./result

#post-mortem analysis
7. ./parse.sh ./result SAMPLE*
./analyze 2
