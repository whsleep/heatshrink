#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_BITS 8                  // 窗口大小，即哈希表大小
#define LOOKAHEAD_BITS 4               // 预测长度，即最长匹配长度
#define WINDOW_SIZE (1 << WINDOW_BITS) // 256
#define MAX_LEN (1 << LOOKAHEAD_BITS)  // 16
#define BREAK_EVEN (1 + WINDOW_BITS + LOOKAHEAD_BITS) // 13
#define MIN_MATCH ((BREAK_EVEN / 8) + 1)              // 2

/* ---------- 构建索引链表 ---------- */
void build_index(const uint8_t *data, int len, int *index) {
  int last[256]; // 记录每个字节值最近一次出现的位置
  for (int i = 0; i < 256; i++) // 初始化为 -1（表示还没出现过）
    last[i] = -1;
  for (int i = 0; i < len; i++) {
    index[i] = last[data[i]]; // 把 index[i] 指向上一个同值位置
    last[data[i]] = i;        // 更新该字节值最近位置为当前 i
  }
}

/* ---------- 在窗口内搜索最长匹配 ---------- */
int find_match(const uint8_t *data, int input_len, int pos, const int *index,
               int *offset_out, int *len_out) {
  int best_len = 0;              // 最长匹配长度
  int best_pos = -1;             // 最长匹配的起始位置
  int max_len = MAX_LEN;         // 最长匹配长度上限
  if (max_len > input_len - pos) // 若超出输入长度，则取输入长度
    max_len = input_len - pos;

  int p = index[pos]; // 从最近的同值位置开始
  while (p >= 0 && p >= pos - WINDOW_SIZE) {
    // 剪枝：若第 best_len 个字节不同，此候选不可能刷新最佳
    if (best_len > 0 && best_len < max_len &&
        data[p + best_len] != data[pos + best_len]) {
      p = index[p];
      continue;
    }

    int len = 0;
    while (len < max_len && data[p + len] == data[pos + len]) {
      len++;
    }

    if (len > best_len) {
      best_len = len;
      best_pos = p;
      if (best_len == max_len)
        break; // 已达理论上限
    }
    p = index[p];
  }

  if (best_len >= MIN_MATCH) {
    *offset_out = pos - best_pos;
    *len_out = best_len;
    return 1;
  }
  return 0;
}

/* ---------- 比特流写入（MSB first） ---------- */
typedef struct {
  uint8_t *buf;
  int buf_size;
  int bit_pos;
} BitWriter;

void bw_init(BitWriter *bw, uint8_t *buf, int buf_size) {
  bw->buf = buf;
  bw->buf_size = buf_size;
  bw->bit_pos = 0;
  memset(buf, 0, buf_size);
}

void bw_write_bits(BitWriter *bw, uint32_t value, int nbits) {
  for (int i = nbits - 1; i >= 0; i--) {
    int bit = (value >> i) & 1;
    int byte_idx = bw->bit_pos / 8;
    int bit_idx = 7 - (bw->bit_pos % 8);
    if (byte_idx >= bw->buf_size)
      return;
    if (bit)
      bw->buf[byte_idx] |= (1 << bit_idx);
    bw->bit_pos++;
  }
}

/* ---------- 比特流读取 ---------- */
typedef struct {
  const uint8_t *buf;
  int bit_pos;
} BitReader;

uint32_t br_read_bits(BitReader *br, int nbits) {
  uint32_t val = 0;
  for (int i = 0; i < nbits; i++) {
    int byte_idx = br->bit_pos / 8;
    int bit_idx = 7 - (br->bit_pos % 8);
    int bit = (br->buf[byte_idx] >> bit_idx) & 1;
    val = (val << 1) | bit;
    br->bit_pos++;
  }
  return val;
}

/* ---------- 压缩 ---------- */
int compress(const uint8_t *input, int input_len, uint8_t *output,
             int output_cap) {
  int *index = malloc(input_len * sizeof(int));
  if (!index)
    return -1;
  build_index(input, input_len, index);

  BitWriter bw;
  bw_init(&bw, output, output_cap);

  int pos = 0;
  while (pos < input_len) {
    int offset, length;
    if (find_match(input, input_len, pos, index, &offset, &length)) {
      printf("pos %3d: backref  offset=%3d length=%2d\n", pos, offset, length);
      bw_write_bits(&bw, 0, 1);                       // tag=0
      bw_write_bits(&bw, offset - 1, WINDOW_BITS);    // 存 offset-1
      bw_write_bits(&bw, length - 1, LOOKAHEAD_BITS); // 存 length-1
      pos += length;
    } else {
      printf("pos %3d: literal  0x%02X\n", pos, input[pos]);
      bw_write_bits(&bw, 1, 1);          // tag=1
      bw_write_bits(&bw, input[pos], 8); // 原始字节
      pos += 1;
    }
  }

  int total_bytes = (bw.bit_pos + 7) / 8;
  free(index);
  return total_bytes;
}

/* ---------- 解压 ---------- */
int decompress(const uint8_t *compressed, int compressed_len, uint8_t *output,
               int original_len) {
  BitReader br = {compressed, 0};
  int out_pos = 0;

  while (out_pos < original_len) {
    uint32_t tag = br_read_bits(&br, 1);
    if (tag == 1) {
      uint8_t byte = (uint8_t)br_read_bits(&br, 8);
      output[out_pos++] = byte;
    } else {
      uint32_t offset_val = br_read_bits(&br, WINDOW_BITS);
      uint32_t length_val = br_read_bits(&br, LOOKAHEAD_BITS);
      int offset = offset_val + 1;
      int length = length_val + 1;

      for (int i = 0; i < length; i++) {
        if (out_pos - offset < 0) {
          fprintf(stderr, "invalid backref at pos %d, offset %d\n", out_pos,
                  offset);
          return -1;
        }
        output[out_pos] = output[out_pos - offset]; // 支持重叠复制
        out_pos++;
      }
    }
  }
  return 0;
}

/* ---------- 测试 ---------- */
int main(void) {
  // 构造 0x00..0x0F 重复 4 次，共 64 字节
  uint8_t input[64];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 16; j++)
      input[i * 16 + j] = j;

  printf("Original (%d bytes):\n", (int)sizeof(input));
  for (int i = 0; i < 64; i++) {
    if (i % 16 == 0)
      printf("  ");
    printf("%02X ", input[i]);
    if (i % 16 == 15)
      printf("\n");
  }

  uint8_t compressed[100];
  int comp_len = compress(input, sizeof(input), compressed, sizeof(compressed));

  printf("\nCompressed (%d bytes):\n  ", comp_len);
  for (int i = 0; i < comp_len; i++)
    printf("%02X ", compressed[i]);
  printf("\n");

  uint8_t output[64] = {0};
  if (decompress(compressed, comp_len, output, sizeof(input)) != 0) {
    printf("Decompression failed!\n");
    return 1;
  }

  printf("\nDecompressed (%d bytes):\n", (int)sizeof(output));
  for (int i = 0; i < 64; i++) {
    if (i % 16 == 0)
      printf("  ");
    printf("%02X ", output[i]);
    if (i % 16 == 15)
      printf("\n");
  }

  if (memcmp(input, output, sizeof(input)) == 0)
    printf("\n[OK] 压缩/解压结果一致！\n");
  else
    printf("\n[FAIL] 结果不一致！\n");

  return 0;
}