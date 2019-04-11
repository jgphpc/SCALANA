//版本5.2
//3.6改良，先遍历所有函数，在动态组合
//4.0改良，主要是减少插装函数内容，所有事可以放到finalize中做，也可以输出日志。
//5.0 改良，每个节点的代码行号范围，并且只有CALL——INDIRECT和未拼接的FUNCTION插装
//5.1 如果按照函数栈搜索时遇到CALL_REC等，直接返回节点，并把函数栈清空。
//      函数栈如果在中途遇到一些节点先退出了，要把函数栈清空
//      CALL LOG太多，间接调用重复太多
//      加COMPUTING节点时，有重复现象，提前加以判断
//      多次遇到相同的间接调用时，只进行一次扩展拼接，提前加以判断
//      该版本动态拼接函数时，未拷贝copy一棵树，即所有调用该函数的地方都会累计起来。

//5.2 改良
//      1. ENTRY和EXIT的开始和结束进行PAPI_stop和 PAPI_start， 不统计插装的执行的cycle数，
//              BUG：解决FORTRAN的mainstart（调用main函数的函数）在init之前papi_stop的问题，判断不对该函数插装。
//      2. 增加了计时，统计非写入LOG的时间
//      3. 采用unw_backtrace的方法获取函数调用栈
//      4. LOOP的行号范围重新确定，代码解释IR的意思没有问题，但是IR中preheader指向的行号不对，应该是preheader指向的BB中跳转指令的行号。

#define __USE_GNU
#define _GNU_SOURCE


#include "IRStruct.h"
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cassert>
#include <chrono>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/time.h>
//#include <unwind.h>
//#include <asm-generic/sections.h>
//#include <pthread.h>
//#include <dlfcn.h>
#include <papi.h>
//#include <execinfo.h>
#include <libunwind.h>
#include <fstream>
#include <mpi.h>
using namespace std;



// these macros can be used for colorful output
#define TPRT_NOCOLOR "\033[0m"
#define TPRT_RED "\033[1;31m"
#define TPRT_GREEN "\033[1;32m"
#define TPRT_YELLOW "\033[1;33m"
#define TPRT_BLUE "\033[1;34m"
#define TPRT_MAGENTA "\033[1;35m"
#define TPRT_CYAN "\033[1;36m"
#define TPRT_REVERSE "\033[7m"

#define LOG_INFO(fmt, ...) fprintf(stderr,TPRT_GREEN fmt TPRT_NOCOLOR, __VA_ARGS__);
#define LOG_ERROR(fmt, ...) fprintf(stderr,TPRT_RED fmt TPRT_NOCOLOR, __VA_ARGS__);
#define LOG_WARN(fmt, ...) fprintf(stderr,TPRT_MAGENTA fmt TPRT_NOCOLOR, __VA_ARGS__);
#define LOG_LINE fprintf(stderr,TPRT_BLUE "line=%d\n" TPRT_NOCOLOR, __LINE__);
//

#define LOOP_0 CMD(0)
#define LOOP_1 LOOP_0 CMD(1)
#define LOOP_2 LOOP_1 CMD(2)
#define LOOP_3 LOOP_2 CMD(3)
#define LOOP_4 LOOP_3 CMD(4)
#define LOOP_5 LOOP_4 CMD(5)
#define LOOP_6 LOOP_5 CMD(6)
#define LOOP_7 LOOP_6 CMD(7)
#define LOOP_8 LOOP_7 CMD(8)
#define LOOP_9 LOOP_8 CMD(9)


#define LOOP_HELPER(n) LOOP_##n
#define LOOP(n) LOOP_HELPER(n)

#define DECLARE(n) buffer[n]=__builtin_return_address(n); printf("\nmy: %p\n", buffer[n]);

#define CMD(n) DECLARE(n)

#define LOOP_END 9

//


#define MODULE_INITED 1
#define PTHREAD_VERSION "GLIBC_2.3.2"


#define LOGSIZE 1000000
#define ADDRLOGSIZE 2000000
#define MAX_STACK_DEPTH 100
#define MAX_NODE_NUM 1000


