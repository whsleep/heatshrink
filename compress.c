/*
 * compress.c —— heatshrink 主机端压缩程序。
 *
 * 读取一个文件（默认 Boot_B.bin），用 heatshrink 压缩后写入输出文件
 * （默认 Boot_B.bin.hs）。压缩端使用动态内存分配。
 *
 * 下面的 window / lookahead 参数必须与解压端的编译期配置一致
 * （heatshrink_config.h 里的 HEATSHRINK_STATIC_WINDOW_BITS /
 *  HEATSHRINK_STATIC_LOOKAHEAD_BITS）。
 *
 * 编译方式（或直接 `make compress`）：
 *   gcc -std=c99 -O2 -Wall compress.c -DHEATSHRINK_DYNAMIC_ALLOC=1 \
 *       -LBuild -lheatshrink_dynamic -o compress
 */
#include <stdio.h>
#include <stdint.h>

#include "heatshrink_encoder.h"

#define WINDOW_SZ2     8   /* 2^8 = 256 字节窗口   */
#define LOOKAHEAD_SZ2  4   /* 2^4 = 16 字节前向匹配 */

/* 轮询并写出编码器当前所有待输出的数据。成功返回 0。 */
static int poll_and_write(heatshrink_encoder *hse, FILE *out) {
    uint8_t buf[4096];
    HSE_poll_res pres;
    do {
        size_t count = 0;
        pres = heatshrink_encoder_poll(hse, buf, sizeof(buf), &count);
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
    heatshrink_encoder *hse = heatshrink_encoder_alloc(WINDOW_SZ2, LOOKAHEAD_SZ2);
    if (hse == NULL) { fprintf(stderr, "encoder alloc failed\n"); return -1; }

    uint8_t in_buf[1024];
    size_t n;
    while ((n = fread(in_buf, 1, sizeof(in_buf), in)) > 0) {
        size_t sunk = 0;
        while (sunk < n) {
            size_t count = 0;
            if (heatshrink_encoder_sink(hse, in_buf + sunk, n - sunk, &count) < 0) {
                fprintf(stderr, "encode sink failed\n");
                heatshrink_encoder_free(hse);
                return -1;
            }
            sunk += count;
            if (poll_and_write(hse, out) < 0) {
                heatshrink_encoder_free(hse);
                return -1;
            }
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "read failed\n");
        heatshrink_encoder_free(hse);
        return -1;
    }

    /* 冲刷流的最后几个 bit。 */
    HSE_finish_res fres;
    do {
        fres = heatshrink_encoder_finish(hse);
        if (fres < 0) {
            fprintf(stderr, "encode finish failed\n");
            heatshrink_encoder_free(hse);
            return -1;
        }
        if (poll_and_write(hse, out) < 0) {
            heatshrink_encoder_free(hse);
            return -1;
        }
    } while (fres == HSER_FINISH_MORE);

    heatshrink_encoder_free(hse);
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
