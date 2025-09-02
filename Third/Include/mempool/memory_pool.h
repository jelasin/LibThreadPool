#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifndef false
#define false 0
#endif

#ifndef true
#define true 1
#endif

// 魔数定义
#define MAGIC_NUMBER 0xDEADBEEF
// 内存对齐优化
#define DEFAULT_ALIGNMENT 64    // CPU缓存行大小
// 最小块大小
#define MIN_BLOCK_SIZE 32      // 减少碎片
// 最大固定大小类别
#define MAX_SIZE_CLASSES 16    // 支持的固定大小数量
#define PAGE_SIZE 4096

// 内存块头部结构
typedef struct memory_block {
    struct memory_block* next;     // 指向下一个空闲块
    size_t size;                   // 块大小（包含头部）
    uint32_t magic;                // 魔数，用于检测内存损坏
    uint32_t padding;              // 填充字节，保证对齐
} memory_block_t;

// 大小类别池（用于固定大小分配优化）
typedef struct size_class_pool {
    memory_block_t* free_blocks;   // 空闲块链表
    size_t block_size;             // 固定块大小
    size_t block_count;            // 总块数量
    size_t used_count;             // 已使用块数
} size_class_pool_t;

// 内存池结构
typedef struct memory_pool {
    void* pool_start;              // 池起始地址
    size_t pool_size;              // 池总大小
    size_t used_size;              // 已使用大小
    memory_block_t* free_list;     // 空闲块链表
    pthread_mutex_t mutex;         // 互斥锁
    bool thread_safe;              // 是否线程安全
    uint32_t alignment;            // 内存对齐字节数
    
    // 性能统计
    uint64_t alloc_count;          // 分配次数
    uint64_t free_count;           // 释放次数
    uint64_t merge_count;          // 合并次数
    
    // 固定大小池
    size_class_pool_t size_classes[MAX_SIZE_CLASSES];
    size_t class_sizes[MAX_SIZE_CLASSES];
    int num_classes;
} memory_pool_t;

// 内存池统计信息
typedef struct pool_stats {
    size_t total_size;             // 总大小
    size_t used_size;              // 已使用大小
    size_t free_size;              // 空闲大小
    size_t largest_free_block;     // 最大空闲块
    size_t fragmentation_ratio;    // 碎片率(百分比)
    uint64_t allocation_count;     // 分配次数
    uint64_t free_count;           // 释放次数
    uint64_t merge_count;          // 合并次数
    size_t free_block_count;       // 空闲块数量
} pool_stats_t;

// 内存池配置
typedef struct pool_config {
    size_t pool_size;              // 池大小
    bool thread_safe;              // 是否线程安全
    uint32_t alignment;            // 对齐字节数
    bool enable_size_classes;      // 是否启用固定大小池
    size_t* size_class_sizes;      // 固定大小数组
    int num_size_classes;          // 固定大小数量
} pool_config_t;

// 内存池创建和销毁
memory_pool_t* memory_pool_create(size_t pool_size, bool thread_safe);
memory_pool_t* memory_pool_create_with_config(const pool_config_t* config);
void memory_pool_destroy(memory_pool_t* pool);

// 内存分配和释放
void* memory_pool_alloc(memory_pool_t* pool, size_t size);
void* memory_pool_alloc_aligned(memory_pool_t* pool, size_t size, size_t alignment);
void* memory_pool_calloc(memory_pool_t* pool, size_t count, size_t size);
void* memory_pool_realloc(memory_pool_t* pool, void* ptr, size_t new_size);
void memory_pool_free(memory_pool_t* pool, void* ptr);

// 内存池管理
void memory_pool_reset(memory_pool_t* pool);
bool memory_pool_contains(memory_pool_t* pool, void* ptr);
size_t memory_pool_get_block_size(memory_pool_t* pool, void* ptr);

// 性能优化
void memory_pool_warmup(memory_pool_t* pool);
void memory_pool_defragment(memory_pool_t* pool);

// 统计和调试
void memory_pool_get_stats(memory_pool_t* pool, pool_stats_t* stats);
void memory_pool_print_stats(memory_pool_t* pool);
bool memory_pool_validate(memory_pool_t* pool);

// 固定大小池操作
int memory_pool_add_size_class(memory_pool_t* pool, size_t size, size_t count);
void* memory_pool_alloc_fixed(memory_pool_t* pool, size_t size);
void memory_pool_free_fixed(memory_pool_t* pool, void* ptr);

// 错误码
typedef enum {
    POOL_OK = 0,
    POOL_ERROR_NULL_POINTER,
    POOL_ERROR_INVALID_SIZE,
    POOL_ERROR_OUT_OF_MEMORY,
    POOL_ERROR_CORRUPTION,
    POOL_ERROR_DOUBLE_FREE,
    POOL_ERROR_INVALID_POINTER
} pool_error_t;

// 获取最后错误
pool_error_t memory_pool_get_last_error(void);
const char* memory_pool_error_string(pool_error_t error);

#endif // MEMORY_POOL_H
