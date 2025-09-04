# 目录设置
SRC_DIR = $(PWD)/Src
INCLUDE_DIR = $(PWD)/Include
THIRD_DIR = $(PWD)/Third
EXAMPLE_DIR = $(PWD)/example
BUILD_DIR = $(PWD)/Build
BIN_DIR = $(PWD)/Bin

# 源文件和目标文件
SRCS = \
	$(SRC_DIR)/Threadpool.c \
	$(EXAMPLE_DIR)/example.c \
	$(THIRD_DIR)/Src/ring_queue/ring_queue.c \
	$(THIRD_DIR)/Src/mempool/memory_pool.c

# 针对不同子目录分别生成目标文件路径
OBJS = \
	$(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%.c,$(SRCS))) \
	$(patsubst $(EXAMPLE_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(EXAMPLE_DIR)/%.c,$(SRCS))) \
	$(patsubst $(THIRD_DIR)/Src/ring_queue/%.c,$(BUILD_DIR)/%.o,$(filter $(THIRD_DIR)/Src/ring_queue/%.c,$(SRCS))) \
	$(patsubst $(THIRD_DIR)/Src/mempool/%.c,$(BUILD_DIR)/%.o,$(filter $(THIRD_DIR)/Src/mempool/%.c,$(SRCS)))
TARGET = $(BIN_DIR)/threadpool_demo

CC = gcc
CFLAGS = -I$(INCLUDE_DIR)/ -I$(THIRD_DIR)/Include -Wall -Wextra -O2 -pthread
DEBUG_FLAGS = -g -DDEBUG

# 默认目标
all: prepare $(TARGET)

# 准备构建目录
prepare:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# 编译目标程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# 编译对象文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(EXAMPLE_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(THIRD_DIR)/Src/ring_queue/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(THIRD_DIR)/Src/mempool/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 调试版本
debug: CFLAGS += $(DEBUG_FLAGS)
debug: all

# 清理构建文件
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# 只运行不编译
run: all
	./$(TARGET)

# 静态库规则
static: prepare $(BUILD_DIR)/Threadpool.o $(BUILD_DIR)/ring_queue.o $(BUILD_DIR)/memory_pool.o
	ar rcs $(BIN_DIR)/libthreadpool.a \
		$(BUILD_DIR)/Threadpool.o \
        $(BUILD_DIR)/ring_queue.o \
        $(BUILD_DIR)/memory_pool.o

# 动态库规则
shared: CFLAGS += -fPIC
shared: prepare 
	$(CC) $(CFLAGS) -fPIC -c $(SRC_DIR)/Threadpool.c -o $(BUILD_DIR)/Threadpool.pic.o
	$(CC) $(CFLAGS) -fPIC -c $(THIRD_DIR)/Src/ring_queue/ring_queue.c -o $(BUILD_DIR)/ring_queue.pic.o
	$(CC) $(CFLAGS) -fPIC -c $(THIRD_DIR)/Src/mempool/memory_pool.c -o $(BUILD_DIR)/memory_pool.pic.o
	$(CC) -shared -o $(BIN_DIR)/libthreadpool.so \
        $(BUILD_DIR)/Threadpool.pic.o \
        $(BUILD_DIR)/ring_queue.pic.o \
        $(BUILD_DIR)/memory_pool.pic.o

.PHONY: all prepare clean debug run static shared