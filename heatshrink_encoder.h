#ifndef HEATSHRINK_ENCODER_H
#define HEATSHRINK_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include "heatshrink_common.h"
#include "heatshrink_config.h"

typedef enum {
    HSER_SINK_OK,               /* 数据已送入输入缓冲区 */
    HSER_SINK_ERROR_NULL=-1,    /* NULL 参数 */
    HSER_SINK_ERROR_MISUSE=-2,  /* API 误用 */
} HSE_sink_res;

typedef enum {
    HSER_POLL_EMPTY,            /* 输入已耗尽 */
    HSER_POLL_MORE,             /* 再次 poll 以获取更多输出 */
    HSER_POLL_ERROR_NULL=-1,    /* NULL 参数 */
    HSER_POLL_ERROR_MISUSE=-2,  /* API 误用 */
} HSE_poll_res;

typedef enum {
    HSER_FINISH_DONE,           /* 编码完成 */
    HSER_FINISH_MORE,           /* 仍有输出未取出；调用 poll */
    HSER_FINISH_ERROR_NULL=-1,  /* NULL 参数 */
} HSE_finish_res;

#if HEATSHRINK_DYNAMIC_ALLOC
#define HEATSHRINK_ENCODER_WINDOW_BITS(HSE) \
    ((HSE)->window_sz2)
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(HSE) \
    ((HSE)->lookahead_sz2)
#define HEATSHRINK_ENCODER_INDEX(HSE) \
    ((HSE)->search_index)
struct hs_index {
    uint16_t size;
    int16_t index[];
};
#else
#define HEATSHRINK_ENCODER_WINDOW_BITS(_) \
    (HEATSHRINK_STATIC_WINDOW_BITS)
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(_) \
    (HEATSHRINK_STATIC_LOOKAHEAD_BITS)
#define HEATSHRINK_ENCODER_INDEX(HSE) \
    (&(HSE)->search_index)
struct hs_index {
    uint16_t size;
    int16_t index[2 << HEATSHRINK_STATIC_WINDOW_BITS];
};
#endif

typedef struct {
    uint16_t input_size;        /* 输入缓冲区中的字节数 */
    uint16_t match_scan_index;
    uint16_t match_length;
    uint16_t match_pos;
    uint16_t outgoing_bits;     /* 已入队的待输出比特 */
    uint8_t outgoing_bits_count;
    uint8_t flags;
    uint8_t state;              /* 当前状态机状态 */
    uint8_t current_byte;       /* 当前输出字节 */
    uint8_t bit_index;          /* 当前比特位置 */
#if HEATSHRINK_DYNAMIC_ALLOC
    uint8_t window_sz2;         /* 窗口大小为 2^n */
    uint8_t lookahead_sz2;      /* 前向匹配大小为 2^n */
#if HEATSHRINK_USE_INDEX
    struct hs_index *search_index;
#endif
    /* 输入缓冲区 / 用于扩展的滑动窗口 */
    uint8_t buffer[];
#else
    #if HEATSHRINK_USE_INDEX
        struct hs_index search_index;
    #endif
    /* 输入缓冲区 / 用于扩展的滑动窗口 */
    uint8_t buffer[2 << HEATSHRINK_ENCODER_WINDOW_BITS(_)];
#endif
} heatshrink_encoder;

#if HEATSHRINK_DYNAMIC_ALLOC
/* 分配新的 encoder 结构体及其缓冲区。出错返回 NULL。 */
heatshrink_encoder *heatshrink_encoder_alloc(uint8_t window_sz2,
    uint8_t lookahead_sz2);

/* 释放 encoder。 */
void heatshrink_encoder_free(heatshrink_encoder *hse);
#endif

/* 重置 encoder。 */
void heatshrink_encoder_reset(heatshrink_encoder *hse);

/* 将 IN_BUF 中最多 SIZE 字节送入 encoder。
 * *INPUT_SIZE 会设置为实际送入的字节数（缓冲区可能已满）。 */
HSE_sink_res heatshrink_encoder_sink(heatshrink_encoder *hse,
    uint8_t *in_buf, size_t size, size_t *input_size);

/* 从 encoder 轮询输出，最多把 OUT_BUF_SIZE 字节拷入 OUT_BUF
 * （*OUTPUT_SIZE 设为实际拷贝的字节数）。 */
HSE_poll_res heatshrink_encoder_poll(heatshrink_encoder *hse,
    uint8_t *out_buf, size_t out_buf_size, size_t *output_size);

/* 通知 encoder 输入流已结束。
 * 若返回 HSER_FINISH_MORE，说明还有输出未取出，需反复调用
 * heatshrink_encoder_poll。 */
HSE_finish_res heatshrink_encoder_finish(heatshrink_encoder *hse);

#endif
