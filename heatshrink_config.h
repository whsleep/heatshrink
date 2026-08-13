#ifndef HEATSHRINK_CONFIG_H
#define HEATSHRINK_CONFIG_H

/* 是否启用依赖动态内存分配的功能？ */
#ifndef HEATSHRINK_DYNAMIC_ALLOC
#define HEATSHRINK_DYNAMIC_ALLOC 1
#endif

#if HEATSHRINK_DYNAMIC_ALLOC
    /* malloc/free 的可选替换实现 */
    #define HEATSHRINK_MALLOC(SZ) malloc(SZ)
    #define HEATSHRINK_FREE(P, SZ) free(P)
#else
    /* 静态配置所需的参数 */
    #define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 32
    #define HEATSHRINK_STATIC_WINDOW_BITS 8
    #define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4
#endif

/* 开启调试日志。 */
#define HEATSHRINK_DEBUGGING_LOGS 0

/* 使用索引来加速压缩。（这会额外占用内存空间。） */
#define HEATSHRINK_USE_INDEX 1

#endif
