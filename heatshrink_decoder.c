#include <stdlib.h>
#include <string.h>
#include "heatshrink_decoder.h"

/* 轮询状态机的状态。 */
typedef enum {
    HSDS_TAG_BIT,               /* 标记位 */
    HSDS_YIELD_LITERAL,         /* 准备输出字面量字节 */
    HSDS_BACKREF_INDEX_MSB,     /* 索引的高字节 */
    HSDS_BACKREF_INDEX_LSB,     /* 索引的低字节 */
    HSDS_BACKREF_COUNT_MSB,     /* 计数的高字节 */
    HSDS_BACKREF_COUNT_LSB,     /* 计数的低字节 */
    HSDS_YIELD_BACKREF,         /* 准备输出反向引用 */
} HSD_state;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "tag_bit",
    "yield_literal",
    "backref_index_msb",
    "backref_index_lsb",
    "backref_count_msb",
    "backref_count_lsb",
    "yield_backref",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

typedef struct {
    uint8_t *buf;               /* 输出缓冲区 */
    size_t buf_size;            /* 缓冲区大小 */
    size_t *output_size;        /* 到目前为止已写入缓冲区的字节数 */
} output_info;

#define NO_BITS ((uint16_t)-1)

/* 前置声明。 */
static uint16_t get_bits(heatshrink_decoder *hsd, uint8_t count);
static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_decoder *heatshrink_decoder_alloc(uint16_t input_buffer_size,
                                             uint8_t window_sz2,
                                             uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (input_buffer_size == 0) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 >= window_sz2)) {
        return NULL;
    }
    size_t buffers_sz = (1 << window_sz2) + input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    heatshrink_decoder *hsd = HEATSHRINK_MALLOC(sz);
    if (hsd == NULL) { return NULL; }
    hsd->input_buffer_size = input_buffer_size;
    hsd->window_sz2 = window_sz2;
    hsd->lookahead_sz2 = lookahead_sz2;
    heatshrink_decoder_reset(hsd);
    LOG("-- allocated decoder with buffer size of %zu (%zu + %u + %u)\n",
        sz, sizeof(heatshrink_decoder), (1 << window_sz2), input_buffer_size);
    return hsd;
}

void heatshrink_decoder_free(heatshrink_decoder *hsd) {
    size_t buffers_sz = (1 << hsd->window_sz2) + hsd->input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    HEATSHRINK_FREE(hsd, sz);
    (void)sz;   /* free 可能用不到该值 */
}
#endif

void heatshrink_decoder_reset(heatshrink_decoder *hsd) {
    size_t buf_sz = 1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd);
    size_t input_sz = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd);
    memset(hsd->buffers, 0, buf_sz + input_sz);
    hsd->state = HSDS_TAG_BIT;
    hsd->input_size = 0;
    hsd->input_index = 0;
    hsd->bit_index = 0x00;
    hsd->current_byte = 0x00;
    hsd->output_count = 0;
    hsd->output_index = 0;
    hsd->head_index = 0;
}

/* 把 SIZE 字节拷入 decoder 的输入缓冲区（如果放得下的话）。 */
HSD_sink_res heatshrink_decoder_sink(heatshrink_decoder *hsd,
        uint8_t *in_buf, size_t size, size_t *input_size) {
    if ((hsd == NULL) || (in_buf == NULL) || (input_size == NULL)) {
        return HSDR_SINK_ERROR_NULL;
    }

    size_t rem = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd) - hsd->input_size;
    if (rem == 0) {
        *input_size = 0;
        return HSDR_SINK_FULL;
    }

    size = rem < size ? rem : size;
    LOG("-- sinking %zd bytes\n", size);
    /* 拷入输入缓冲区（位于 buffers 头部） */
    memcpy(&hsd->buffers[hsd->input_size], in_buf, size);
    hsd->input_size += size;
    *input_size = size;
    return HSDR_SINK_OK;
}


/*****************
 * 解压          *
 *****************/

#define BACKREF_COUNT_BITS(HSD) (HEATSHRINK_DECODER_LOOKAHEAD_BITS(HSD))
#define BACKREF_INDEX_BITS(HSD) (HEATSHRINK_DECODER_WINDOW_BITS(HSD))

// 状态
static HSD_state st_tag_bit(heatshrink_decoder *hsd);
static HSD_state st_yield_literal(heatshrink_decoder *hsd,
    output_info *oi);
static HSD_state st_backref_index_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd);
static HSD_state st_yield_backref(heatshrink_decoder *hsd,
    output_info *oi);

