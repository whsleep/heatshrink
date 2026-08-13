#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "heatshrink_encoder.h"

typedef enum {
    HSES_NOT_FULL,              /* 输入缓冲区尚未填满 */
    HSES_FILLED,                /* 缓冲区已满 */
    HSES_SEARCH,                /* 正在查找匹配模式 */
    HSES_YIELD_TAG_BIT,         /* 输出标记位 */
    HSES_YIELD_LITERAL,         /* 输出字面量字节 */
    HSES_YIELD_BR_INDEX,        /* 输出反向引用索引 */
    HSES_YIELD_BR_LENGTH,       /* 输出反向引用长度 */
    HSES_SAVE_BACKLOG,          /* 把缓冲区拷贝到 backlog */
    HSES_FLUSH_BITS,            /* 冲刷比特缓冲区 */
    HSES_DONE,                  /* 完成 */
} HSE_state;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "not_full",
    "filled",
    "search",
    "yield_tag_bit",
    "yield_literal",
    "yield_br_index",
    "yield_br_length",
    "save_backlog",
    "flush_bits",
    "done",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

// 编码器标志位
enum {
    FLAG_IS_FINISHING = 0x01,
};

typedef struct {
    uint8_t *buf;               /* 输出缓冲区 */
    size_t buf_size;            /* 缓冲区大小 */
    size_t *output_size;        /* 到目前为止已写入缓冲区的字节数 */
} output_info;

#define MATCH_NOT_FOUND ((uint16_t)-1)

static uint16_t get_input_offset(heatshrink_encoder *hse);
static uint16_t get_input_buffer_size(heatshrink_encoder *hse);
static uint16_t get_lookahead_size(heatshrink_encoder *hse);
static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag);
static int can_take_byte(output_info *oi);
static int is_finishing(heatshrink_encoder *hse);
static void save_backlog(heatshrink_encoder *hse);

/* 把 COUNT（最多 8）个比特写入还有空间的输出缓冲区。 */
static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
    output_info *oi);
static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi);
static void push_literal_byte(heatshrink_encoder *hse, output_info *oi);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_encoder *heatshrink_encoder_alloc(uint8_t window_sz2,
        uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 >= window_sz2)) {
        return NULL;
    }

    /* 注意：缓冲区大小取 2 倍窗口大小，因为缓冲区需要容纳
     * (1 << window_sz2) 字节的当前输入，以及额外
     * (1 << window_sz2) 字节的上一份输入（用于扫描查找
     * 有用的反向引用）。 */
    size_t buf_sz = (2 << window_sz2);

    heatshrink_encoder *hse = HEATSHRINK_MALLOC(sizeof(*hse) + buf_sz);
    if (hse == NULL) { return NULL; }
    hse->window_sz2 = window_sz2;
    hse->lookahead_sz2 = lookahead_sz2;
    heatshrink_encoder_reset(hse);

#if HEATSHRINK_USE_INDEX
    size_t index_sz = buf_sz*sizeof(uint16_t);
    hse->search_index = HEATSHRINK_MALLOC(index_sz + sizeof(struct hs_index));
    if (hse->search_index == NULL) {
        HEATSHRINK_FREE(hse, sizeof(*hse) + buf_sz);
        return NULL;
    }
    hse->search_index->size = index_sz;
#endif

    LOG("-- allocated encoder with buffer size of %zu (%u byte input size)\n",
        buf_sz, get_input_buffer_size(hse));
    return hse;
}

void heatshrink_encoder_free(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
#if HEATSHRINK_USE_INDEX
    size_t index_sz = sizeof(struct hs_index) + hse->search_index->size;
    HEATSHRINK_FREE(hse->search_index, index_sz);
    (void)index_sz;
#endif
    HEATSHRINK_FREE(hse, sizeof(heatshrink_encoder) + buf_sz);
    (void)buf_sz;
}
#endif

void heatshrink_encoder_reset(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
    memset(hse->buffer, 0, buf_sz);
    hse->input_size = 0;
    hse->state = HSES_NOT_FULL;
    hse->match_scan_index = 0;
    hse->flags = 0;
    hse->bit_index = 0x80;
    hse->current_byte = 0x00;
    hse->match_length = 0;

    hse->outgoing_bits = 0x0000;
    hse->outgoing_bits_count = 0;

    #ifdef LOOP_DETECT
    hse->loop_detect = (uint32_t)-1;
    #endif
}

