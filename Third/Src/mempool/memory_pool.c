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
// RB-tree (按 size, 次键地址) 管理空闲块，O(log n) best-fit
static void rb_insert(memory_pool_t* pool, memory_block_t* node);
static void rb_remove(memory_pool_t* pool, memory_block_t* node);
static memory_block_t* rb_find_best_fit(memory_pool_t* pool, size_t size, memory_pool_t** owner_pool);
static void rb_init_node(memory_block_t* n) { n->rb_left = n->rb_right = n->rb_parent = NULL; n->rb_color = 0; }

// 旋转与修复
static void rb_left_rotate(memory_pool_t* pool, memory_block_t* x) {
    pool = pool->master ? pool->master : pool;
    memory_block_t* y = x->rb_right;
    x->rb_right = y->rb_left;
    if (y->rb_left) y->rb_left->rb_parent = x;
    y->rb_parent = x->rb_parent;
    if (!x->rb_parent) pool->rb_root = y;
    else if (x == x->rb_parent->rb_left) x->rb_parent->rb_left = y; else x->rb_parent->rb_right = y;
    y->rb_left = x;
    x->rb_parent = y;
}
static void rb_right_rotate(memory_pool_t* pool, memory_block_t* y) {
    pool = pool->master ? pool->master : pool;
    memory_block_t* x = y->rb_left;
    y->rb_left = x->rb_right;
    if (x->rb_right) x->rb_right->rb_parent = y;
    x->rb_parent = y->rb_parent;
    if (!y->rb_parent) pool->rb_root = x;
    else if (y == y->rb_parent->rb_left) y->rb_parent->rb_left = x; else y->rb_parent->rb_right = x;
    x->rb_right = y; y->rb_parent = x;
}
static int rb_cmp(memory_block_t* a, memory_block_t* b) {
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}
static void rb_insert(memory_pool_t* pool, memory_block_t* z) {
    pool = pool->master ? pool->master : pool;
    rb_init_node(z);
    memory_block_t* y = NULL; memory_block_t* x = pool->rb_root;
    while (x) { y = x; int c = rb_cmp(z, x); x = (c < 0) ? x->rb_left : x->rb_right; }
    z->rb_parent = y;
    if (!y) pool->rb_root = z;
    else if (rb_cmp(z, y) < 0) y->rb_left = z; else y->rb_right = z;
    // fixup
    z->rb_color = 0; // red
    while (z != pool->rb_root && z->rb_parent->rb_color == 0) {
        memory_block_t* p = z->rb_parent; memory_block_t* g = p->rb_parent;
        if (!g) break;
        if (p == g->rb_left) {
            memory_block_t* u = g->rb_right;
            if (u && u->rb_color == 0) { p->rb_color = 1; u->rb_color = 1; g->rb_color = 0; z = g; }
            else {
                if (z == p->rb_right) { z = p; rb_left_rotate(pool, z); p = z->rb_parent; g = p? p->rb_parent:NULL; }
                p->rb_color = 1; if (g) { g->rb_color = 0; rb_right_rotate(pool, g); }
            }
        } else {
            memory_block_t* u = g->rb_left;
            if (u && u->rb_color == 0) { p->rb_color = 1; u->rb_color = 1; g->rb_color = 0; z = g; }
            else {
                if (z == p->rb_left) { z = p; rb_right_rotate(pool, z); p = z->rb_parent; g = p? p->rb_parent:NULL; }
                p->rb_color = 1; if (g) { g->rb_color = 0; rb_left_rotate(pool, g); }
            }
        }
    }
    pool->rb_root->rb_color = 1; // root black
}
static memory_block_t* rb_min(memory_block_t* n) { while (n && n->rb_left) n = n->rb_left; return n; }
static void rb_transplant(memory_pool_t* pool, memory_block_t* u, memory_block_t* v) {
    if (!u->rb_parent) pool->rb_root = v;
    else if (u == u->rb_parent->rb_left) u->rb_parent->rb_left = v; else u->rb_parent->rb_right = v;
    if (v) v->rb_parent = u->rb_parent;
}
static void rb_remove(memory_pool_t* pool, memory_block_t* z) {
    pool = pool->master ? pool->master : pool;
    // 简单存在性检查：自 root 向下按比较寻找 z
    memory_block_t* probe = pool->rb_root; bool found=false; while (probe) { int c=rb_cmp(z, probe); if (c==0) { if (probe==z) found=true; break; } probe = (c<0)?probe->rb_left:probe->rb_right; }
    if (!found) { MP_LOG("rb_remove skip: node %p not in tree", (void*)z); return; }
    memory_block_t* y = z; unsigned char y_original = y->rb_color; memory_block_t* x = NULL; memory_block_t* x_parent = NULL;
    if (!z->rb_left) { x = z->rb_right; rb_transplant(pool, z, z->rb_right); x_parent = z->rb_parent; }
    else if (!z->rb_right) { x = z->rb_left; rb_transplant(pool, z, z->rb_left); x_parent = z->rb_parent; }
    else {
        y = rb_min(z->rb_right); y_original = y->rb_color; x = y->rb_right; if (y->rb_parent == z) { if (x) x->rb_parent = y; x_parent = y; } else { rb_transplant(pool, y, y->rb_right); y->rb_right = z->rb_right; y->rb_right->rb_parent = y; x_parent = y->rb_parent; }
        rb_transplant(pool, z, y); y->rb_left = z->rb_left; y->rb_left->rb_parent = y; y->rb_color = z->rb_color;
    }
    if (y_original == 1) { // fix double-black
        while ((x != pool->rb_root) && (!x || x->rb_color == 1)) {
            if (x_parent && x == x_parent->rb_left) {
                memory_block_t* w = x_parent->rb_right;
                if (!w) { x = x_parent; x_parent = x_parent->rb_parent; continue; }
                if (w && w->rb_color == 0) { w->rb_color = 1; x_parent->rb_color = 0; rb_left_rotate(pool, x_parent); w = x_parent->rb_right; }
                if ((!w->rb_left || w->rb_left->rb_color == 1) && (!w->rb_right || w->rb_right->rb_color == 1)) { if (w) w->rb_color = 0; x = x_parent; x_parent = x_parent->rb_parent; }
                else {
                    if (!w->rb_right || w->rb_right->rb_color == 1) {
                        if (w->rb_left) w->rb_left->rb_color = 1;
                        w->rb_color = 0;
                        rb_right_rotate(pool, w);
                        w = x_parent->rb_right;
                    }
                    if (w) {
                        w->rb_color = x_parent->rb_color;
                    }
                    x_parent->rb_color = 1;
                    if (w && w->rb_right) w->rb_right->rb_color = 1;
                    rb_left_rotate(pool, x_parent);
                    x = pool->rb_root;
                    break; }
            } else if (x_parent) {
                memory_block_t* w = x_parent->rb_left;
                if (!w) { x = x_parent; x_parent = x_parent->rb_parent; continue; }
                if (w && w->rb_color == 0) { w->rb_color = 1; x_parent->rb_color = 0; rb_right_rotate(pool, x_parent); w = x_parent->rb_left; }
                if ((!w->rb_left || w->rb_left->rb_color == 1) && (!w->rb_right || w->rb_right->rb_color == 1)) { if (w) w->rb_color = 0; x = x_parent; x_parent = x_parent->rb_parent; }
                else {
                    if (!w->rb_left || w->rb_left->rb_color == 1) {
                        if (w->rb_right) w->rb_right->rb_color = 1;
                        w->rb_color = 0;
                        rb_left_rotate(pool, w);
                        w = x_parent->rb_left;
                    }
                    if (w) {
                        w->rb_color = x_parent->rb_color;
                    }
                    x_parent->rb_color = 1;
                    if (w && w->rb_left) w->rb_left->rb_color = 1;
                    rb_right_rotate(pool, x_parent);
                    x = pool->rb_root;
                    break; }
            } else break;
        }
        if (x) x->rb_color = 1;
    }
}
static memory_block_t* rb_find_best_fit(memory_pool_t* root, size_t size, memory_pool_t** owner_pool) {
    if (!root) return NULL;
    memory_pool_t* master = root->master ? root->master : root;
    memory_block_t* cur = master->rb_root; memory_block_t* candidate = NULL;
    while (cur) {
        if (cur->size == size) { candidate = cur; break; }
        if (cur->size > size) { candidate = cur; cur = cur->rb_left; } else cur = cur->rb_right;
    }
    if (!candidate) return NULL;
    // 找到后需确定其所属池：通过遍历链判断地址范围
    memory_pool_t* p = master; while (p) { if ((char*)candidate >= (char*)p->pool_start && (char*)candidate < (char*)p->pool_start + p->pool_size) { *owner_pool = p; break; } p = p->next; }
    rb_remove(master, candidate);
    candidate->flags &= ~MB_FLAG_FREE;
    return candidate;
}
static bool pool_contains(memory_pool_t* pool, void* ptr);
// 新增：物理相邻辅助与元数据维护
static inline memory_block_t* next_physical_block(memory_pool_t* pool, memory_block_t* blk);
static inline void set_next_prev_free(memory_pool_t* pool, memory_block_t* free_blk);
static inline void clear_next_prev_free(memory_pool_t* pool, memory_block_t* blk);
static void remove_free_block(memory_pool_t* pool, memory_block_t* block);

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

