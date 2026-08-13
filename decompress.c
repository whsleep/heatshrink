/*
 * decompress.c —— heatshrink 解压程序，静态内存分配（可移植到嵌入式）。
 *
 * 下面的核心函数 decompress() 使用静态分配的 decoder（不依赖 malloc、
 * 不依赖 stdio），可以直接连同 `static heatshrink_decoder hsd;` 声明一起
 * 拷贝进固件使用。
 *
 * main() 只是主机端测试程序：读取压缩文件（默认 Boot_B.bin.hs），
 * 写出解压结果（默认 Boot_B.restored.bin）。
 *
 * 编译期参数在 heatshrink_config.h 里，必须与压缩端一致：
 *   HEATSHRINK_STATIC_WINDOW_BITS    （== 压缩端的 window）
 *   HEATSHRINK_STATIC_LOOKAHEAD_BITS （== 压缩端的 lookahead）
 *   HEATSHRINK_STATIC_INPUT_BUFFER_SIZE
 *
 * 编译方式（或直接 `make decompress`）：
 *   gcc -std=c99 -O2 -Wall decompress.c -DHEATSHRINK_DYNAMIC_ALLOC=0 \
 *       -LBuild -lheatshrink_static -o decompress
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "heatshrink_decoder.h"

/* 静态分配的 decoder（放在 BSS 段，不需要堆内存）。 */
static heatshrink_decoder hsd;

size_t decompress(const uint8_t *in, size_t in_size,
                  uint8_t *out, size_t out_capacity);

/*
 * 把 in_size 字节的压缩数据 `in` 解压到 `out`，`out` 至少有 out_capacity
 * 字节空间。返回实际写入的解压字节数；出错（包括输出缓冲区溢出）返回 0。
 *
 * 不依赖堆内存、不依赖 stdio —— 可在裸机环境直接使用。
 */
size_t decompress(const uint8_t *in, size_t in_size,
                  uint8_t *out, size_t out_capacity) {
    heatshrink_decoder_reset(&hsd);

    size_t in_pos = 0;
    size_t out_pos = 0;

    /* 送入全部压缩输入，输入缓冲区满时轮询取出输出。 */
    while (in_pos < in_size) {
        size_t sunk = 0;
        /* sink() 形参是非 const 指针，但只读不写；这里的强转让调用方能安全
         * 传入 const（例如映射在 flash 上的）缓冲区。 */
        if (heatshrink_decoder_sink(&hsd, (uint8_t *)(in + in_pos),
                                    in_size - in_pos, &sunk) < 0)
            return 0;
        in_pos += sunk;

        HSD_poll_res pres;
        do {
            size_t polled = 0;
            if (out_pos >= out_capacity) return 0;   /* 输出缓冲区溢出 */
            pres = heatshrink_decoder_poll(&hsd, out + out_pos,
                                           out_capacity - out_pos, &polled);
            if (pres < 0) return 0;
            out_pos += polled;
        } while (pres == HSDR_POLL_MORE);
    }

    /* 冲刷剩余输出。 */
    HSD_finish_res fres;
    do {
        fres = heatshrink_decoder_finish(&hsd);
        if (fres < 0) return 0;

        HSD_poll_res pres;
        do {
            size_t polled = 0;
            if (out_pos >= out_capacity) return 0;
            pres = heatshrink_decoder_poll(&hsd, out + out_pos,
                                           out_capacity - out_pos, &polled);
            if (pres < 0) return 0;
            out_pos += polled;
        } while (pres == HSDR_POLL_MORE);
    } while (fres == HSDR_FINISH_MORE);

    return out_pos;
}

/* ---- 主机端测试程序（移植到固件时删掉以下 main 即可）。 ---- */

int main(int argc, char **argv) {
    const char *in_name  = (argc > 1) ? argv[1] : "Boot_B.bin.hs";
    const char *out_name = (argc > 2) ? argv[2] : "Boot_B.restored.bin";

    FILE *in = fopen(in_name, "rb");
    if (in == NULL) { perror(in_name); return 1; }

    if (fseek(in, 0, SEEK_END) != 0) { perror("fseek"); fclose(in); return 1; }
    long in_size = ftell(in);
    if (in_size < 0) { perror("ftell"); fclose(in); return 1; }
    rewind(in);

    uint8_t *in_buf = malloc((size_t)in_size);
    if (in_buf == NULL) { fprintf(stderr, "out of memory\n"); fclose(in); return 1; }
    if (fread(in_buf, 1, (size_t)in_size, in) != (size_t)in_size) {
        fprintf(stderr, "read failed\n");
        free(in_buf);
        fclose(in);
        return 1;
    }
    fclose(in);

    /* 输出缓冲区留足余量（heatshrink 不保存原始长度）。 */
    size_t out_capacity = (size_t)in_size * 8 + 1024;
    uint8_t *out_buf = malloc(out_capacity);
    if (out_buf == NULL) { fprintf(stderr, "out of memory\n"); free(in_buf); return 1; }

    size_t out_size = decompress(in_buf, (size_t)in_size, out_buf, out_capacity);
    if (out_size == 0) {
        fprintf(stderr, "decompression failed\n");
        free(out_buf);
        free(in_buf);
        return 1;
    }

    FILE *out = fopen(out_name, "wb");
    if (out == NULL) { perror(out_name); free(out_buf); free(in_buf); return 1; }
    fwrite(out_buf, 1, out_size, out);
    fclose(out);

    free(out_buf);
    free(in_buf);

    printf("%s: %ld -> %s (%zu bytes)\n", in_name, in_size, out_name, out_size);
    return 0;
}