HSE_sink_res heatshrink_encoder_sink(heatshrink_encoder *hse,
        uint8_t *in_buf, size_t size, size_t *input_size) {
    if ((hse == NULL) || (in_buf == NULL) || (input_size == NULL)) {
        return HSER_SINK_ERROR_NULL;
    }

    /* 在已声明内容结束后又继续送入内容，这是误用 */
    if (is_finishing(hse)) { return HSER_SINK_ERROR_MISUSE; }

    /* 在处理完成之前又继续送入内容 */
    if (hse->state != HSES_NOT_FULL) { return HSER_SINK_ERROR_MISUSE; }

    uint16_t write_offset = get_input_offset(hse) + hse->input_size;
    uint16_t ibs = get_input_buffer_size(hse);
    uint16_t rem = ibs - hse->input_size;
    uint16_t cp_sz = rem < size ? rem : size;

    memcpy(&hse->buffer[write_offset], in_buf, cp_sz);
    *input_size = cp_sz;
    hse->input_size += cp_sz;

    LOG("-- sunk %u bytes (of %zu) into encoder at %d, input buffer now has %u\n",
        cp_sz, size, write_offset, hse->input_size);
    if (cp_sz == rem) {
        LOG("-- internal buffer is now full\n");
        hse->state = HSES_FILLED;
    }

    return HSER_SINK_OK;
}


/***************
 * 压缩        *
 ***************/

static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
    uint16_t end, const uint16_t maxlen, uint16_t *match_length);
static void do_indexing(heatshrink_encoder *hse);

static HSE_state st_step_search(heatshrink_encoder *hse);
static HSE_state st_yield_tag_bit(heatshrink_encoder *hse,
    output_info *oi);
static HSE_state st_yield_literal(heatshrink_encoder *hse,
    output_info *oi);
static HSE_state st_yield_br_index(heatshrink_encoder *hse,
    output_info *oi);
static HSE_state st_yield_br_length(heatshrink_encoder *hse,
    output_info *oi);
static HSE_state st_save_backlog(heatshrink_encoder *hse);
static HSE_state st_flush_bit_buffer(heatshrink_encoder *hse,
    output_info *oi);

HSE_poll_res heatshrink_encoder_poll(heatshrink_encoder *hse,
        uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
    if ((hse == NULL) || (out_buf == NULL) || (output_size == NULL)) {
        return HSER_POLL_ERROR_NULL;
    }
    if (out_buf_size == 0) {
        LOG("-- MISUSE: output buffer size is 0\n");
        return HSER_POLL_ERROR_MISUSE;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- polling, state %u (%s), flags 0x%02x\n",
            hse->state, state_names[hse->state], hse->flags);

        uint8_t in_state = hse->state;
        switch (in_state) {
        case HSES_NOT_FULL:
            return HSER_POLL_EMPTY;
        case HSES_FILLED:
            do_indexing(hse);
            hse->state = HSES_SEARCH;
            break;
        case HSES_SEARCH:
            hse->state = st_step_search(hse);
            break;
        case HSES_YIELD_TAG_BIT:
            hse->state = st_yield_tag_bit(hse, &oi);
            break;
        case HSES_YIELD_LITERAL:
            hse->state = st_yield_literal(hse, &oi);
            break;
        case HSES_YIELD_BR_INDEX:
            hse->state = st_yield_br_index(hse, &oi);
            break;
        case HSES_YIELD_BR_LENGTH:
            hse->state = st_yield_br_length(hse, &oi);
            break;
        case HSES_SAVE_BACKLOG:
            hse->state = st_save_backlog(hse);
            break;
        case HSES_FLUSH_BITS:
            hse->state = st_flush_bit_buffer(hse, &oi);
        case HSES_DONE:
            return HSER_POLL_EMPTY;
        default:
            LOG("-- bad state %s\n", state_names[hse->state]);
            return HSER_POLL_ERROR_MISUSE;
        }

        if (hse->state == in_state) {
            /* 检查输出缓冲区是否已满。 */
            if (*output_size == out_buf_size) return HSER_POLL_MORE;
        }
    }
}

