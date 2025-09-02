#include "../../Include/mempool/memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

// 线程局部错误码
static __thread pool_error_t g_last_error = POOL_OK;

// 内部函数声明
static inline size_t align_size(size_t size, size_t alignment);
static inline bool is_power_of_two(size_t n);
static memory_block_t* find_best_fit(memory_pool_t* pool, size_t size);
static void merge_free_blocks(memory_pool_t* pool);
static bool validate_block(memory_block_t* block);
static void insert_free_block(memory_pool_t* pool, memory_block_t* block);

// 设置错误码
static inline void set_error(pool_error_t error) {
    g_last_error = error;
}

// 获取最后错误
pool_error_t memory_pool_get_last_error(void) {
    return g_last_error;
}

// 错误码转字符串
const char* memory_pool_error_string(pool_error_t error) {
    switch (error) {
        case POOL_OK: return "Success";
        case POOL_ERROR_NULL_POINTER: return "Null pointer";
        case POOL_ERROR_INVALID_SIZE: return "Invalid size";
        case POOL_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case POOL_ERROR_CORRUPTION: return "Memory corruption detected";
        case POOL_ERROR_DOUBLE_FREE: return "Double free detected";
        case POOL_ERROR_INVALID_POINTER: return "Invalid pointer";
        default: return "Unknown error";
    }
}

