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
static void merge_free_blocks(memory_pool_t* pool);
static bool validate_block(memory_block_t* block);
static void insert_free_block(memory_pool_t* pool, memory_block_t* block);
static memory_pool_t* create_child_pool(memory_pool_t* root, size_t min_size);
static memory_block_t* find_best_fit_chain(memory_pool_t* root, memory_pool_t** owner_pool, size_t size);
static bool pool_contains(memory_pool_t* pool, void* ptr);

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
    bool ok = block && block->magic == MAGIC_NUMBER && block->size >= sizeof(memory_block_t);
    if (!ok) {
        MP_LOG("invalid block blk=%p size=%zu magic=%08x", (void*)block, block ? (size_t)block->size : 0, block ? block->magic : 0);
    }
    return ok;
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
    pool->num_classes = 0;
    pool->next = NULL;

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
    MP_LOG("create pool %p size=%zu align=%u", (void*)pool, pool->pool_size, pool->alignment);

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
// 创建子池（至少 min_size，向上取整到页）
static memory_pool_t* create_child_pool(memory_pool_t* root, size_t min_size) {
    pool_config_t cfg = {
        .pool_size = (min_size < root->pool_size) ? root->pool_size : min_size,
        .thread_safe = root->thread_safe,
        .alignment = root->alignment,
        .enable_size_classes = false,
        .size_class_sizes = NULL,
        .num_size_classes = 0
    };
    memory_pool_t* child = memory_pool_create_with_config(&cfg);
    if (!child) return NULL;
    // 挂到链尾
    memory_pool_t* p = root;
    while (p->next) p = p->next;
    p->next = child;
    return child;
}

// 链式查找最佳适配块，返回块与其所属池
static memory_block_t* find_best_fit_chain(memory_pool_t* root, memory_pool_t** owner_pool, size_t size) {
    memory_pool_t* pool = root;
    memory_block_t* best_block = NULL;
    memory_pool_t* best_owner = NULL;
    size_t best_size = SIZE_MAX;

    while (pool) {
        memory_block_t* current = pool->free_list;
        memory_block_t* prev = NULL;
        while (current) {
            if (!validate_block(current)) return NULL;
            if (current->size >= size) {
                if (current->size == size) {
                    // 精确匹配：从该池直接摘除
                    if (prev) prev->next = current->next; else pool->free_list = current->next;
                    *owner_pool = pool;
                    MP_LOG("best-fit exact from %p blk=%p size=%zu", (void*)pool, (void*)current, (size_t)current->size);
                    return current;
                }
                if (current->size < best_size) {
                    best_block = current;
                    best_owner = pool;
                    best_size = current->size;
                }
            }
            prev = current;
            current = current->next;
        }
        pool = pool->next;
    }

    if (best_block && best_owner) {
        // 在best_owner中再次遍历，摘除best_block
        memory_block_t* current = best_owner->free_list;
        memory_block_t* prev = NULL;
    while (current) {
            if (current == best_block) {
                if (prev) prev->next = current->next; else best_owner->free_list = current->next;
                *owner_pool = best_owner;
        MP_LOG("best-fit from %p blk=%p size=%zu", (void*)best_owner, (void*)best_block, (size_t)best_block->size);
                return best_block;
            }
            prev = current;
            current = current->next;
        }
    }
    return NULL;
}

static bool pool_contains(memory_pool_t* pool, void* ptr) {
    return (char*)ptr >= (char*)pool->pool_start && 
           (char*)ptr < (char*)pool->pool_start + pool->pool_size;
}

// 销毁内存池
void memory_pool_destroy(memory_pool_t* pool) {
    if (!pool) return;
    memory_pool_t* p = pool;
    while (p) {
        memory_pool_t* next = p->next;
        if (p->thread_safe) {
            pthread_mutex_destroy(&p->mutex);
        }
        munmap(p->pool_start, p->pool_size);
        free(p);
        p = next;
    }
}