#define DEFAULT_SAMPLE_COUNT  (10000) // 10ms
#define TRY(func, flag) \
{ \
	int retval = func;\
	if (retval != flag) LOG_ERROR("%s, ErrCode: %s\n", #func, PAPI_strerror(retval));\
}


typedef struct logstruct{
	char funcType = 0; // Y for entry , T for exit , L for LATCH , C for COMBINE
	//int childId = 0;
	int id = 0;
	int nodeType = 0; // 
	//long long time = 0;
}LOG;
/*
struct print_trace_unwind_state_t {
	size_t frames_to_skip;
	_Unwind_Word* current;
	_Unwind_Word* end;
};
*/

static int SAMPLE_COUNT ;
static int module_init = 0;
static int EventSet = PAPI_NULL;

static unsigned int addr_log_pointer = 0;
static void* address_log[ADDRLOGSIZE]={0};

static unsigned int logPointer = 0;
static LOG call_log[LOGSIZE]; //124MB

static int mpiRank;
static char* addr_threshold;

static unsigned int scannedPointer = 0;
static int scannedCallIndirctNode[MAX_NODE_NUM];

ofstream outputStream;
ofstream logStream;

static decltype(chrono::high_resolution_clock::now()) timer;
static double sumTime = 0;

static void startTimer() {
    timer = chrono::high_resolution_clock::now();
}

static void stopTimer() {
    auto end = chrono::high_resolution_clock::now();
    chrono::duration<double> duration = end - timer;
    sumTime += duration.count();
}

static void writelog(){
	stopTimer();
	LOG_INFO("WRITE %d LOG to TXT\n",logPointer);
	ofstream logStream((string("LOG") + to_string(mpiRank) + string(".TXT")), ios::app);

	if (logStream.fail()) {

		cerr << "Failed to open log file"<<endl;

		return;
	}
	for (int i = 0; i < logPointer; i++){
		//cerr << i <<endl;
		logStream << call_log[i].funcType << ' '
			<< call_log[i].id << ' '
			<< call_log[i].nodeType << '\n';
	}

	logPointer = 0;

	logStream.close();
	startTimer();
}


void ENTRY_FUNC( int id, int type) {

	//printf("ENTRY ID: %d , TYPE: %d\n",id,type);

	TRY(PAPI_stop(EventSet, NULL), PAPI_OK);

	int i = 0;
	for(i = 0; i <= scannedPointer ;i++){
		if (scannedCallIndirctNode[i] == id){
			TRY(PAPI_start(EventSet), PAPI_OK);
			return;
		}
	}	

	call_log[logPointer].funcType =  'Y' ;
	//call_log[logPointer].childId = childID;
	call_log[logPointer].id = id;
	call_log[logPointer].nodeType = type;
	//log[logPointer++].time = chrono::high_resolution_clock::now().time_since_epoch().count();

	logPointer++;

	if (logPointer >= LOGSIZE - 1){
		writelog();
	}

	TRY(PAPI_start(EventSet), PAPI_OK);

}

void EXIT_FUNC(int id, int type) {

	//printf("EXIT ID: %d , TYPE: %d\n",id,type);

	TRY(PAPI_stop(EventSet, NULL), PAPI_OK);

	//LOG_LINE;
	// Get rank after MPI_Init(_thread)
	if (type == 0 || type == 1){
		MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
		TRY(PAPI_start(EventSet), PAPI_OK);
		return ;
	}
	int i = 0;
	for(i = 0; i <= scannedPointer ;i++){
		if (scannedCallIndirctNode[i] == id){
			TRY(PAPI_start(EventSet), PAPI_OK);
			return;
		}
	}

	call_log[logPointer].funcType =  'T' ;
	//call_log[logPointer].childId = childID;
	call_log[logPointer].id = id;
	call_log[logPointer].nodeType = type;
	//call_log[logPointer++].time = chrono::high_resolution_clock::now().time_since_epoch().count();

	logPointer++;

	if(type == CALL_INDIRECT){
		scannedCallIndirctNode[scannedPointer] = id;
		scannedPointer = (scannedPointer >= MAX_NODE_NUM - 1 ) ? scannedPointer : scannedPointer++ ;
	}

	if (logPointer >= LOGSIZE - 1){
		writelog();
	}
	//LOG_LINE;

	TRY(PAPI_start(EventSet), PAPI_OK);

}

static void write_addr_log(){
	stopTimer();
	//ofstream outputStream((string("SAMPLE") + to_string(mpiRank) + string(".TXT")), ios_base::app);
	LOG_INFO("WRITE %d ADDR to TXT\n",addr_log_pointer);
	ofstream outputStream((string("SAMPLE") + to_string(mpiRank) + string(".TXT")), ios_base::app);

	if (!outputStream.good()) {
		cerr << "Failed to open sample file\n";
		return;
	}
	for (int i = 0; i < addr_log_pointer; i++){
		//cerr << i <<"\n";
		outputStream << address_log[i]<<'\n';
	}

	addr_log_pointer = 0;

	outputStream.close();
	startTimer();
}

void get_ebp(unsigned long *ebp)  
{  
	__asm__ __volatile__("mov %%ebp, %0 \r\n"  
			:"=m"(*ebp)  
			::"memory");  

}  

void get_esp(unsigned long *esp)
{
	__asm__ __volatile__("mov %%esp, %0 \r\n"
			:"=m"(*esp)
			::"memory");

}

int my_backtrace1(void **stack, int size, unsigned long ebp)  
{  
	int layer = 0;  
	while(layer < size && ebp != 0 && *(unsigned long*)ebp != 0 && *(unsigned long *)ebp != ebp)  
	{  
		printf("0:%p\n",(void*)(*(unsigned long *)(ebp+4))); 
		stack[layer++] = (void*)(*(unsigned long *)(ebp+4));  
		ebp = *(unsigned long*)ebp;  
	}  

	return layer;  
}  

#define STACKCALL __attribute__((regparm(1),noinline))
//void * STACKCALL getESP(void){
//        void *esp=NULL;
//        __asm__ __volatile__("mov %%rsp, %0;\n\t"
//                        :"=m"(esp)      /* 输出 */
//                        :      /* 输入 */
//                        :"memory");     /* 不受影响的寄存器 */
//        return (void *)(*esp);
//}


//#define STACKCALL __attribute__((regparm(1),noinline))
//void * STACKCALL getEBP(void){
//	void *ebp=NULL;
//	__asm__ __volatile__("mov %%rbp, %0;\n\t"
//			:"=m"(ebp)      /* 输出 */
//			:      /* 输入 */
//			:"memory");     /* 不受影响的寄存器 */
//	return (void *)(*ebp);
//}
/*int my_backtrace2(void **buffer,int size){
  printf("%d\n",__LINE__);
  int frame=0;
  void ** ebp;
  void **ret=NULL;
  unsigned long long func_frame_distance=0;
  if(buffer!=NULL && size >0)
  {
  printf("%d\n",__LINE__);
  ebp=getEBP();
  func_frame_distance=(unsigned long long)(*ebp) - (unsigned long long)ebp;
  printf("0:%p\n",ebp);			
  while(ebp&& frame<size
  &&(((void*)ebp > (void*)0x7ff000000000))//jump over .so lib
  &&(func_frame_distance>0))
  {
  ret=(ebp+1);
  if(*ebp < (void*)0x7ff000000000){
  ebp = (void**)(*ebp);
  break;
  }
  printf("1:%p\n",*ret);
  buffer[frame++]=*ret;
  ebp=(void**)(*ebp);
  func_frame_distance=(unsigned long long)(*ebp) - (unsigned long long)ebp;
  }
  while(ebp&& frame<size
  &&((  (void*)ebp > (void*)0x400b58 && (void*)ebp < (void*)addr_threshold))//assume function ebp more than 16M
  &&(func_frame_distance>0))
  {
  ret=ebp;
  printf("2:%p\n",*ret);
  buffer[frame++]=*ret;
  ebp=(void**)(*ebp);
  func_frame_distance=(unsigned long long)(*ebp) - (unsigned long long)ebp;
  }
  }
  return frame;
  }*/
/*
   void do_backtrace2()
   {
   unw_cursor_t    cursor;
   unw_context_t   context;

   unw_getcontext(&context);
   unw_init_local(&cursor, &context);

   while (unw_step(&cursor) > 0) {
   unw_word_t  offset, pc;
   char        fname[64];

   unw_get_reg(&cursor, UNW_REG_IP, &pc);

   fname[0] = '\0';
   (void) unw_get_proc_name(&cursor, fname, sizeof(fname), &offset);

   printf ("%p : (%s+0x%x) [%p]\n", pc, fname, offset, pc);
   }
   }
   */

int my_backtrace3(void **buffer,int size,unsigned long ebp,unsigned long esp){
	int frame=0;
	unsigned long *ret=NULL;
	printf("ebp:%p  esp:%p\n",ebp,esp);
	if(buffer!=NULL && size >0){
		while(ebp && ebp > esp &&ebp > (unsigned long)addr_threshold && frame<size){
			ret = (unsigned long*)(ebp+4);
			if(!(*ret))
				break;
			printf("1:%p\n",(void*)*ret);
			buffer[frame++]=(void*)*ret;
			ebp=*(unsigned long *)ebp;
		}
	}

	return frame;
}
/*
static _Unwind_Reason_Code print_trace_unwind_callback(::_Unwind_Context* context, void* arg) {
	// Note: do not write `::_Unwind_GetIP` because it is a macro on some platforms.
	// Use `_Unwind_GetIP` instead!
	print_trace_unwind_state_t* const state = reinterpret_cast<print_trace_unwind_state_t*>(arg);
	if (state->frames_to_skip) {
		--state->frames_to_skip;
		return _Unwind_GetIP(context) ? ::_URC_NO_REASON : ::_URC_END_OF_STACK;
	}

	*state->current = _Unwind_GetIP(context);

	++state->current;
	if (!*(state->current - 1) || state->current == state->end) {
		return ::_URC_END_OF_STACK;
	}

	return ::_URC_NO_REASON;
}
*/
void papi_handler(int EventSet, void *address, long_long overflow_vector, void *context){
	//LOG_LINE;
	TRY(PAPI_stop(EventSet, NULL), PAPI_OK);
	//LOG_LINE;
	void *buffer[MAX_STACK_DEPTH];
	unsigned int i, depth = 0;
	//memset(buffer, 0, sizeof(buffer));
	//unsigned long ebp = 0, esp = 0;
	//get_ebp(&ebp);
	//get_esp(&esp);
	depth = unw_backtrace(buffer, MAX_STACK_DEPTH);
	/*
	_Unwind_Word buffer[MAX_STACK_DEPTH];
	print_trace_unwind_state_t state;
	state.frames_to_skip = 0;
	state.current = buffer;
	state.end = buffer + MAX_STACK_DEPTH;

	::_Unwind_Backtrace(&print_trace_unwind_callback, &state);
	size_t depth = state.current - &buffer[0];
	size_t i = 0;
	
	*/
	//for (i = 0; i < depth; ++ i) {
	//	printf("my: %p\n", buffer[i]);
	//}
	//LOOP(LOOP_END);

	//unsigned long ebp = 0;
	//get_ebp(&ebp);
	//depth = my_backtrace1(buffer, MAX_STACK_DEPTH, ebp);
	//for(i = 0; i < depth; i++)
	//	printf("my: %p\n", buffer[i]);
	//printf("\n");


	for (i = 0; i < depth; ++ i)
	{

		//LOG_INFO("%08x\n",buffer[i]);
		if( (void*)buffer[i] != NULL && (char*)buffer[i] < addr_threshold ){ 
			address_log[addr_log_pointer] = (void*)(buffer[i]-2);
			addr_log_pointer++;
			//LOG_INFO("%08x\n",buffer[i]);
			//break;
		}

	}

	if(addr_log_pointer >= ADDRLOGSIZE-100){
		write_addr_log();
	}
	//LOG_LINE;

	TRY(PAPI_start(EventSet), PAPI_OK);
}


void static set_papi_overflow()
{
	EventSet = PAPI_NULL;
	TRY(PAPI_create_eventset(&EventSet), PAPI_OK);
	TRY(PAPI_add_event(EventSet, PAPI_TOT_CYC), PAPI_OK);
	TRY(PAPI_overflow(EventSet, PAPI_TOT_CYC, SAMPLE_COUNT, 0, papi_handler), PAPI_OK);
	TRY(PAPI_start(EventSet), PAPI_OK);
	//printf("set_papi_overflow() PAPI_start(EventSet), PAPI_OK\n");
}


static void init() __attribute__((constructor));
static void fini() __attribute__((destructor));

static void init() {
	if(module_init == MODULE_INITED) return ;
	module_init = MODULE_INITED;	
	addr_threshold =(char*)malloc( sizeof(char));
	//LOG_LINE;
	//LOGINFO("%08x -- %08x\n",_stext,_etext)
	//unsetenv("LD_PRELOAD");
	// PAPI setup for main thread
	char* str = getenv("SAMPLE_COUNT");
	SAMPLE_COUNT = (str ? atoi(str) : DEFAULT_SAMPLE_COUNT)*2500;
	LOG_INFO("SET sample interval to %d * 2500 cycles\n", SAMPLE_COUNT/2500);
	TRY(PAPI_library_init(PAPI_VER_CURRENT), PAPI_VER_CURRENT);
	//TRY(PAPI_thread_init(pthread_self), PAPI_OK);
	set_papi_overflow();
	startTimer();
}

static void fini(){
	//LOG_LINE;
	TRY(PAPI_stop(EventSet, NULL), PAPI_OK);
	stopTimer();
	if(mpiRank == 0){
        printf("TOTAL TIME is %.4f\n",sumTime);
        }
	if(logPointer >= 0 ){
		writelog();
	}
	if(addr_log_pointer >= 0){
		//cerr << "ssssssssssssss"<<endl;

		write_addr_log();
	}

	free(addr_threshold);
	//tree_print(sample_count_tree, thread_gid);

}
