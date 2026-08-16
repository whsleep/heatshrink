/*
 * example_compress.c —— heatshrink 压缩过程学习示例（静态内存分配）。
 *
 * 用一个硬编码的字节数组（0x00~0x0F 循环重复），演示 heatshrink 的完整
 * 压缩流程，并打印压缩结果（十六进制 + 大小对比），最后解压回去验证无损。
 *
 * 核心流程（编码器）只有三步：
 *   1. heatshrink_encoder_reset()  重置编码器（静态分配，无需 alloc）
 *   2. heatshrink_encoder_sink()   把输入数据“送”进编码器内部缓冲区
 *   3. heatshrink_encoder_poll()   从编码器“取”出压缩后的输出
 *   4. heatshrink_encoder_finish() 告诉编码器输入结束，冲刷剩余比特
 *
 * 这里使用静态分配的 encoder / decoder（static 全局变量，放在 BSS 段，
 * 不依赖 malloc），窗口 / lookahead 大小由编译期配置 heatshrink_config.h
 * 里的 HEATSHRINK_STATIC_WINDOW_BITS / HEATSHRINK_STATIC_LOOKAHEAD_BITS 决定。
 *
 * 原理简述（LZSS）：
 *   压缩时，编码器在已处理的历史数据（“窗口”）里查找与当前数据重复的
 *   片段。找到就用一个“反向引用”(offset, length) 代替原始字节，找不到就
 *   原样输出一个“字面量”字节。
 *
 *   输出是比特级的，每个元素前有一个 1 bit 的标记（tag）：
 *     tag = 1：后面跟着 8 bit 的字面量字节
 *     tag = 0：后面跟着 window_sz2 bit 的“偏移” + lookahead_sz2 bit 的“长度”
 *
 *   - window_sz2 = 8  => 窗口 = 2^8  = 256 字节（能往回引用多远）
 *   - lookahead_sz2 = 4 => 最长匹配 = 2^4 = 16 字节（一次能替代多长）
 *
 * 想观察编码器状态机每一步的细节，可把 heatshrink_config.h 里的
 * HEATSHRINK_DEBUGGING_LOGS 改成 1，再重新编译运行。
 *
 * 编译（或 `make example_compress`）：
 *   gcc -std=c99 -O2 -Wall example_compress.c -DHEATSHRINK_DYNAMIC_ALLOC=0 \
 *       -LBuild -lheatshrink_static -o example_compress
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heatshrink_decoder.h"
#include "heatshrink_encoder.h"

/*
 * 以下两个参数仅作理解参考，与 heatshrink_config.h 里的静态参数一致；
 * 静态分配时实际生效的是 config.h 里的
 * HEATSHRINK_STATIC_WINDOW_BITS / HEATSHRINK_STATIC_LOOKAHEAD_BITS。
 */
#define WINDOW_SZ2 8    /* 窗口大小 = 2^8 = 256 字节 */
#define LOOKAHEAD_SZ2 4 /* 最长匹配 = 2^4 = 16 字节 */

/* 输出缓冲区容量：64 字节输入压缩后 + 解压回 64 字节都远远用不满。 */
#define OUT_CAPACITY 256

/* 静态分配的 encoder / decoder（放在 BSS 段，不依赖堆内存）。 */
static heatshrink_encoder hse;
static heatshrink_decoder hsd;

/*
 * 给定的输入数据：0x00~0x0F 的字节序列重复 4 次，共 64 字节。
 * 第 1 遍（前 16 字节）是全新数据，会被当作“字面量”输出；
 * 后面 3 遍与第 1 遍完全重复，会被压缩成“反向引用”，压缩效果明显。
 */
static const uint8_t input[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
};
#define INPUT_SIZE (sizeof(input))

/* 以十六进制打印一段字节，每行 16 字节，带偏移。 */
static void hex_dump(const char *title, const uint8_t *data, size_t size) {
  printf("%s (%zu 字节):\n", title, size);
  for (size_t i = 0; i < size; i++) {
    if (i % 16 == 0) {
      printf("  %04zx: ", i);
    }
    printf("%02x ", data[i]);
    if (i % 16 == 15) {
      printf("\n");
    }
  }
  if (size % 16 != 0) {
    printf("\n");
  }
  printf("\n");
}

/*
 * 压缩：把 in_size 字节的输入压缩到 out（容量 out_capacity）。
 * 成功返回 0，并把实际写入的压缩字节数写到 *out_size。
 *
 * 这是通用的“流式”写法，输入可以任意大。关键点在于：
 *   - 编码器内部缓冲区是有限的（约 2*2^window_sz2 字节），一次 sink
 *     可能只吃下其中一部分，所以要用 while 循环反复 sink；
 *   - 每 sink 一段，就 poll 把已产生的压缩输出取走，腾出缓冲区空间；
 *   - 输入全部送完后，再 finish + poll 把末尾残留的比特冲刷出来。
 */