// （已移除旧的 find_best_fit 实现，改用 find_best_fit_chain）

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

    memory_pool_t* owner = pool;
    memory_block_t* block = find_best_fit_chain(pool, &owner, aligned_size);
    if (!block) {
        // 当前链都不足，创建子池
        if (pool->thread_safe) {
            pthread_mutex_unlock(&pool->mutex);
        }
        memory_pool_t* child = create_child_pool(pool, aligned_size);
    if (!child) {
            set_error(POOL_ERROR_OUT_OF_MEMORY);
            return NULL;
        }
        if (pool->thread_safe) {
            pthread_mutex_lock(&pool->mutex);
        }
        owner = child;
        block = find_best_fit_chain(child, &owner, aligned_size);
        if (!block) {
            if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
            set_error(POOL_ERROR_OUT_OF_MEMORY);
            return NULL;
        }
    }

    // 分割大块（避免内部碎片）
    size_t remaining_size = block->size - aligned_size;
    if (remaining_size >= MIN_BLOCK_SIZE) {
        memory_block_t* new_block = (memory_block_t*)((char*)block + aligned_size);
        new_block->size = remaining_size;
        new_block->magic = MAGIC_NUMBER;
        new_block->padding = 0;
    new_block->next = owner->free_list;
    owner->free_list = new_block;
        
        block->size = aligned_size;
    }

    owner->used_size += block->size;
    MP_LOG("alloc pool=%p user=%p size=%zu (blk=%zu)", (void*)owner, (void*)((char*)block + sizeof(memory_block_t)), (size_t)(aligned_size - sizeof(memory_block_t)), (size_t)block->size);

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    set_error(POOL_OK);
    return (char*)block + sizeof(memory_block_t);
}

