PROJECT = heatshrink
BUILD_DIR = Build

OPTIMIZE = -O3
WARN = -Wall -Wextra -pedantic #-Werror
WARN += -Wmissing-prototypes
WARN += -Wstrict-prototypes
WARN += -Wmissing-declarations

CFLAGS += -std=c99 -g ${WARN} ${OPTIMIZE}

# 全部使用静态内存分配（不依赖 malloc），可移植到嵌入式/裸机。
# 窗口 / lookahead / 输入缓冲区大小等编译期参数在 heatshrink_config.h：
#   HEATSHRINK_STATIC_WINDOW_BITS / HEATSHRINK_STATIC_LOOKAHEAD_BITS /
#   HEATSHRINK_STATIC_INPUT_BUFFER_SIZE
CFLAGS_STATIC = ${CFLAGS} -DHEATSHRINK_DYNAMIC_ALLOC=0

HEADERS = heatshrink_common.h heatshrink_config.h \
          heatshrink_encoder.h heatshrink_decoder.h

# 所有编译产物统一放在 Build/ 目录下。
STATIC_OBJS = $(BUILD_DIR)/heatshrink_encoder.os $(BUILD_DIR)/heatshrink_decoder.os

LIB_STATIC = $(BUILD_DIR)/libheatshrink_static.a

BIN_COMPRESS   = $(BUILD_DIR)/compress
BIN_DECOMPRESS = $(BUILD_DIR)/decompress
BIN_EXAMPLE    = $(BUILD_DIR)/example_compress
BIN_DEMO       = $(BUILD_DIR)/demo

all: libraries $(BIN_COMPRESS) $(BIN_DECOMPRESS) $(BIN_EXAMPLE) $(BIN_DEMO)

libraries: $(LIB_STATIC)

$(BIN_COMPRESS): compress.c $(HEADERS) $(LIB_STATIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_STATIC} -L$(BUILD_DIR) -lheatshrink_static

$(BIN_DECOMPRESS): decompress.c $(HEADERS) $(LIB_STATIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_STATIC} -L$(BUILD_DIR) -lheatshrink_static

$(BIN_EXAMPLE): example_compress.c $(HEADERS) $(LIB_STATIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_STATIC} -L$(BUILD_DIR) -lheatshrink_static

# demo 是独立的教学演示（不依赖 heatshrink 库，自带算法实现），
# 用基础警告级别即可，不套用库的严格原型检查。
$(BIN_DEMO): demo.c | $(BUILD_DIR)
	${CC} -o $@ $< -std=c99 -g ${OPTIMIZE} -Wall

$(LIB_STATIC): $(STATIC_OBJS) | $(BUILD_DIR)
	ar -rcs $@ $^

$(BUILD_DIR)/%.os: %.c $(HEADERS) | $(BUILD_DIR)
	${CC} -c -o $@ $< ${CFLAGS_STATIC}

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all libraries clean
