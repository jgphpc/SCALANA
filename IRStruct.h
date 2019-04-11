//5.2 改良
//      1. ENTRY和EXIT的开始和结束进行PAPI_stop和 PAPI_start， 不统计插装的执行的cycle数，
//              BUG：解决FORTRAN的mainstart（调用main函数的函数）在init之前papi_stop的问题，判断不对该函数插装。
//      2. 增加了计时，统计非写入LOG的时间
//      3. 采用unw_backtrace的方法获取函数调用栈
//      4. LOOP的行号范围重新确定，代码解释IR的意思没有问题，但是IR中preheader指向的行号不对，应该是preheader指向的BB中跳转指令的行号。
#ifndef IRSTRUCT_H
#define IRSTRUCT_H

#define ENTRY_FUNC entryPoint
#define EXIT_FUNC exitPoint
#define LATCH_FUNC latchPoint
#define COMBINE_FUNC combinePoint

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ENTRY_FUNC_NAME TOSTRING(ENTRY_FUNC)
#define EXIT_FUNC_NAME TOSTRING(EXIT_FUNC)
#define LATCH_FUNC_NAME TOSTRING(LATCH_FUNC)
#define COMBINE_FUNC_NAME TOSTRING(COMBINE_FUNC)

#ifdef __cplusplus
extern "C" {
#endif

enum NodeType {
    COMPUTING = -9,
    COMBINE = -8,
    CALL_INDIRECT = -7,
    CALL_REC = -6,
    CALL = -5,
    FUNCTION = -4,
    COMPOUND = -3,
    BRANCH = -2,
    LOOP = -1,
};

//void ENTRY_FUNC(int childID, int id, int type);
void ENTRY_FUNC(int id, int type);

//void EXIT_FUNC(int childID, int id, int type);
void EXIT_FUNC(int id, int type);

//void LATCH_FUNC(int childID, int id, int type);

//void COMBINE_FUNC(int childID, int id, int type);

#ifdef __cplusplus
}
#endif

#endif // IRSTRUCT_H