// 物理后继块（可能跨越到池末尾则返回 NULL）
static inline memory_block_t* next_physical_block(memory_pool_t* pool, memory_block_t* blk) {
    if (!blk) return NULL;
    char* base = (char*)pool->pool_start;
    char* end  = base + pool->pool_size;
    char* next = (char*)blk + blk->size;
    if (next >= end) return NULL;
    return (memory_block_t*)next;
}

// 在 free_blk 已经是自由块后，设置其后继块的 PREV_FREE 元数据
static inline void set_next_prev_free(memory_pool_t* pool, memory_block_t* free_blk) {
    memory_block_t* nxt = next_physical_block(pool, free_blk);
    if (!nxt) return;
    // 只有在后继块不是空闲列表中的 size-class 专属块时才安全；暂未区分，保持通用逻辑
    nxt->flags |= MB_FLAG_PREV_FREE;
    // prev_size 仅在后继块“当前不在通用 free_list”或者需要反向合并时使用
    nxt->u.prev_size = (uint32_t)free_blk->size; // 若 >4G 会截断（当前设计限制）
}

static inline void clear_next_prev_free(memory_pool_t* pool, memory_block_t* blk) {
    memory_block_t* nxt = next_physical_block(pool, blk);
    if (!nxt) return;
    nxt->flags &= ~MB_FLAG_PREV_FREE;
}