HSE_finish_res heatshrink_encoder_finish(heatshrink_encoder *hse) {
    if (hse == NULL) { return HSER_FINISH_ERROR_NULL; }
    LOG("-- setting is_finishing flag\n");
    hse->flags |= FLAG_IS_FINISHING;
    if (hse->state == HSES_NOT_FULL) { hse->state = HSES_FILLED; }
    return hse->state == HSES_DONE ? HSER_FINISH_DONE : HSER_FINISH_MORE;
}

static HSE_state st_step_search(heatshrink_encoder *hse) {
    uint16_t window_length = get_input_buffer_size(hse);
    uint16_t lookahead_sz = get_lookahead_size(hse);
    uint16_t msi = hse->match_scan_index;
    LOG("## step_search, scan @ +%d (%d/%d), input size %d\n",
        msi, hse->input_size + msi, 2*window_length, hse->input_size);

    bool fin = is_finishing(hse);
    if (msi > hse->input_size - (fin ? 1 : lookahead_sz)) {
        /* 当前搜索缓冲区已耗尽，把它拷入 backlog 并等待更多输入。 */
        LOG("-- end of search @ %d\n", msi);
        return fin ? HSES_FLUSH_BITS : HSES_SAVE_BACKLOG;
    }

    uint16_t input_offset = get_input_offset(hse);
    uint16_t end = input_offset + msi;
    uint16_t start = end - window_length;

    uint16_t max_possible = lookahead_sz;
    if (hse->input_size - msi < lookahead_sz) {
        max_possible = hse->input_size - msi;
    }

    uint16_t match_length = 0;
    uint16_t match_pos = find_longest_match(hse,
        start, end, max_possible, &match_length);

    if (match_pos == MATCH_NOT_FOUND) {
        LOG("ss Match not found\n");
        hse->match_scan_index++;
        hse->match_length = 0;
        return HSES_YIELD_TAG_BIT;
    } else {
        LOG("ss Found match of %d bytes at %d\n", match_length, match_pos);
        hse->match_pos = match_pos;
        hse->match_length = match_length;
        ASSERT(match_pos <= 1 << HEATSHRINK_ENCODER_WINDOW_BITS(hse) /*window_length*/);

        return HSES_YIELD_TAG_BIT;
    }
}

static HSE_state st_yield_tag_bit(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        if (hse->match_length == 0) {
            add_tag_bit(hse, oi, HEATSHRINK_LITERAL_MARKER);
            return HSES_YIELD_LITERAL;
        } else {
            add_tag_bit(hse, oi, HEATSHRINK_BACKREF_MARKER);
            hse->outgoing_bits = hse->match_pos - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_WINDOW_BITS(hse);
            return HSES_YIELD_BR_INDEX;
        }
    } else {
        return HSES_YIELD_TAG_BIT; /* 输出已满，继续 */
    }
}

static HSE_state st_yield_literal(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        push_literal_byte(hse, oi);
        return HSES_SEARCH;
    } else {
        return HSES_YIELD_LITERAL;
    }
}

static HSE_state st_yield_br_index(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        LOG("-- yielding backref index %u\n", hse->match_pos);
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_INDEX; /* 继续 */
        } else {
            hse->outgoing_bits = hse->match_length - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse);
            return HSES_YIELD_BR_LENGTH; /* 完成 */
        }
    } else {
        return HSES_YIELD_BR_INDEX; /* 继续 */
    }
}

static HSE_state st_yield_br_length(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        LOG("-- yielding backref length %u\n", hse->match_length);
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_LENGTH;
        } else {
            hse->match_scan_index += hse->match_length;
            hse->match_length = 0;
            return HSES_SEARCH;
        }
    } else {
        return HSES_YIELD_BR_LENGTH;
    }
}

static HSE_state st_save_backlog(heatshrink_encoder *hse) {
    LOG("-- saving backlog\n");
    save_backlog(hse);
    return HSES_NOT_FULL;
}

static HSE_state st_flush_bit_buffer(heatshrink_encoder *hse,
        output_info *oi) {
    if (hse->bit_index == 0x80) {
        LOG("-- done!\n");
        return HSES_DONE;
    } else if (can_take_byte(oi)) {
        LOG("-- flushing remaining byte (bit_index == 0x%02x)\n", hse->bit_index);
        oi->buf[(*oi->output_size)++] = hse->current_byte;
        LOG("-- done!\n");
        return HSES_DONE;
    } else {
        return HSES_FLUSH_BITS;
    }
}

