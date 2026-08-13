#ifndef HEATSHRINK_DECODER_H
#define HEATSHRINK_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include "heatshrink_common.h"
#include "heatshrink_config.h"

typedef enum {
    HSDR_SINK_OK,               /* 数据已送入，可进行 poll */
    HSDR_SINK_FULL,             /* 内部缓冲区空间不足 */
    HSDR_SINK_ERROR_NULL=-1,    /* NULL 参数 */
} HSD_sink_res;

typedef enum {
    HSDR_POLL_EMPTY,            /* 输入已耗尽 */
    HSDR_POLL_MORE,             /* 仍有数据未处理，用新的输出缓冲区再次调用 */
    HSDR_POLL_ERROR_NULL=-1,    /* NULL 参数 */
    HSDR_POLL_ERROR_UNKNOWN=-2,
} HSD_poll_res;

typedef enum {
    HSDR_FINISH_DONE,           /* 输出已完成 */
    HSDR_FINISH_MORE,           /* 仍有输出未取出 */
    HSDR_FINISH_ERROR_NULL=-1,  /* NULL 参数 */
} HSD_finish_res;

#if HEATSHRINK_DYNAMIC_ALLOC
#define HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(BUF) \
    ((BUF)->input_buffer_size)
#define HEATSHRINK_DECODER_WINDOW_BITS(BUF) \
    ((BUF)->window_sz2)
#define HEATSHRINK_DECODER_LOOKAHEAD_BITS(BUF) \
    ((BUF)->lookahead_sz2)
#else
#define HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(_) \
    HEATSHRINK_STATIC_INPUT_BUFFER_SIZE
#define HEATSHRINK_DECODER_WINDOW_BITS(_) \
    (HEATSHRINK_STATIC_WINDOW_BITS)
#define HEATSHRINK_DECODER_LOOKAHEAD_BITS(BUF) \
    (HEATSHRINK_STATIC_LOOKAHEAD_BITS)
#endif

typedef struct {
    uint16_t input_size;        /* 输入缓冲区中的字节数 */
    uint16_t input_index;       /* 下一个待处理输入字节的偏移 */
    uint16_t output_count;      /* 还需输出的字节数 */
    uint16_t output_index;      /* 待输出字节的索引 */
    uint16_t head_index;        /* 窗口缓冲区的头部 */
    uint8_t state;              /* 当前状态机状态 */
    uint8_t current_byte;       /* 当前输入字节 */
    uint8_t bit_index;          /* 当前比特位置 */

#if HEATSHRINK_DYNAMIC_ALLOC
    /* 仅在动态分配时使用的字段。 */
    uint8_t window_sz2;         /* 窗口缓冲区位数 */
    uint8_t lookahead_sz2;      /* 前向匹配位数 */
    uint16_t input_buffer_size; /* 输入缓冲区大小 */

    /* 先是输入缓冲区，然后是扩展窗口缓冲区 */
    uint8_t buffers[];
#else
    /* 先是输入缓冲区，然后是扩展窗口缓冲区 */
    uint8_t buffers[(1 << HEATSHRINK_DECODER_WINDOW_BITS(_))
        + HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(_)];
#endif
} heatshrink_decoder;

#if HEATSHRINK_DYNAMIC_ALLOC
/* 分配一个 decoder：输入缓冲区为 INPUT_BUFFER_SIZE 字节，
 * 扩展缓冲区为 2^WINDOW_SZ2，前向匹配大小为 2^lookahead_sz2。
 * （窗口缓冲区和前向匹配大小必须与压缩数据时使用的设置一致。）
 * 出错返回 NULL。 */
heatshrink_decoder *heatshrink_decoder_alloc(uint16_t input_buffer_size,
    uint8_t expansion_buffer_sz2, uint8_t lookahead_sz2);

/* 释放 decoder。 */
void heatshrink_decoder_free(heatshrink_decoder *hsd);
#endif

/* 重置 decoder。 */
void heatshrink_decoder_reset(heatshrink_decoder *hsd);

/* 将 IN_BUF 中最多 SIZE 字节送入 decoder。
 * *INPUT_SIZE 会设置为实际送入的字节数（缓冲区可能已满）。 */
HSD_sink_res heatshrink_decoder_sink(heatshrink_decoder *hsd,
    uint8_t *in_buf, size_t size, size_t *input_size);

/* 从 decoder 轮询输出，最多把 OUT_BUF_SIZE 字节拷入 OUT_BUF
 * （*OUTPUT_SIZE 设为实际拷贝的字节数）。 */
HSD_poll_res heatshrink_decoder_poll(heatshrink_decoder *hsd,
    uint8_t *out_buf, size_t out_buf_size, size_t *output_size);

/* 通知 decoder 输入流已结束。
 * 若返回 HSDR_FINISH_MORE，说明还有输出未取出，需反复调用
 * heatshrink_decoder_poll。 */
HSD_finish_res heatshrink_decoder_finish(heatshrink_decoder *hsd);

#endif