// 从空闲链表中移除（地址排序的单链表）
static void remove_free_block(memory_pool_t* pool, memory_block_t* block) {
    if (!pool->free_list || !block) return;
    memory_pool_t* master = pool->master ? pool->master : pool;
    MP_ASSERT(block->flags & MB_FLAG_FREE, "remove_free_block: block not marked FREE");
    if (pool->free_list == block) {
        pool->free_list = block->u.next;
        if (block->flags & MB_FLAG_FREE) rb_remove(master, block);
        return;    
    }
    memory_block_t* cur = pool->free_list;
    while (cur->u.next && cur->u.next != block) cur = cur->u.next;
    if (cur->u.next == block) {
        cur->u.next = block->u.next;
        if (block->flags & MB_FLAG_FREE) rb_remove(master, block);
    } else {
        MP_LOG("remove_free_block: target %p not found in address list pool=%p", (void*)block, (void*)pool);
    }
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
    pool->master = pool; // self master

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
    initial_block->u.next = NULL;
    initial_block->size = pool->pool_size;
    initial_block->magic = MAGIC_NUMBER;
    initial_block->flags = MB_FLAG_FREE;
    initial_block->rb_left = initial_block->rb_right = initial_block->rb_parent = NULL; initial_block->rb_color = 1; // root black
    pool->free_list = initial_block;
    pool->rb_root = initial_block; // only master uses
    MP_LOG("create pool %p size=%zu align=%u", (void*)pool, pool->pool_size, pool->alignment);

    // 初始化固定大小池
    if (config->enable_size_classes && config->size_class_sizes && config->num_size_classes > 0) {
        int classes_to_add = config->num_size_classes < MAX_SIZE_CLASSES ? 
                           config->num_size_classes : MAX_SIZE_CLASSES;
        
        for (int i = 0; i < classes_to_add; i++) {
            // 记录用户尺寸阈值
            pool->class_sizes[i] = config->size_class_sizes[i];
            // 注意：block_size 存储内部使用的“对齐后且含头部”的块大小，
            // 以便 free_fixed 能够用 block->size 做精确匹配。
            pool->size_classes[i].block_size = align_size(config->size_class_sizes[i] + sizeof(memory_block_t), pool->alignment);
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
    // 子池继承 master，不自建 rb_root
    memory_pool_t* master = root->master ? root->master : root;
    child->master = master;
    // 原创建函数把自身 initial_block 设为 rb_root，需要转接到 master 的树
    memory_block_t* initial_block = (memory_block_t*)child->pool_start;
    // 清理其 rb 链接后插入 master
    initial_block->rb_left = initial_block->rb_right = initial_block->rb_parent = NULL;
    initial_block->rb_color = 0; // will be recolored in insert
    child->rb_root = NULL;
    rb_insert(master, initial_block);
    // 挂到链尾
    memory_pool_t* p = root;
    while (p->next) p = p->next;
    p->next = child;
    return child;
}

// 链式查找最佳适配块，返回块与其所属池
static memory_block_t* find_best_fit_chain(memory_pool_t* root, memory_pool_t** owner_pool, size_t size) {
    memory_block_t* blk = rb_find_best_fit(root, size, owner_pool);
    if (!blk) return NULL; // 仅使用红黑树，不再线性回退
    memory_pool_t* p = *owner_pool;
    memory_block_t* cur = p->free_list; memory_block_t* prev = NULL;
    while (cur) { if (cur == blk) { if (prev) prev->u.next = cur->u.next; else p->free_list = cur->u.next; break; } prev = cur; cur = cur->u.next; }
    MP_LOG("best-fit(rb) from %p blk=%p size=%zu", (void*)*owner_pool, (void*)blk, (size_t)blk->size);
    return blk;
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
        // 先尝试在整条链上整理合并空闲块，再次尝试分配
        memory_pool_t* p = pool;
        while (p) { merge_free_blocks(p); p = p->next; }
        owner = pool;
        block = find_best_fit_chain(pool, &owner, aligned_size);
    }
    if (!block) {
        // 仍不足，则创建子池
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
    new_block->flags = 0; // will be set FREE by insert_free_block
    new_block->u.next = NULL;
        block->size = aligned_size;
    insert_free_block(owner, new_block); // 插入全局结构 (包含 RB)
    set_next_prev_free(owner, new_block); // 更新其后继 PREV_FREE
    new_block->flags &= ~MB_FLAG_PREV_FREE; // 前驱为已分配块
    } else {
        // 没有剩余自由块，清除后继块 PREV_FREE（前驱变为已分配）
        clear_next_prev_free(owner, block);
    }
    block->flags &= ~MB_FLAG_FREE; // 已分配

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
        // 先在整条链上合并空闲块再试一次
        memory_pool_t* p = pool;
        while (p) { merge_free_blocks(p); p = p->next; }
        owner = pool;
        block = find_best_fit_chain(pool, &owner, min_needed);
    }
    if (!block) {
        // 仍无则创建子池后重试
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
        pre->flags = MB_FLAG_FREE;
        pre->u.next = NULL;
        insert_free_block(owner, pre);
        set_next_prev_free(owner, pre);
        pre->flags &= ~MB_FLAG_PREV_FREE; // 物理首块或其前驱不一定空闲
    }

    // 设置对齐后的使用块头
    aligned_block->size = used_total;
    aligned_block->magic = MAGIC_NUMBER;
    aligned_block->flags &= ~MB_FLAG_FREE; // allocated
    if (prefix >= MIN_BLOCK_SIZE) {
        aligned_block->flags |= MB_FLAG_PREV_FREE;
        aligned_block->u.prev_size = (uint32_t)((memory_block_t*)raw)->size;
    } else {
        aligned_block->flags &= ~MB_FLAG_PREV_FREE;
    }
    aligned_block->u.next = NULL;

    // 尾部回收
    if (suffix >= MIN_BLOCK_SIZE) {
        memory_block_t* suf = (memory_block_t*)((char*)aligned_block + used_total);
        suf->size = suffix;
        suf->magic = MAGIC_NUMBER;
        suf->flags = MB_FLAG_FREE;
        suf->u.next = NULL;
        insert_free_block(owner, suf);
        set_next_prev_free(owner, suf);
    }
    else {
        // 没有尾部自由块，则需要清理下一物理块的 PREV_FREE（前驱已分配 & 无新自由块）
        clear_next_prev_free(owner, aligned_block);
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
    if (block->flags & MB_FLAG_SIZECLASS) {
        // size-class 块不进入通用空闲链
        return;
    }
    // 标记为空闲（通用）
    block->flags |= MB_FLAG_FREE;
    // 插入主池 RB 树（按 size 排序）
    memory_pool_t* master = pool->master ? pool->master : pool;
    rb_insert(master, block);
    if (!pool->free_list || block < pool->free_list) {
        block->u.next = pool->free_list;
        pool->free_list = block;
        return;
    }
    memory_block_t* current = pool->free_list;
    while (current->u.next && current->u.next < block) {
        current = current->u.next;
    }
    block->u.next = current->u.next;
    current->u.next = block;
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

    // 若为 size-class 块，改用 fixed 释放逻辑（不触发合并）
    if (block->flags & MB_FLAG_SIZECLASS) {
        if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
        memory_pool_free_fixed(owner, ptr);
        return;
    }

    // 双重释放检测（仅适用于通用 free；固定大小池内部释放由 free_fixed）
    if (block->flags & MB_FLAG_FREE) {
        if (pool->thread_safe) pthread_mutex_unlock(&pool->mutex);
        set_error(POOL_ERROR_DOUBLE_FREE);
        MP_LOG("double free detected blk=%p", (void*)block);
        return;
    }
    owner->used_size -= block->size;
    MP_LOG("free pool=%p user=%p blk_size=%zu", (void*)owner, ptr, (size_t)block->size);

    // 重写合并逻辑：先计算最终合并后的块大小，再一次性插入空闲结构（避免红黑树中途 size 变化破坏有序性）
    memory_block_t* base = block; // 最终要插入的块
    // bool merged_backward = false; // 已不再需要
    if (block->flags & MB_FLAG_PREV_FREE) {
        memory_block_t* prev = (memory_block_t*)((char*)block - block->u.prev_size);
        if (validate_block(prev) && (prev->flags & MB_FLAG_FREE) && (char*)prev + prev->size == (char*)block) {
            // 从自由结构中移除 prev（它本就在 free_list 和 RB 中）
            MP_LOG("free coalesce backward prev=%p size=%zu with blk=%p size=%zu", (void*)prev, (size_t)prev->size, (void*)block, (size_t)block->size);
            remove_free_block(owner, prev);
            prev->size += block->size;
            base = prev;
        } else {
            block->flags &= ~MB_FLAG_PREV_FREE; // 清理无效标记，按未合并处理
        }
    }

    // 向前（后继）合并：不断吸收紧邻的自由块
    while (1) {
        memory_block_t* nxt = next_physical_block(owner, base);
        if (!nxt || (nxt->flags & MB_FLAG_SIZECLASS) || !(nxt->flags & MB_FLAG_FREE) || (char*)base + base->size != (char*)nxt) break;
        remove_free_block(owner, nxt); // detach nxt (包括 RB)
    MP_LOG("free coalesce forward base=%p new_size=%zu absorb nxt=%p size=%zu", (void*)base, (size_t)(base->size + nxt->size), (void*)nxt, (size_t)nxt->size);
        base->size += nxt->size;
    }

    // 现在 base 还未在 RB/链表内（若 backward 合并则已移除；若未 backward 合并则是新释放块，不在结构中）
    base->flags |= MB_FLAG_FREE;
    base->flags &= ~MB_FLAG_PREV_FREE; // 自身作为自由块不需要该标记
    base->u.next = NULL;
    insert_free_block(owner, base); // 一次性按新 size 插入
    set_next_prev_free(owner, base); // 设置其后继的 PREV_FREE

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
    // 直接释放旧块（若为 size-class 将自动回到其私有空闲链）
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
        initial_block->u.next = NULL;
        initial_block->size = p->pool_size;
        initial_block->magic = MAGIC_NUMBER;
        initial_block->flags = MB_FLAG_FREE;
        p->free_list = initial_block;
        if (p == pool->master) {
            // 重建 master 根（先清空 rb_root）
            p->rb_root = NULL;
            initial_block->rb_left = initial_block->rb_right = initial_block->rb_parent = NULL; initial_block->rb_color = 0;
            rb_insert(p, initial_block); // becomes root
        } else {
            // 将子池初始块插入 master 的树
            initial_block->rb_left = initial_block->rb_right = initial_block->rb_parent = NULL; initial_block->rb_color = 0;
            rb_insert(pool->master, initial_block);
        }
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
    memory_pool_t* master = pool->master ? pool->master : pool;
    memory_block_t* current = pool->free_list;
    while (current) {
        bool did_merge = false;
        while (current->u.next && (char*)current + current->size == (char*)current->u.next) {
            memory_block_t* next_block = current->u.next;
            rb_remove(master, next_block);
            current->u.next = next_block->u.next;
            if (!did_merge) { rb_remove(master, current); did_merge = true; }
            current->size += next_block->size;
        }
        if (did_merge) {
            rb_insert(master, current);
            set_next_prev_free(pool, current);
        }
        current = current->u.next;
    }
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
            current = current->u.next;
        }
    if (!(valid && (p->used_size + total_free == p->pool_size))) {
#if MP_DEBUG
        size_t dbg_total=0; size_t count=0; memory_block_t* c2=p->free_list;
        while(c2){ dbg_total+=c2->size; count++; c2=c2->u.next; }
        MP_LOG("validate fail pool=%p used=%zu free_sum=%zu expect=%zu blocks=%zu", (void*)p, p->used_size, dbg_total, p->pool_size, count);
#endif
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
                memory_block_t* next = current->u.next;
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
    // 预留给 size-class，自有空闲链：仅打 SIZECLASS 标记，不加入通用 free_list
    block->flags &= ~MB_FLAG_FREE; // 确保未被视为通用空闲
    block->flags |= MB_FLAG_SIZECLASS;
    block->u.next = class_pool->free_blocks; // 复用 u.next 作为 size-class 单链表
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

#if MP_DEBUG
    // 建议：固定大小 API 应在拥有 size_classes 的“主池”上调用
    MP_ASSERT(pool->num_classes >= 0 && pool->num_classes <= MAX_SIZE_CLASSES, "invalid num_classes");
#endif

    if (pool->thread_safe) {
        pthread_mutex_lock(&pool->mutex);
    }

    // 查找合适的大小类别
    for (int i = 0; i < pool->num_classes; i++) {
        if (size <= pool->class_sizes[i]) {
            size_class_pool_t* class_pool = &pool->size_classes[i];
            
            if (class_pool->free_blocks) {
                memory_block_t* block = class_pool->free_blocks;
                class_pool->free_blocks = block->u.next;
                block->flags &= ~MB_FLAG_FREE; // allocated to user (size-class)
                block->flags |= MB_FLAG_SIZECLASS; // keep classification
                class_pool->used_count++;
                
                if (pool->thread_safe) {
                    pthread_mutex_unlock(&pool->mutex);
                }
                
                set_error(POOL_OK);
                return (char*)block + sizeof(memory_block_t);
            }
            // 没有可用的固定类块：不回退到通用“非类”分配。
            // 释放锁后按“该类的用户大小”进行一次普通分配，内部会按需链式扩展；
            // 分配出的块大小与该类 block_size 一致，随后计入 used_count。
            size_t class_user_size = pool->class_sizes[i];
            if (pool->thread_safe) {
                pthread_mutex_unlock(&pool->mutex);
            }
            void* ptr = memory_pool_alloc(pool, class_user_size);
            if (!ptr) {
                // memory_pool_alloc 已设置错误码
                return NULL;
            }
            if (pool->thread_safe) {
                pthread_mutex_lock(&pool->mutex);
            }
            // 再次获取 class_pool 指针（池可能因链式扩展发生变化，但本池结构仍有效）
            class_pool = &pool->size_classes[i];
            class_pool->used_count++;
#if MP_DEBUG
            // 确认得到的块大小与该类内部块大小一致
            size_t blk_sz = memory_pool_get_block_size(pool, ptr);
            MP_ASSERT(blk_sz == class_pool->block_size, "alloc_fixed: block size mismatch");
#endif
            if (pool->thread_safe) {
                pthread_mutex_unlock(&pool->mutex);
            }
            set_error(POOL_OK);
            return ptr;
        }
    }

    if (pool->thread_safe) {
        pthread_mutex_unlock(&pool->mutex);
    }

    // 未找到匹配的固定大小类别，使用普通分配（可能链式扩展）一般不会到这里。
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
#if MP_DEBUG
    MP_ASSERT(pool->num_classes >= 0 && pool->num_classes <= MAX_SIZE_CLASSES, "invalid num_classes");
#endif
    for (int i = 0; i < pool->num_classes; i++) {
        if (block->size == pool->size_classes[i].block_size) {
            size_class_pool_t* class_pool = &pool->size_classes[i];
            
            // 将块返回到固定大小池
            block->flags &= ~MB_FLAG_FREE; // returning to private free list
            block->flags |= MB_FLAG_SIZECLASS;
            block->u.next = class_pool->free_blocks;
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

    // 不属于任何 size-class：清除 SIZECLASS 标记后走普通释放
#if MP_DEBUG
    MP_LOG("free_fixed: block size %zu not matching any class -> general free", (size_t)block->size);
#endif
    block->flags &= ~MB_FLAG_SIZECLASS;
    memory_pool_free(pool, ptr);
}