static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag) {
    LOG("-- adding tag bit: %d\n", tag);
    push_bits(hse, 1, tag, oi);
}

static uint16_t get_input_offset(heatshrink_encoder *hse) {
    return get_input_buffer_size(hse);
}

static uint16_t get_input_buffer_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
    (void)hse;
}

static uint16_t get_lookahead_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse));
    (void)hse;
}

static void do_indexing(heatshrink_encoder *hse) {
#if HEATSHRINK_USE_INDEX
    /* 构建索引数组 index，为缓冲区中每个字节的“上一次出现”维护
     * 扁平化的链表。
     *
     * 例如，若 buf[200] == 'x'，则 index[200] 要么是一个满足
     * buf[i] == 'x' 的偏移 i，要么是一个负值（表示链表结束）。
     * 这能显著加快匹配速度，代价只是
     * sizeof(uint16_t) * sizeof(buffer) 字节的 RAM。
     *
     * 后续可优化方向：
     * 1. 既然任何负值都表示链表结束，其余 15 个比特
     *    可以用来动态改善索引。
     * 2. 同理，索引最后 lookahead_sz 个字节不会被使用，
     *    也可以在那里存放临时数据以动态改善索引。
     */
    struct hs_index *hsi = HEATSHRINK_ENCODER_INDEX(hse);
    int16_t last[256];
    memset(last, 0xFF, sizeof(last));

    uint8_t * const data = hse->buffer;
    int16_t * const index = hsi->index;

    const uint16_t input_offset = get_input_offset(hse);
    const uint16_t end = input_offset + hse->input_size;

    for (uint16_t i=0; i<end; i++) {
        uint8_t v = data[i];
        int16_t lv = last[v];
        index[i] = lv;
        last[v] = i;
    }
#else
    (void)hse;
#endif
}

static int is_finishing(heatshrink_encoder *hse) {
    return hse->flags & FLAG_IS_FINISHING;
}

static int can_take_byte(output_info *oi) {
    return *oi->output_size < oi->buf_size;
}

/* 在 buf[start] 到 buf[end-1] 之间，为 buf[end:end+maxlen] 处的字节
 * 查找最长匹配。未找到则返回 -1。 */
static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
        uint16_t end, const uint16_t maxlen, uint16_t *match_length) {
    LOG("-- scanning for match of buf[%u:%u] between buf[%u:%u] (max %u bytes)\n",
        end, end + maxlen, start, end + maxlen - 1, maxlen);
    uint8_t *buf = hse->buffer;

    uint16_t match_maxlen = 0;
    uint16_t match_index = MATCH_NOT_FOUND;

    uint16_t len = 0;
    uint8_t * const needlepoint = &buf[end];
#if HEATSHRINK_USE_INDEX
    struct hs_index *hsi = HEATSHRINK_ENCODER_INDEX(hse);
    int16_t pos = hsi->index[end];

    while (pos - (int16_t)start >= 0) {
        uint8_t * const pospoint = &buf[pos];
        len = 0;

        /* 只检查那些可能超过当前最大长度的匹配。
         * 在 match_maxlen 为 0 时这与索引是冗余的，但检查
         * 它是否 == 0 所增加的额外分支开销似乎更糟。 */
        if (pospoint[match_maxlen] != needlepoint[match_maxlen]) {
            pos = hsi->index[pos];
            continue;
        }

        for (len = 1; len < maxlen; len++) {
            if (pospoint[len] != needlepoint[len]) break;
        }

        if (len > match_maxlen) {
            match_maxlen = len;
            match_index = pos;
            if (len == maxlen) { break; } /* 不可能找到更优的了 */
        }
        pos = hsi->index[pos];
    }
#else
    for (int16_t pos=end - 1; pos - (int16_t)start >= 0; pos--) {
        uint8_t * const pospoint = &buf[pos];
        if ((pospoint[match_maxlen] == needlepoint[match_maxlen])
            && (*pospoint == *needlepoint)) {
            for (len=1; len<maxlen; len++) {
                if (0) {
                    LOG("  --> cmp buf[%d] == 0x%02x against %02x (start %u)\n",
                        pos + len, pospoint[len], needlepoint[len], start);
                }
                if (pospoint[len] != needlepoint[len]) { break; }
            }
            if (len > match_maxlen) {
                match_maxlen = len;
                match_index = pos;
                if (len == maxlen) { break; } /* 不再继续查找 */
            }
        }
    }
#endif

    const size_t break_even_point =
      (1 + HEATSHRINK_ENCODER_WINDOW_BITS(hse) +
          HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse));

    /* 不是拿 break_even_point 与 8*match_maxlen 比较，而是拿
     * match_maxlen 与 break_even_point/8 比较，以避免溢出。
     * 由于 MIN_WINDOW_BITS 和 MIN_LOOKAHEAD_BITS 分别为 4 和 3，
     * break_even_point/8 始终至少为 1。 */
    if (match_maxlen > (break_even_point / 8)) {
        LOG("-- best match: %u bytes at -%u\n",
            match_maxlen, end - match_index);
        *match_length = match_maxlen;
        return end - match_index;
    }
    LOG("-- none found\n");
    return MATCH_NOT_FOUND;
}

