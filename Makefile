# 目录设置
SRC_DIR = $(PWD)/Src
INCLUDE_DIR = $(PWD)/Include
BUILD_DIR = $(PWD)/Build
BIN_DIR = $(PWD)/Bin

# 源文件和目标文件
SRCS = $(SRC_DIR)/Threadpool.c $(SRC_DIR)/main.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET = $(BIN_DIR)/threadpool_demo

CC = gcc
CFLAGS = -I$(INCLUDE_DIR)/ -Wall -Wextra -O2 -pthread
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
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

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
static: prepare $(BUILD_DIR)/Threadpool.o
	ar rcs $(BIN_DIR)/libthreadpool.a $(BUILD_DIR)/Threadpool.o

# 动态库规则
shared: CFLAGS += -fPIC
shared: prepare $(BUILD_DIR)/Threadpool.o
	$(CC) -shared -o $(BIN_DIR)/libthreadpool.so $(BUILD_DIR)/Threadpool.o

# 安装规则
install: static shared
	@echo "安装库文件到系统..."
	@sudo cp $(BIN_DIR)/libthreadpool.a /usr/local/lib/
	@sudo cp $(BIN_DIR)/libthreadpool.so /usr/local/lib/
	@sudo cp $(INCLUDE_DIR)/Threadpool.h /usr/local/include/
	@sudo ldconfig
	@echo "库安装完成"

# 卸载规则
uninstall:
	@echo "从系统中移除库文件..."
	@sudo rm -f /usr/local/lib/libthreadpool.a
	@sudo rm -f /usr/local/lib/libthreadpool.so
	@sudo rm -f /usr/local/include/Threadpool.h
	@sudo ldconfig
	@echo "库卸载完成"

.PHONY: all prepare clean debug run static shared install uninstall