HSD_poll_res heatshrink_decoder_poll(heatshrink_decoder *hsd,
        uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
    if ((hsd == NULL) || (out_buf == NULL) || (output_size == NULL)) {
        return HSDR_POLL_ERROR_NULL;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- poll, state is %d (%s), input_size %d\n",
            hsd->state, state_names[hsd->state], hsd->input_size);
        uint8_t in_state = hsd->state;
        switch (in_state) {
        case HSDS_TAG_BIT:
            hsd->state = st_tag_bit(hsd);
            break;
        case HSDS_YIELD_LITERAL:
            hsd->state = st_yield_literal(hsd, &oi);
            break;
        case HSDS_BACKREF_INDEX_MSB:
            hsd->state = st_backref_index_msb(hsd);
            break;
        case HSDS_BACKREF_INDEX_LSB:
            hsd->state = st_backref_index_lsb(hsd);
            break;
        case HSDS_BACKREF_COUNT_MSB:
            hsd->state = st_backref_count_msb(hsd);
            break;
        case HSDS_BACKREF_COUNT_LSB:
            hsd->state = st_backref_count_lsb(hsd);
            break;
        case HSDS_YIELD_BACKREF:
            hsd->state = st_yield_backref(hsd, &oi);
            break;
        default:
            return HSDR_POLL_ERROR_UNKNOWN;
        }

        /* 若当前状态无法继续推进，检查输入或输出缓冲区是否耗尽。 */
        if (hsd->state == in_state) {
            if (*output_size == out_buf_size) { return HSDR_POLL_MORE; }
            return HSDR_POLL_EMPTY;
        }
    }
}

static HSD_state st_tag_bit(heatshrink_decoder *hsd) {
    uint32_t bits = get_bits(hsd, 1);  // 读取标记位
    if (bits == NO_BITS) {
        return HSDS_TAG_BIT;
    } else if (bits) {
        return HSDS_YIELD_LITERAL;
    } else if (HEATSHRINK_DECODER_WINDOW_BITS(hsd) > 8) {
        return HSDS_BACKREF_INDEX_MSB;
    } else {
        hsd->output_index = 0;
        return HSDS_BACKREF_INDEX_LSB;
    }
}

static HSD_state st_yield_literal(heatshrink_decoder *hsd,
        output_info *oi) {
    /* 从窗口缓冲区输出一段重复数据，并把它（再次）写回窗口缓冲区。
     * （注意重复部分可能包含自身。） */
    if (*oi->output_size < oi->buf_size) {
        uint16_t byte = get_bits(hsd, 8);
        if (byte == NO_BITS) { return HSDS_YIELD_LITERAL; } /* 输入不足 */
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd))  - 1;
        uint8_t c = byte & 0xFF;
        LOG("-- emitting literal byte 0x%02x ('%c')\n", c, isprint(c) ? c : '.');
        buf[hsd->head_index++ & mask] = c;
        push_byte(hsd, oi, c);
        return HSDS_TAG_BIT;
    } else {
        return HSDS_YIELD_LITERAL;
    }
}

static HSD_state st_backref_index_msb(heatshrink_decoder *hsd) {
    uint8_t bit_ct = BACKREF_INDEX_BITS(hsd);
    ASSERT(bit_ct > 8);
    uint16_t bits = get_bits(hsd, bit_ct - 8);
    LOG("-- backref index (msb), got 0x%04x (+1)\n", bits);
    if (bits == NO_BITS) { return HSDS_BACKREF_INDEX_MSB; }
    hsd->output_index = bits << 8;
    return HSDS_BACKREF_INDEX_LSB;
}

static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd) {
    uint8_t bit_ct = BACKREF_INDEX_BITS(hsd);
    uint16_t bits = get_bits(hsd, bit_ct < 8 ? bit_ct : 8);
    LOG("-- backref index (lsb), got 0x%04x (+1)\n", bits);
    if (bits == NO_BITS) { return HSDS_BACKREF_INDEX_LSB; }
    hsd->output_index |= bits;
    hsd->output_index++;
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    hsd->output_count = 0;
    return (br_bit_ct > 8) ? HSDS_BACKREF_COUNT_MSB : HSDS_BACKREF_COUNT_LSB;
}

static HSD_state st_backref_count_msb(heatshrink_decoder *hsd) {
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    ASSERT(br_bit_ct > 8);
    uint16_t bits = get_bits(hsd, br_bit_ct - 8);
    LOG("-- backref count (msb), got 0x%04x (+1)\n", bits);
    if (bits == NO_BITS) { return HSDS_BACKREF_COUNT_MSB; }
    hsd->output_count = bits << 8;
    return HSDS_BACKREF_COUNT_LSB;
}