static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi) {
    uint8_t count = 0;
    uint8_t bits = 0;
    if (hse->outgoing_bits_count > 8) {
        count = 8;
        bits = hse->outgoing_bits >> (hse->outgoing_bits_count - 8);
    } else {
        count = hse->outgoing_bits_count;
        bits = hse->outgoing_bits;
    }

    if (count > 0) {
        LOG("-- pushing %d outgoing bits: 0x%02x\n", count, bits);
        push_bits(hse, count, bits, oi);
        hse->outgoing_bits_count -= count;
    }
    return count;
}

/* 把 COUNT（最多 8）个比特写入还有空间的输出缓冲区。
 * 字节从最低位开始填充。 */
static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
        output_info *oi) {
    ASSERT(count <= 8);
    LOG("++ push_bits: %d bits, input of 0x%02x\n", count, bits);

    /* 如果要写入的是完整一个字节且正处于新输出字节的起始处，
     * 就直接整字节写入，跳过逐比特处理循环。 */
    if (count == 8 && hse->bit_index == 0x80) {
        oi->buf[(*oi->output_size)++] = bits;
    } else {
        for (int i=count - 1; i>=0; i--) {
            bool bit = bits & (1 << i);
            if (bit) { hse->current_byte |= hse->bit_index; }
            if (0) {
                LOG("  -- setting bit %d at bit index 0x%02x, byte => 0x%02x\n",
                    bit ? 1 : 0, hse->bit_index, hse->current_byte);
            }
            hse->bit_index >>= 1;
            if (hse->bit_index == 0x00) {
                hse->bit_index = 0x80;
                LOG(" > pushing byte 0x%02x\n", hse->current_byte);
                oi->buf[(*oi->output_size)++] = hse->current_byte;
                hse->current_byte = 0x00;
            }
        }
    }
}

static void push_literal_byte(heatshrink_encoder *hse, output_info *oi) {
    uint16_t processed_offset = hse->match_scan_index - 1;
    uint16_t input_offset = get_input_offset(hse) + processed_offset;
    uint8_t c = hse->buffer[input_offset];
    LOG("-- yielded literal byte 0x%02x ('%c') from +%d\n",
        c, isprint(c) ? c : '.', input_offset);
    push_bits(hse, 8, c, oi);
}

static void save_backlog(heatshrink_encoder *hse) {
    size_t input_buf_sz = get_input_buffer_size(hse);

    uint16_t msi = hse->match_scan_index;

    /* 把已处理的数据拷贝到缓冲区开头，以便用于后续匹配。
     * 无需检查输入是否小于最大尺寸，因为如果不是，那反正
     * 也快结束了。 */
    uint16_t rem = input_buf_sz - msi; // 未处理的字节数
    uint16_t shift_sz = input_buf_sz + rem;

    memmove(&hse->buffer[0],
        &hse->buffer[input_buf_sz - rem],
        shift_sz);

    hse->match_scan_index = 0;
    hse->input_size -= input_buf_sz - rem;
}
