#ifndef HEATSHRINK_ENCODER_H
#define HEATSHRINK_ENCODER_H

#include "heatshrink_common.h"
#include "heatshrink_config.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
  HSER_SINK_OK,                /* 数据已送入输入缓冲区 */
  HSER_SINK_ERROR_NULL = -1,   /* NULL 参数 */
  HSER_SINK_ERROR_MISUSE = -2, /* API 误用 */
} HSE_sink_res;

typedef enum {
  HSER_POLL_EMPTY,             /* 输入已耗尽 */
  HSER_POLL_MORE,              /* 再次 poll 以获取更多输出 */
  HSER_POLL_ERROR_NULL = -1,   /* NULL 参数 */
  HSER_POLL_ERROR_MISUSE = -2, /* API 误用 */
} HSE_poll_res;

typedef enum {
  HSER_FINISH_DONE,            /* 编码完成 */
  HSER_FINISH_MORE,            /* 仍有输出未取出；调用 poll */
  HSER_FINISH_ERROR_NULL = -1, /* NULL 参数 */
} HSE_finish_res;

#if HEATSHRINK_DYNAMIC_ALLOC
// 获取编码器的输入缓冲区大小（字节数）
#define HEATSHRINK_ENCODER_WINDOW_BITS(HSE) ((HSE)->window_sz2)
// 获取编码器的前向匹配缓冲区大小（字节数）
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(HSE) ((HSE)->lookahead_sz2)
// 获取编码器的索引
#define HEATSHRINK_ENCODER_INDEX(HSE) ((HSE)->search_index)
struct hs_index {
  uint16_t size;
  int16_t index[];
};
#else
#define HEATSHRINK_ENCODER_WINDOW_BITS(_) (HEATSHRINK_STATIC_WINDOW_BITS)
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(_) (HEATSHRINK_STATIC_LOOKAHEAD_BITS)
#define HEATSHRINK_ENCODER_INDEX(HSE) (&(HSE)->search_index)
struct hs_index {
  uint16_t size;
  int16_t index[2 << HEATSHRINK_STATIC_WINDOW_BITS];
};
#endif

/*
 * 编码器状态结构体。
 *
 * 内部缓冲区 buffer 一分为二：
 *   buffer[0 .. 窗口大小-1]           —— backlog 区：上一批已压缩完的数据，
 *                                       作为历史窗口供查找反向引用
 *   buffer[窗口大小 .. 2*窗口大小-1]   —— input 区：当前待压缩的输入
 *
 * 状态机按 HSE_state 枚举逐步推进，把 input 区的内容压成
 * “字面量 / 反向引用”两种元素输出。
 */
typedef struct {
  /* ---- 输入相关 ---- */
  uint16_t input_size; /* 当前已送入 input 区的字节数（0 ~ 窗口大小） */
  uint16_t match_scan_index; /* 匹配搜索游标：已扫到 input 区内的哪个下标 */
  uint16_t match_length; /* 当前找到的最长匹配长度（一次能替代几个字节） */
  uint16_t match_pos; /* 匹配处（更早出现的那份数据）在缓冲区中的下标；
                       * 输出反向引用偏移时以 (match_pos - 1) 编码 */
  /* ---- 输出比特缓冲 ---- */
  uint16_t outgoing_bits; /* 已排队待输出的比特（反向引用的偏移或长度） */
  uint8_t outgoing_bits_count; /* outgoing_bits 中有效的比特个数 */
  uint8_t current_byte; /* 正在逐比特拼装、准备写入输出的那个字节 */
  uint8_t bit_index; /* current_byte 中下一个要写入的位掩码（0x80 → 0x01） */
  /* ---- 状态与控制 ---- */
  uint8_t state; /* 当前状态机状态，取值见 HSE_state 枚举 */
  uint8_t flags; /* 控制标志位（bit0 = FLAG_IS_FINISHING 正在收尾） */
#if HEATSHRINK_DYNAMIC_ALLOC
  /* ---- 动态分配：运行时可指定参数 ---- */
  uint8_t window_sz2; /* 窗口大小 = 2^n 字节（反向引用最多往回引多远） */
  uint8_t lookahead_sz2; /* 最长匹配 = 2^n 字节（一次反向引用最多替代多长） */
#if HEATSHRINK_USE_INDEX
  struct hs_index
      *search_index; /* 指向哈希索引，加速匹配查找（见 do_indexing） */
#endif
  /* 输入/历史滑动窗口，总长 2*窗口大小（见上方布局说明） */
  uint8_t buffer[];
#else
  /* ---- 静态分配：参数在编译期定死 ---- */
#if HEATSHRINK_USE_INDEX
  struct hs_index search_index; /* 内嵌哈希索引（大小在编译期固定） */
#endif
  /* 输入/历史滑动窗口，总长 2*窗口大小，编译期固定 */
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
HSE_sink_res heatshrink_encoder_sink(heatshrink_encoder *hse, uint8_t *in_buf,
                                     size_t size, size_t *input_size);

/* 从 encoder 轮询输出，最多把 OUT_BUF_SIZE 字节拷入 OUT_BUF
 * （*OUTPUT_SIZE 设为实际拷贝的字节数）。 */
HSE_poll_res heatshrink_encoder_poll(heatshrink_encoder *hse, uint8_t *out_buf,
                                     size_t out_buf_size, size_t *output_size);

/* 通知 encoder 输入流已结束。
 * 若返回 HSER_FINISH_MORE，说明还有输出未取出，需反复调用
 * heatshrink_encoder_poll。 */
HSE_finish_res heatshrink_encoder_finish(heatshrink_encoder *hse);

#endif