static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd) {
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    uint16_t bits = get_bits(hsd, br_bit_ct < 8 ? br_bit_ct : 8);
    LOG("-- backref count (lsb), got 0x%04x (+1)\n", bits);
    if (bits == NO_BITS) { return HSDS_BACKREF_COUNT_LSB; }
    hsd->output_count |= bits;
    hsd->output_count++;
    return HSDS_YIELD_BACKREF;
}

static HSD_state st_yield_backref(heatshrink_decoder *hsd,
        output_info *oi) {
    size_t count = oi->buf_size - *oi->output_size;
    if (count > 0) {
        size_t i = 0;
        if (hsd->output_count < count) count = hsd->output_count;
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd)) - 1;
        uint16_t neg_offset = hsd->output_index;
        LOG("-- emitting %zu bytes from -%u bytes back\n", count, neg_offset);
        ASSERT(neg_offset <= mask + 1);
        ASSERT(count <= (size_t)(1 << BACKREF_COUNT_BITS(hsd)));

        for (i=0; i<count; i++) {
            uint8_t c = buf[(hsd->head_index - neg_offset) & mask];
            push_byte(hsd, oi, c);
            buf[hsd->head_index & mask] = c;
            hsd->head_index++;
            LOG("  -- ++ 0x%02x\n", c);
        }
        hsd->output_count -= count;
        if (hsd->output_count == 0) { return HSDS_TAG_BIT; }
    }
    return HSDS_YIELD_BACKREF;
}

/* 从输入缓冲区读取接下来 COUNT 个比特，并保存中间进度。
 * 输入结束或请求超过 15 个比特时返回 NO_BITS。 */
static uint16_t get_bits(heatshrink_decoder *hsd, uint8_t count) {
    uint16_t accumulator = 0;
    int i = 0;
    if (count > 15) { return NO_BITS; }
    LOG("-- popping %u bit(s)\n", count);

    /* 如果凑不齐 COUNT 个比特，立即挂起，因为我们没有记录
     * 挂起前已累积了多少个比特。 */
    if (hsd->input_size == 0) {
        if (hsd->bit_index < (1 << (count - 1))) { return NO_BITS; }
    }

    for (i = 0; i < count; i++) {
        if (hsd->bit_index == 0x00) {
            if (hsd->input_size == 0) {
                LOG("  -- out of bits, suspending w/ accumulator of %u (0x%02x)\n",
                    accumulator, accumulator);
                return NO_BITS;
            }
            hsd->current_byte = hsd->buffers[hsd->input_index++];
            LOG("  -- pulled byte 0x%02x\n", hsd->current_byte);
            if (hsd->input_index == hsd->input_size) {
                hsd->input_index = 0; /* 输入已耗尽 */
                hsd->input_size = 0;
            }
            hsd->bit_index = 0x80;
        }
        accumulator <<= 1;
        if (hsd->current_byte & hsd->bit_index) {
            accumulator |= 0x01;
            if (0) {
                LOG("  -- got 1, accumulator 0x%04x, bit_index 0x%02x\n",
                accumulator, hsd->bit_index);
            }
        } else {
            if (0) {
                LOG("  -- got 0, accumulator 0x%04x, bit_index 0x%02x\n",
                accumulator, hsd->bit_index);
            }
        }
        hsd->bit_index >>= 1;
    }

    if (count > 1) { LOG("  -- accumulated %08x\n", accumulator); }
    return accumulator;
}

HSD_finish_res heatshrink_decoder_finish(heatshrink_decoder *hsd) {
    if (hsd == NULL) { return HSDR_FINISH_ERROR_NULL; }
    switch (hsd->state) {
    case HSDS_TAG_BIT:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    /* 若在没有输入的情况下想结束，却处于这些状态，是因为最后一个
     * 字节的 0 填充看起来像一个反向引用标记位，后面跟着全 0 的
     * 索引和计数字段。 */
    case HSDS_BACKREF_INDEX_LSB:
    case HSDS_BACKREF_INDEX_MSB:
    case HSDS_BACKREF_COUNT_LSB:
    case HSDS_BACKREF_COUNT_MSB:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    /* 若输出流被 0xFF 填充（可能是位于 flash 中导致），也需显式检查
     * 输入大小，而不是徒劳地返回 MORE、poll 时却输出 0 字节。 */
    case HSDS_YIELD_LITERAL:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;

    default:
        return HSDR_FINISH_MORE;
    }
}

static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte) {
    LOG(" -- pushing byte: 0x%02x ('%c')\n", byte, isprint(byte) ? byte : '.');
    oi->buf[(*oi->output_size)++] = byte;
    (void)hsd;
}