// 对齐分配：通过在链上寻找足够大的块，切分出对齐后的使用块，并将前后余留重新挂回空闲链
void* memory_pool_alloc_aligned(memory_pool_t* pool, size_t size, size_t alignment) {
    if (!pool || size == 0 || !is_power_of_two(alignment)) {
        set_error(POOL_ERROR_INVALID_SIZE);
        return NULL;
    }

    // 使用块总大小（包含头部），并按池对齐
    size_t used_total = align_size(size + sizeof(memory_block_t), pool->alignment);
    // 需要预留最多 alignment 字节作为前缀填充
    size_t min_needed = used_total + alignment;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    memory_pool_t* owner = pool;
    memory_block_t* block = find_best_fit_chain(pool, &owner, min_needed);
    if (!block) {
        // 创建子池后重试
        if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
        memory_pool_t* child = create_child_pool(pool, min_needed);
        if (!child) { set_error(POOL_ERROR_OUT_OF_MEMORY); return NULL; }
        if (pool->thread_safe) pthread_mutex_lock(&pool->mutex);
        owner = child;
        block = find_best_fit_chain(child, &owner, min_needed);
        if (!block) {
            if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
            set_error(POOL_ERROR_OUT_OF_MEMORY);
            return NULL;
        }
    }

    // 计算对齐后的用户指针与对齐块头位置
    char* raw = (char*)block;
    char* user_min = raw + sizeof(memory_block_t);
    uintptr_t aligned_user_addr = align_size((uintptr_t)user_min, alignment);
    memory_block_t* aligned_block = (memory_block_t*)((char*)aligned_user_addr - sizeof(memory_block_t));

    // 确保前缀块大小要么为0要么 >= MIN_BLOCK_SIZE；不足则前移到下一个对齐位置
    size_t prefix = (size_t)((char*)aligned_block - raw);
    if (prefix > 0 && prefix < MIN_BLOCK_SIZE) {
        uintptr_t bumped = align_size((uintptr_t)user_min + (MIN_BLOCK_SIZE - prefix), alignment);
        aligned_block = (memory_block_t*)((char*)bumped - sizeof(memory_block_t));
        prefix = (size_t)((char*)aligned_block - raw);
    }

    // 使用块大小：至少 used_total，尾部不够 MIN_BLOCK_SIZE 则并入使用块
    size_t suffix = block->size - prefix - used_total;
    if ((long)suffix < 0) {
        // 由于对齐调整导致空间不足，扩大使用块到可行范围
        used_total = block->size - prefix;
        suffix = 0;
    }
    if (suffix > 0 && suffix < MIN_BLOCK_SIZE) {
        used_total += suffix;
        suffix = 0;
        // 再次按池对齐
        used_total = align_size(used_total, pool->alignment);
    }

    // 前缀回收
    if (prefix >= MIN_BLOCK_SIZE) {
        memory_block_t* pre = (memory_block_t*)raw;
        pre->size = prefix;
        pre->magic = MAGIC_NUMBER;
        pre->padding = 0;
        pre->next = NULL;
        insert_free_block(owner, pre);
    }

    // 设置对齐后的使用块头
    aligned_block->size = used_total;
    aligned_block->magic = MAGIC_NUMBER;
    aligned_block->padding = 0;
    aligned_block->next = NULL;

    // 尾部回收
    if (suffix >= MIN_BLOCK_SIZE) {
        memory_block_t* suf = (memory_block_t*)((char*)aligned_block + used_total);
        suf->size = suffix;
        suf->magic = MAGIC_NUMBER;
        suf->padding = 0;
        suf->next = NULL;
        insert_free_block(owner, suf);
    }

    owner->used_size += used_total;
    MP_LOG("alloc_aligned pool=%p user=%p size=%zu align=%zu used_total=%zu", (void*)owner, (void*)((char*)aligned_block + sizeof(memory_block_t)), (size_t)size, (size_t)alignment, (size_t)used_total);

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    set_error(POOL_OK);
    return (char*)aligned_block + sizeof(memory_block_t);
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
    // 找到所属池
    memory_pool_t* owner = pool;
    while (owner && !pool_contains(owner, ptr)) owner = owner->next;
    if (!owner) { set_error(POOL_ERROR_INVALID_POINTER); return; }

    memory_block_t* block = (memory_block_t*)((char*)ptr - sizeof(memory_block_t));

    // 验证块的完整性
    if (!validate_block(block)) {
        set_error(POOL_ERROR_CORRUPTION);
        return;
    }

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    owner->used_size -= block->size;
    MP_LOG("free pool=%p user=%p blk_size=%zu", (void*)owner, ptr, (size_t)block->size);

    // 清除next指针，确保块状态正确
    block->next = NULL;

    // 将块插入空闲链表
    insert_free_block(owner, block);

    // 立即合并相邻的空闲块
    merge_free_blocks(owner);

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

    // 遍历整条链路重置
    memory_pool_t* p = pool;
    while (p) {
        p->used_size = 0;
        memory_block_t* initial_block = (memory_block_t*)p->pool_start;
        initial_block->next = NULL;
        initial_block->size = p->pool_size;
        initial_block->magic = MAGIC_NUMBER;
        initial_block->padding = 0;
    p->free_list = initial_block;
    MP_LOG("reset pool=%p size=%zu", (void*)p, p->pool_size);
        for (int i = 0; i < p->num_classes; i++) {
            p->size_classes[i].free_blocks = NULL;
            p->size_classes[i].used_count = 0;
        }
        p = p->next;
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

// 检查指针是否属于内存池
bool memory_pool_contains(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return false;
    memory_pool_t* p = pool;
    while (p) {
        if (pool_contains(p, ptr)) return true;
        p = p->next;
    }
    return false;
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
    memory_pool_t* p = pool;
    while (p) {
        volatile char* ptr = (char*)p->pool_start;
        for (size_t i = 0; i < p->pool_size; i += PAGE_SIZE) {
            ptr[i] = 0;
        }
        p = p->next;
    }
}

// 内存碎片整理
void memory_pool_defragment(memory_pool_t* pool) {
    if (!pool) return;
    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }
    memory_pool_t* p = pool;
    while (p) {
    merge_free_blocks(p);
    MP_LOG("defragment pool=%p", (void*)p);
        p = p->next;
    }
    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
}

// 合并空闲块
static void merge_free_blocks(memory_pool_t* pool) {
    if (!pool->free_list) return;

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
                merged = true;
                // 继续检查当前块是否能与下一个块合并
            } else {
                current = current->next;
            }
        }
    } while (merged);  // 重复合并直到没有可合并的块
}

// 验证内存池完整性
bool memory_pool_validate(memory_pool_t* pool) {
    if (!pool) return false;

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    memory_pool_t* p = pool;
    while (p) {
        bool valid = true;
        size_t total_free = 0;
        memory_block_t* current = p->free_list;
        while (current) {
            if (!validate_block(current)) { valid = false; break; }
            total_free += current->size;
            current = current->next;
        }
        if (!(valid && (p->used_size + total_free == p->pool_size))) {
            if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
            return false;
        }
        p = p->next;
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }
    return true;
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