static int compress(const uint8_t *in, size_t in_size, uint8_t *out,
                    size_t out_capacity, size_t *out_size) {
  /* 1. 重置编码器（静态模式下参数由 heatshrink_config.h 决定）。 */
  heatshrink_encoder_reset(&hse);

  size_t in_pos = 0;  /* 已经送入的输入字节数 */
  size_t out_pos = 0; /* 已经写出的输出字节数 */

  /* 2. 反复“送入输入 -> 取出输出”，直到所有输入都处理完。 */
  while (in_pos < in_size) {
    /* sink：把输入送进编码器内部缓冲区。
     * *sunk 会被设成真正吃下的字节数（缓冲区可能一次装不下）。 */
    size_t sunk = 0;
    /* sink 形参非 const，但只读不写；这里的强转让调用方能传入 const 数据。 */
    if (heatshrink_encoder_sink(&hse, (uint8_t *)(in + in_pos),
                                in_size - in_pos, &sunk) < 0) {
      fprintf(stderr, "sink failed\n");
      return -1;
    }
    in_pos += sunk;

    /* poll：驱动状态机，把已产生的压缩输出拷贝到 out。
     * 返回 HSER_POLL_MORE 表示还有输出没取完，继续 poll；
     * 返回 HSER_POLL_EMPTY 表示当前没有更多输出。 */
    HSE_poll_res pres;
    do {
      size_t polled = 0;
      if (out_pos >= out_capacity) { /* 输出缓冲区溢出 */
        return -1;
      }
      pres = heatshrink_encoder_poll(&hse, out + out_pos,
                                     out_capacity - out_pos, &polled);
      if (pres < 0) {
        fprintf(stderr, "poll failed\n");
        return -1;
      }
      out_pos += polled;
    } while (pres == HSER_POLL_MORE);
  }

  /* 3. finish：标记输入结束，冲刷最后不足一个字节的残余比特。 */
  HSE_finish_res fres;
  do {
    fres = heatshrink_encoder_finish(&hse);
    if (fres < 0) {
      fprintf(stderr, "finish failed\n");
      return -1;
    }

    HSE_poll_res pres;
    do {
      size_t polled = 0;
      if (out_pos >= out_capacity) {
        return -1;
      }
      pres = heatshrink_encoder_poll(&hse, out + out_pos,
                                     out_capacity - out_pos, &polled);
      if (pres < 0) {
        fprintf(stderr, "poll failed\n");
        return -1;
      }
      out_pos += polled;
    } while (pres == HSER_POLL_MORE);
  } while (fres == HSER_FINISH_MORE);

  *out_size = out_pos;
  return 0;
}

/*
 * 解压验证：把压缩数据解压回 out，并与原始输入逐字节比较。
 * 成功返回 0，不一致返回 1，出错返回 -1。
 *
 * 解码器流程与编码器对称：sink（送入压缩数据）-> poll（取出解压数据）
 * -> finish（结束并冲刷）。注意解码器的 window / lookahead 必须与
 * 压缩时一致（这里都由 config.h 决定，天然一致）。
 */
static int decompress_and_verify(const uint8_t *comp, size_t comp_size,
                                 const uint8_t *orig, size_t orig_size) {
  /* 重置解码器（静态模式下参数由 heatshrink_config.h 决定）。 */
  heatshrink_decoder_reset(&hsd);

  uint8_t out[OUT_CAPACITY];
  size_t in_pos = 0;
  size_t out_pos = 0;

  /* sink + poll，送入全部压缩数据并取出解压结果。 */
  while (in_pos < comp_size) {
    size_t sunk = 0;
    if (heatshrink_decoder_sink(&hsd, (uint8_t *)(comp + in_pos),
                                comp_size - in_pos, &sunk) < 0) {
      return -1;
    }
    in_pos += sunk;

    HSD_poll_res pres;
    do {
      size_t polled = 0;
      if (out_pos >= OUT_CAPACITY) {
        return -1;
      }
      pres = heatshrink_decoder_poll(&hsd, out + out_pos,
                                     OUT_CAPACITY - out_pos, &polled);
      if (pres < 0) {
        return -1;
      }
      out_pos += polled;
    } while (pres == HSDR_POLL_MORE);
  }

  /* finish + poll，冲刷剩余输出。 */
  HSD_finish_res fres;
  do {
    fres = heatshrink_decoder_finish(&hsd);
    if (fres < 0) {
      return -1;
    }

    HSD_poll_res pres;
    do {
      size_t polled = 0;
      if (out_pos >= OUT_CAPACITY) {
        return -1;
      }
      pres = heatshrink_decoder_poll(&hsd, out + out_pos,
                                     OUT_CAPACITY - out_pos, &polled);
      if (pres < 0) {
        return -1;
      }
      out_pos += polled;
    } while (pres == HSDR_POLL_MORE);
  } while (fres == HSDR_FINISH_MORE);

  /* 校验：解压出的字节数和内容都要和原始输入一致。 */
  if (out_pos != orig_size) {
    printf("解压长度不一致：原始 %zu 字节，解压出 %zu 字节\n", orig_size,
           out_pos);
    return 1;
  }
  if (memcmp(out, orig, orig_size) != 0) {
    printf("解压内容不一致\n");
    return 1;
  }
  return 0;
}

int main(void) {
  uint8_t compressed[OUT_CAPACITY];
  size_t compressed_size = 0;

  /* 打印输入数据。 */
  hex_dump("输入数据", input, INPUT_SIZE);

  /* 压缩。 */
  if (compress(input, INPUT_SIZE, compressed, OUT_CAPACITY, &compressed_size) !=
      0) {
    fprintf(stderr, "压缩失败\n");
    return 1;
  }

  /* 打印压缩结果（hex + 大小对比）。 */
  hex_dump("压缩结果", compressed, compressed_size);
  printf("大小对比：%zu 字节 -> %zu 字节（压缩率 %.1f%%）\n", INPUT_SIZE,
         compressed_size, 100.0 * (double)compressed_size / (double)INPUT_SIZE);

  /* 解压验证。 */
  int rc =
      decompress_and_verify(compressed, compressed_size, input, INPUT_SIZE);
  if (rc == 0) {
    printf("解压验证：解压回 %zu 字节，与原数据一致 ✓\n", INPUT_SIZE);
    return 0;
  }
  return 1;
}
