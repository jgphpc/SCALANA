#!/usr/bin/env python

from __future__ import print_function
import os

npb_path = '/home/jyy/PerformanceModeling/IRstruct/APP/NPB3.3-MPI'
out_path = '/home/jyy/PerformanceModeling/IRstruct/APP/run-with-all-leafnode'
irs_path = '/home/jyy/PerformanceModeling/IRstruct/irstruct-with-all-leafnode'

#bms = ['bt', 'cg', 'ep', 'ft', 'mg', 'sp', 'lu', 'is'] #, 'dt']
bms = ['cg']
np1 = [8] #, 16, 32, 64]#, 128, 256] #, 512, 1024, 2048, 4096]
np2 = [9, 16, 36, 64]#, 121, 256] #, 529, 1024, 2025, 4096]
scale1 = ['C']#'A','B','C','D']
scale2 = ['C']#'A','B','C','D']

make_cmd = 'make %s NPROCS=%d CLASS=%s'
work_path = out_path + '/%s/%s/%d'
copy_bc_cmd = 'cp bin/%s ' + work_path
norm_cmd = 'opt -loop-simplify -mergereturn {0} -o n{0}.bc'
opt_cmd = 'opt -load ' + irs_path + '/irs.so -irs -if ' + irs_path + '/in.txt -of out.txt n{0}.bc -o i{0}.bc'
llc_cmd = 'llc -O0  -code-model=medium -filetype=obj i{0}.bc'
comp_cmd = '{0} -fno-omit-frame-pointer -fstack-protector-all i{1}.o -L{2} -lsampler -lstdc++ -rdynamic -o i{1}'
sub_cmd = 'LD_LIBRARY_PATH={2}:$LD_LIBRARY_PATH time mpirun -np {1} ./i{0}'
parse_cmd = '{0}/parse.sh ./i{1} SAMPLE*'
anly_cmd = '{0}/analyze {1}'

for bm in bms:
    if bm in ('bt', 'sp'):
        nps = np2
    else:
        nps = np1
    if bm in ('is', 'dt'):
        mpi_comp = 'mpicxx'
    else:
        mpi_comp = 'mpif90'
    if bm in ('is'):
	scales = scale2
    else:
	scales = scale1
    for scale in scales:
	for np in nps:
        	print('\n==========' + bm + ' ' + scale + ' ' + str(np) + '==========')

        	name = bm + '.' + scale + '.' + str(np)
        	os.chdir(npb_path)

	        print('make bc')
	        os.system(make_cmd % (bm, np, scale))

        	work_path_str = work_path % (scale, bm, np)
	        if os.path.exists(work_path_str):
        	    os.system('rm -rf ' + work_path_str)
	        os.system('mkdir -p ' + work_path_str)

        	os.system(copy_bc_cmd % (name, scale, bm, np))
	        os.chdir(work_path % (scale, bm, np))

        	print('running passes')
	        os.system(norm_cmd.format(name))
        	os.system(opt_cmd.format(name))

	        print('compiling to exe')
        	os.system(llc_cmd.format(name))
	        os.system(comp_cmd.format(mpi_comp, name, irs_path))


        	print('submit & run')
	        os.system(sub_cmd.format(name, np, irs_path))
		
		print('analyze')
		os.system(parse_cmd.format(irs_path,name))
		os.system(anly_cmd.format(irs_path, np))
