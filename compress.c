/*
 * compress.c —— heatshrink 主机端压缩程序（静态内存分配）。
 *
 * 读取一个文件（默认 Boot_B.bin），用 heatshrink 压缩后写入输出文件
 * （默认 Boot_B.bin.hs）。
 *
 * 使用静态分配的 encoder（不依赖 malloc，可移植到嵌入式）。窗口 /
 * lookahead 参数由编译期配置 heatshrink_config.h 里的
 * HEATSHRINK_STATIC_WINDOW_BITS / HEATSHRINK_STATIC_LOOKAHEAD_BITS 决定，
 * 必须与解压端一致。
 *
 * 编译方式（或直接 `make compress`）：
 *   gcc -std=c99 -O2 -Wall compress.c -DHEATSHRINK_DYNAMIC_ALLOC=0 \
 *       -LBuild -lheatshrink_static -o compress
 */
#include <stdio.h>
#include <stdint.h>

#include "heatshrink_encoder.h"

/* 静态分配的 encoder（放在 BSS 段，不需要堆内存）。 */
static heatshrink_encoder hse;

/* 轮询并写出编码器当前所有待输出的数据。成功返回 0。 */
static int poll_and_write(FILE *out) {
    uint8_t buf[4096];
    HSE_poll_res pres;
    do {
        size_t count = 0;
        pres = heatshrink_encoder_poll(&hse, buf, sizeof(buf), &count);
        if (pres < 0) { fprintf(stderr, "encode poll failed\n"); return -1; }
        if (count > 0 && fwrite(buf, 1, count, out) != count) {
            fprintf(stderr, "write failed\n");
            return -1;
        }
    } while (pres == HSER_POLL_MORE);
    return 0;
}

/* 把输入流 `in` 压缩到输出流 `out`。成功返回 0，出错返回非 0。 */
static int compress_stream(FILE *in, FILE *out) {
    heatshrink_encoder_reset(&hse);

    uint8_t in_buf[1024];
    size_t n;
    while ((n = fread(in_buf, 1, sizeof(in_buf), in)) > 0) {
        size_t sunk = 0;
        while (sunk < n) {
            size_t count = 0;
            if (heatshrink_encoder_sink(&hse, in_buf + sunk, n - sunk, &count) < 0) {
                fprintf(stderr, "encode sink failed\n");
                return -1;
            }
            sunk += count;
            if (poll_and_write(out) < 0) {
                return -1;
            }
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "read failed\n");
        return -1;
    }

    /* 冲刷流的最后几个 bit。 */
    HSE_finish_res fres;
    do {
        fres = heatshrink_encoder_finish(&hse);
        if (fres < 0) {
            fprintf(stderr, "encode finish failed\n");
            return -1;
        }
        if (poll_and_write(out) < 0) {
            return -1;
        }
    } while (fres == HSER_FINISH_MORE);

    return 0;
}

int main(int argc, char **argv) {
    const char *in_name  = (argc > 1) ? argv[1] : "Boot_B.bin";
    const char *out_name = (argc > 2) ? argv[2] : "Boot_B.bin.hs";

    FILE *in = fopen(in_name, "rb");
    if (in == NULL) { perror(in_name); return 1; }
    FILE *out = fopen(out_name, "wb");
    if (out == NULL) { perror(out_name); fclose(in); return 1; }

    if (fseek(in, 0, SEEK_END) != 0) { perror("fseek"); fclose(in); fclose(out); return 1; }
    long in_size = ftell(in);
    rewind(in);

    int rc = compress_stream(in, out);

    long out_size = ftell(out);
    fclose(in);
    fclose(out);

    if (rc != 0) return 1;

    printf("%s: %ld -> %ld bytes (%.1f%%)\n",
        in_name, in_size, out_size, 100.0 * (double)out_size / (double)in_size);
    return 0;
}
