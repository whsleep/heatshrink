PROJECT = heatshrink
BUILD_DIR = Build

OPTIMIZE = -O3
WARN = -Wall -Wextra -pedantic #-Werror
WARN += -Wmissing-prototypes
WARN += -Wstrict-prototypes
WARN += -Wmissing-declarations

CFLAGS += -std=c99 -g ${WARN} ${OPTIMIZE}

# 库会分别按“有/无动态内存分配”两个变体编译：
# CLI（heatshrink）和 compress 用动态分配；decompress 用静态分配。
CFLAGS_STATIC = ${CFLAGS} -DHEATSHRINK_DYNAMIC_ALLOC=0
CFLAGS_DYNAMIC = ${CFLAGS} -DHEATSHRINK_DYNAMIC_ALLOC=1

HEADERS = heatshrink_common.h heatshrink_config.h \
          heatshrink_encoder.h heatshrink_decoder.h

# 所有编译产物统一放在 Build/ 目录下。
DYNAMIC_OBJS = $(BUILD_DIR)/heatshrink_encoder.od $(BUILD_DIR)/heatshrink_decoder.od
STATIC_OBJS  = $(BUILD_DIR)/heatshrink_encoder.os $(BUILD_DIR)/heatshrink_decoder.os

LIB_STATIC  = $(BUILD_DIR)/libheatshrink_static.a
LIB_DYNAMIC = $(BUILD_DIR)/libheatshrink_dynamic.a

BIN_HEATSHRINK = $(BUILD_DIR)/heatshrink
BIN_COMPRESS   = $(BUILD_DIR)/compress
BIN_DECOMPRESS = $(BUILD_DIR)/decompress
BIN_EXAMPLE    = $(BUILD_DIR)/example_compress

all: $(BIN_HEATSHRINK) libraries $(BIN_COMPRESS) $(BIN_DECOMPRESS) $(BIN_EXAMPLE)

libraries: $(LIB_STATIC) $(LIB_DYNAMIC)

$(BIN_HEATSHRINK): $(BUILD_DIR)/heatshrink.od $(LIB_DYNAMIC) | $(BUILD_DIR)
	${CC} -o $@ $^ ${CFLAGS_DYNAMIC} -L$(BUILD_DIR) -lheatshrink_dynamic

$(BIN_COMPRESS): compress.c $(HEADERS) $(LIB_DYNAMIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_DYNAMIC} -L$(BUILD_DIR) -lheatshrink_dynamic

$(BIN_DECOMPRESS): decompress.c $(HEADERS) $(LIB_STATIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_STATIC} -L$(BUILD_DIR) -lheatshrink_static

$(BIN_EXAMPLE): example_compress.c $(HEADERS) $(LIB_DYNAMIC) | $(BUILD_DIR)
	${CC} -o $@ $< ${CFLAGS_DYNAMIC} -L$(BUILD_DIR) -lheatshrink_dynamic

$(LIB_STATIC): $(STATIC_OBJS) | $(BUILD_DIR)
	ar -rcs $@ $^

$(LIB_DYNAMIC): $(DYNAMIC_OBJS) | $(BUILD_DIR)
	ar -rcs $@ $^

$(BUILD_DIR)/%.od: %.c $(HEADERS) | $(BUILD_DIR)
	${CC} -c -o $@ $< ${CFLAGS_DYNAMIC}

$(BUILD_DIR)/%.os: %.c $(HEADERS) | $(BUILD_DIR)
	${CC} -c -o $@ $< ${CFLAGS_STATIC}

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all libraries clean