// 对齐大小
static inline size_t align_size(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// 检查是否为2的幂
static inline bool is_power_of_two(size_t n) {
    return n && !(n & (n - 1));
}

// 验证内存块
static bool validate_block(memory_block_t* block) {
    return block && block->magic == MAGIC_NUMBER && block->size >= sizeof(memory_block_t);
}

// 创建内存池
memory_pool_t* memory_pool_create(size_t pool_size, bool thread_safe) {
    pool_config_t config = {
        .pool_size = pool_size,
        .thread_safe = thread_safe,
        .alignment = DEFAULT_ALIGNMENT,
        .enable_size_classes = false,
        .size_class_sizes = NULL,
        .num_size_classes = 0
    };
    return memory_pool_create_with_config(&config);
}

// 使用配置创建内存池
memory_pool_t* memory_pool_create_with_config(const pool_config_t* config) {
    if (!config || config->pool_size == 0) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    if (!is_power_of_two(config->alignment)) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    memory_pool_t* pool = malloc(sizeof(memory_pool_t));
    if (!pool) {
        set_error(POOL_ERROR_OUT_OF_MEMORY);
        return NULL;
    }

    // 确保池大小按页对齐
    size_t aligned_size = align_size(config->pool_size, PAGE_SIZE);

    // 使用mmap分配大块内存，获得更好的性能
    pool->pool_start = mmap(NULL, aligned_size, 
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (pool->pool_start == MAP_FAILED) {
        free(pool);
        set_error(POOL_ERROR_OUT_OF_MEMORY);
        return NULL;
    }

    pool->pool_size = aligned_size;
    pool->used_size = 0;
    pool->alignment = config->alignment;
    pool->thread_safe = config->thread_safe;
    pool->alloc_count = 0;
    pool->free_count = 0;
    pool->merge_count = 0;
    pool->num_classes = 0;

    // 初始化互斥锁
    if (pool->thread_safe) {
        if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
            munmap(pool->pool_start, pool->pool_size);
            free(pool);
            set_error(POOL_ERROR_OUT_OF_MEMORY);
            return NULL;
        }
    }

    // 初始化空闲链表 - 整个池作为一个大的空闲块
    memory_block_t* initial_block = (memory_block_t*)pool->pool_start;
    initial_block->next = NULL;
    initial_block->size = pool->pool_size;
    initial_block->magic = MAGIC_NUMBER;
    initial_block->padding = 0;
    pool->free_list = initial_block;

    // 初始化固定大小池
    if (config->enable_size_classes && config->size_class_sizes && config->num_size_classes > 0) {
        int classes_to_add = config->num_size_classes < MAX_SIZE_CLASSES ? 
                           config->num_size_classes : MAX_SIZE_CLASSES;
        
        for (int i = 0; i < classes_to_add; i++) {
            pool->class_sizes[i] = config->size_class_sizes[i];
            pool->size_classes[i].block_size = config->size_class_sizes[i];
            pool->size_classes[i].free_blocks = NULL;
            pool->size_classes[i].block_count = 0;
            pool->size_classes[i].used_count = 0;
        }
        pool->num_classes = classes_to_add;
    }

    set_error(POOL_OK);
    return pool;
}

// 销毁内存池
void memory_pool_destroy(memory_pool_t* pool) {
    if (!pool) return;

    if (pool->thread_safe) {
        pthread_mutex_destroy(&pool->mutex);
    }

    munmap(pool->pool_start, pool->pool_size);
    free(pool);
}

// 查找最佳适配块
static memory_block_t* find_best_fit(memory_pool_t* pool, size_t size) {
    memory_block_t* current = pool->free_list;
    memory_block_t* best_fit = NULL;
    memory_block_t* prev = NULL;
    memory_block_t* best_prev = NULL;
    size_t best_size = SIZE_MAX;

    while (current) {
        if (!validate_block(current)) {
            set_error(POOL_ERROR_CORRUPTION);
            return NULL;
        }

        if (current->size >= size) {
            // 精确匹配优先
            if (current->size == size) {
                if (prev) {
                    prev->next = current->next;
                } else {
                    pool->free_list = current->next;
                }
                return current;
            }
            
            // 寻找最小的合适块
            if (current->size < best_size) {
                best_fit = current;
                best_prev = prev;
                best_size = current->size;
            }
        }
        
        prev = current;
        current = current->next;
    }

    // 从链表中移除最佳匹配块
    if (best_fit) {
        if (best_prev) {
            best_prev->next = best_fit->next;
        } else {
            pool->free_list = best_fit->next;
        }
    }

    return best_fit;
}

// 分配内存
void* memory_pool_alloc(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    // 内存对齐
    size_t aligned_size = align_size(size + sizeof(memory_block_t), pool->alignment);
    
    // 确保最小块大小
    if (aligned_size < MIN_BLOCK_SIZE) {
        aligned_size = MIN_BLOCK_SIZE;
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    memory_block_t* block = find_best_fit(pool, aligned_size);
    if (!block) {
        if (pool->thread_safe) {
            pthread_mutex_unlock(&pool->mutex);
        }
        set_error(POOL_ERROR_OUT_OF_MEMORY);
        return NULL;
    }

    // 分割大块（避免内部碎片）
    size_t remaining_size = block->size - aligned_size;
    if (remaining_size >= MIN_BLOCK_SIZE) {
        memory_block_t* new_block = (memory_block_t*)((char*)block + aligned_size);
        new_block->size = remaining_size;
        new_block->magic = MAGIC_NUMBER;
        new_block->padding = 0;
        new_block->next = pool->free_list;
        pool->free_list = new_block;
        
        block->size = aligned_size;
    }

    pool->used_size += block->size;
    pool->alloc_count++;

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    set_error(POOL_OK);
    return (char*)block + sizeof(memory_block_t);
}

// 对齐分配
void* memory_pool_alloc_aligned(memory_pool_t* pool, size_t size, size_t alignment) {
    if (!pool || size == 0 || !is_power_of_two(alignment)) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    // 计算需要的总大小（包含额外的对齐空间）
    size_t total_size = size + alignment + sizeof(memory_block_t);
    void* raw_ptr = memory_pool_alloc(pool, total_size);
    
    if (!raw_ptr) {
        return NULL;
    }

    // 计算对齐后的地址
    uintptr_t aligned_addr = align_size((uintptr_t)raw_ptr, alignment);
    
    // 如果已经对齐，直接返回
    if ((void*)aligned_addr == raw_ptr) {
        return raw_ptr;
    }

    // 否则需要重新分配（简化实现）
    memory_pool_free(pool, raw_ptr);
    return memory_pool_alloc(pool, align_size(size, alignment));
}

// 分配并清零
void* memory_pool_calloc(memory_pool_t* pool, size_t count, size_t size) {
    if (!pool || count == 0 || size == 0) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    // 检查溢出
    if (count > SIZE_MAX / size) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    size_t total_size = count * size;
    void* ptr = memory_pool_alloc(pool, total_size);
    
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    
    return ptr;
}

// 插入空闲块到链表中（按地址排序，便于合并）
static void insert_free_block(memory_pool_t* pool, memory_block_t* block) {
    if (!pool->free_list || block < pool->free_list) {
        block->next = pool->free_list;
        pool->free_list = block;
        return;
    }

    memory_block_t* current = pool->free_list;
    while (current->next && current->next < block) {
        current = current->next;
    }
    
    block->next = current->next;
    current->next = block;
}

// 释放内存
void memory_pool_free(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) {
        set_error(POOL_ERROR_NULL_POINTER);
        return;
    }

    // 检查指针是否在池范围内
    if ((char*)ptr < (char*)pool->pool_start || 
        (char*)ptr >= (char*)pool->pool_start + pool->pool_size) {
        set_error(POOL_ERROR_INVALID_POINTER);
        return;
    }

    memory_block_t* block = (memory_block_t*)((char*)ptr - sizeof(memory_block_t));

    // 验证块的完整性
    if (!validate_block(block)) {
        set_error(POOL_ERROR_CORRUPTION);
        return;
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    pool->used_size -= block->size;
    pool->free_count++;

    // 清除next指针，确保块状态正确
    block->next = NULL;

    // 将块插入空闲链表
    insert_free_block(pool, block);

    // 立即合并相邻的空闲块
    merge_free_blocks(pool);

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    set_error(POOL_OK);
}

// 重新分配内存
void* memory_pool_realloc(memory_pool_t* pool, void* ptr, size_t new_size) {
    if (!pool) {
        set_error(POOL_ERROR_NULL_POINTER);
        return NULL;
    }

    if (!ptr) {
        return memory_pool_alloc(pool, new_size);
    }

    if (new_size == 0) {
        memory_pool_free(pool, ptr);
        return NULL;
    }

    // 获取当前块大小
    size_t old_size = memory_pool_get_block_size(pool, ptr);
    if (old_size == 0) {
        set_error(POOL_ERROR_INVALID_POINTER);
        return NULL;
    }

    // 如果新大小小于等于当前大小，直接返回
    size_t usable_old_size = old_size - sizeof(memory_block_t);
    if (new_size <= usable_old_size) {
        return ptr;
    }

    // 分配新内存
    void* new_ptr = memory_pool_alloc(pool, new_size);
    if (!new_ptr) {
        return NULL;
    }

    // 复制数据
    memcpy(new_ptr, ptr, usable_old_size);
    
    // 释放旧内存
    memory_pool_free(pool, ptr);
    
    return new_ptr;
}

// 重置内存池
void memory_pool_reset(memory_pool_t* pool) {
    if (!pool) return;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    // 重置统计信息
    pool->used_size = 0;
    pool->alloc_count = 0;
    pool->free_count = 0;
    pool->merge_count = 0;

    // 重置空闲链表
    memory_block_t* initial_block = (memory_block_t*)pool->pool_start;
    initial_block->next = NULL;
    initial_block->size = pool->pool_size;
    initial_block->magic = MAGIC_NUMBER;
    initial_block->padding = 0;
    pool->free_list = initial_block;

    // 重置固定大小池
    for (int i = 0; i < pool->num_classes; i++) {
        pool->size_classes[i].free_blocks = NULL;
        pool->size_classes[i].used_count = 0;
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

// 检查指针是否属于内存池
bool memory_pool_contains(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return false;
    
    return (char*)ptr >= (char*)pool->pool_start && 
           (char*)ptr < (char*)pool->pool_start + pool->pool_size;
}

// 获取块大小
size_t memory_pool_get_block_size(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr || !memory_pool_contains(pool, ptr)) {
        return 0;
    }

    memory_block_t* block = (memory_block_t*)((char*)ptr - sizeof(memory_block_t));
    
    if (!validate_block(block)) {
        return 0;
    }

    return block->size;
}

// 内存预热
void memory_pool_warmup(memory_pool_t* pool) {
    if (!pool) return;

    volatile char* ptr = (char*)pool->pool_start;
    
    for (size_t i = 0; i < pool->pool_size; i += PAGE_SIZE) {
        ptr[i] = 0;  // 触发页面加载
    }
}

// 内存碎片整理
void memory_pool_defragment(memory_pool_t* pool) {
    if (!pool) return;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    merge_free_blocks(pool);

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

// 合并空闲块
static void merge_free_blocks(memory_pool_t* pool) {
    if (!pool->free_list) return;

    int merge_attempts = 0;
    bool merged;
    do {
        merged = false;
        memory_block_t* current = pool->free_list;
        
        while (current && current->next) {
            char* current_end = (char*)current + current->size;
            char* next_start = (char*)current->next;
            
            if (current_end == next_start) {
                // 合并相邻块
                memory_block_t* next_block = current->next;
                current->size += next_block->size;
                current->next = next_block->next;
                pool->merge_count++;
                merged = true;
                merge_attempts++;
                // 继续检查当前块是否能与下一个块合并
            } else {
                current = current->next;
            }
        }
    } while (merged);  // 重复合并直到没有可合并的块
}

// 获取统计信息
void memory_pool_get_stats(memory_pool_t* pool, pool_stats_t* stats) {
    if (!pool || !stats) return;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    stats->total_size = pool->pool_size;
    stats->used_size = pool->used_size;
    stats->free_size = pool->pool_size - pool->used_size;
    stats->allocation_count = pool->alloc_count;
    stats->free_count = pool->free_count;
    stats->merge_count = pool->merge_count;

    // 计算最大空闲块和块数量
    stats->largest_free_block = 0;
    stats->free_block_count = 0;
    
    memory_block_t* current = pool->free_list;
    while (current) {
        stats->free_block_count++;
        if (current->size > stats->largest_free_block) {
            stats->largest_free_block = current->size;
        }
        current = current->next;
    }

    // 计算碎片率
    if (stats->free_size > 0) {
        size_t avg_block_size = stats->free_size / (stats->free_block_count + 1);
        stats->fragmentation_ratio = (stats->free_block_count * 100) / (avg_block_size / 64 + 1);
    } else {
        stats->fragmentation_ratio = 0;
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

// 打印统计信息
void memory_pool_print_stats(memory_pool_t* pool) {
    if (!pool) return;

    pool_stats_t stats;
    memory_pool_get_stats(pool, &stats);

    printf("=== Memory Pool Statistics ===\n");
    printf("Total Size:          %zu bytes\n", stats.total_size);
    printf("Used Size:           %zu bytes (%.1f%%)\n", 
           stats.used_size, (double)stats.used_size * 100.0 / stats.total_size);
    printf("Free Size:           %zu bytes (%.1f%%)\n", 
           stats.free_size, (double)stats.free_size * 100.0 / stats.total_size);
    printf("Largest Free Block:  %zu bytes\n", stats.largest_free_block);
    printf("Free Block Count:    %zu\n", stats.free_block_count);
    printf("Fragmentation Ratio: %zu%%\n", stats.fragmentation_ratio);
    printf("Allocations:         %lu\n", stats.allocation_count);
    printf("Frees:               %lu\n", stats.free_count);
    printf("Merges:              %lu\n", stats.merge_count);
    printf("================================\n");
}

// 验证内存池完整性
bool memory_pool_validate(memory_pool_t* pool) {
    if (!pool) return false;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    bool valid = true;
    size_t total_free = 0;

    memory_block_t* current = pool->free_list;
    while (current) {
        if (!validate_block(current)) {
            valid = false;
            break;
        }
        
        total_free += current->size;
        current = current->next;
    }

    // 检查大小一致性
    if (valid && (pool->used_size + total_free != pool->pool_size)) {
        valid = false;
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    return valid;
}

// 添加固定大小类别
int memory_pool_add_size_class(memory_pool_t* pool, size_t size, size_t count) {
    if (!pool || size == 0 || count == 0) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return -1;
    }

    if (pool->num_classes >= MAX_SIZE_CLASSES) {
        set_error(POOL_ERROR_OUT_OF_MEMORY);
        return -1;
    }

    // 对齐大小
    size_t aligned_size = align_size(size + sizeof(memory_block_t), pool->alignment);

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    int class_index = pool->num_classes;
    size_class_pool_t* class_pool = &pool->size_classes[class_index];
    
    class_pool->block_size = aligned_size;
    class_pool->block_count = count;
    class_pool->used_count = 0;
    class_pool->free_blocks = NULL;

    // 预分配固定大小的块（暂时释放锁以避免死锁）
    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    for (size_t i = 0; i < count; i++) {
        void* ptr = memory_pool_alloc(pool, size);
        if (!ptr) {
            // 分配失败，清理已分配的块
            if (pool->thread_safe) {
                pthread_mutex_lock(&pool->mutex);
            }
            
            memory_block_t* current = class_pool->free_blocks;
            while (current) {
                memory_block_t* next = current->next;
                if (pool->thread_safe) {
                    pthread_mutex_unlock(&pool->mutex);
                }
                memory_pool_free(pool, (char*)current + sizeof(memory_block_t));
                if (pool->thread_safe) {
                    pthread_mutex_lock(&pool->mutex);
                }
                current = next;
            }
            
            if (pool->thread_safe) {
                pthread_mutex_unlock(&pool->mutex);
            }
            return -1;
        }

        // 将分配的块加入固定大小池的空闲链表
        if (pool->thread_safe) {
            pthread_mutex_lock(&pool->mutex);
        }
        
        memory_block_t* block = (memory_block_t*)((char*)ptr - sizeof(memory_block_t));
        block->next = class_pool->free_blocks;
        class_pool->free_blocks = block;
        
        if (pool->thread_safe) {
            pthread_mutex_unlock(&pool->mutex);
        }
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    pool->class_sizes[class_index] = size;
    pool->num_classes++;

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    set_error(POOL_OK);
    return class_index;
}

// 从固定大小池分配
void* memory_pool_alloc_fixed(memory_pool_t* pool, size_t size) {
    if (!pool || size == 0) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    // 查找合适的大小类别
    for (int i = 0; i < pool->num_classes; i++) {
        if (size <= pool->class_sizes[i]) {
            size_class_pool_t* class_pool = &pool->size_classes[i];
            
            if (class_pool->free_blocks) {
                memory_block_t* block = class_pool->free_blocks;
                class_pool->free_blocks = block->next;
                class_pool->used_count++;
                
                if (pool->thread_safe) {
                    pthread_mutex_unlock(&pool->mutex);
                }
                
                set_error(POOL_OK);
                return (char*)block + sizeof(memory_block_t);
            }
            break;
        }
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    // 回退到主池分配
    return memory_pool_alloc(pool, size);
}

// 释放到固定大小池
void memory_pool_free_fixed(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) {
        set_error(POOL_ERROR_NULL_POINTER);
        return;
    }

    memory_block_t* block = (memory_block_t*)((char*)ptr - sizeof(memory_block_t));
    
    if (!validate_block(block)) {
        set_error(POOL_ERROR_CORRUPTION);
        return;
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    // 检查是否属于某个固定大小类别
    for (int i = 0; i < pool->num_classes; i++) {
        if (block->size == pool->size_classes[i].block_size) {
            size_class_pool_t* class_pool = &pool->size_classes[i];
            
            // 将块返回到固定大小池
            block->next = class_pool->free_blocks;
            class_pool->free_blocks = block;
            class_pool->used_count--;
            
            if (pool->thread_safe) {
                pthread_mutex_unlock(&pool->mutex);
            }
            
            set_error(POOL_OK);
            return;
        }
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    // 不属于固定大小池，使用普通释放
    memory_pool_free(pool, ptr);
}
