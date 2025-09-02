#include "../Include/Threadpool.h"
#include "../Third/Include/ring_queue/ring_queue.h"
#include "../Third/Include/mempool/memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 任务结构体 */
typedef struct threadpool_task {
    threadpool_task_func function; // 任务函数
    void *argument;                // 函数参数
} threadpool_task_t;

/* 线程池结构体定义 */
struct threadpool_t {
    pthread_mutex_t lock;       // 互斥锁保护任务队列
    pthread_cond_t notify;      // 条件变量用于任务通知
    pthread_cond_t empty;       // 条件变量：队列清空并无活跃任务
    pthread_t *threads;         // 工作线程数组
    ring_queue_t *queue;        // 任务队列（环形队列）
    int thread_count;           // 线程数量
    int started;                // 已成功启动的线程数
    int queue_size;             // 当前队列中任务数量
    int max_queue_size;         // 任务队列最大容量（0 表示无限制，将自动扩容）
    bool shutdown;              // 线程池关闭标志
    bool shutdown_immediate;    // 立即关闭标志
    int active;                 // 正在执行的任务数量

    // 任务节点内存池
    memory_pool_t *task_pool;
};

/**
 * 工作线程函数
 */
static void *threadpool_worker(void *arg) 
{
    threadpool_t *pool = (threadpool_t *)arg;
    threadpool_task_t *task = NULL;

    while (1) {
        // 获取互斥锁
        pthread_mutex_lock(&(pool->lock));

        // 等待任务或关闭信号
        while ((pool->queue_size == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        // 如果立即关闭，或优雅关闭并且队列为空，则退出
        if (pool->shutdown && (pool->shutdown_immediate || pool->queue_size == 0)) {
            pthread_mutex_unlock(&(pool->lock));
            pthread_exit(NULL);
        }

        // 取出一个任务（peek+dequeue）
        if (pool->queue_size > 0) {
            void *elem = NULL;
            if (ring_queue_peek(pool->queue, &elem) == RING_QUEUE_SUCCESS && elem != NULL) {
                task = (threadpool_task_t *)elem;
                // 先从队列移除，避免被其他线程重复获取
                if (ring_queue_dequeue(pool->queue) == RING_QUEUE_SUCCESS) {
                    pool->queue_size--;
                    pool->active++; // 标记活跃任务
                } else {
                    // 理论上不应发生，保守处理
                    task = NULL;
                }
            }

            // 解锁，允许其他线程访问队列
            pthread_mutex_unlock(&(pool->lock));

            // 执行任务
            if (task) {
                (*(task->function))(task->argument);
                // 任务完成后释放内存
                if (pool->task_pool) {
                    memory_pool_free(pool->task_pool, task);
                } else {
                    free(task);
                }
            }

            // 任务完成，更新活跃计数
            pthread_mutex_lock(&(pool->lock));
            pool->active--;
            // 如果优雅关闭并且队列空且无活跃任务，唤醒等待者
            if (pool->shutdown && !pool->shutdown_immediate && pool->queue_size == 0 && pool->active == 0) {
                pthread_cond_broadcast(&(pool->empty));
            }
            pthread_mutex_unlock(&(pool->lock));
        } else {
            pthread_mutex_unlock(&(pool->lock));
        }
    }

    return NULL;
}

/**
 * 创建线程池
 */
threadpool_t *threadpool_create(int thread_count, int queue_size) 
{
    int i;
    threadpool_t *pool;

    // 参数检查
    if (thread_count <= 0) {
        thread_count = 4; // 默认4个线程
    }

    // 分配线程池结构体内存
    if ((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
        goto err;
    }

    // 初始化线程池结构体
    memset(pool, 0, sizeof(threadpool_t));
    pool->thread_count = thread_count;
    pool->max_queue_size = queue_size;
    pool->queue = NULL;
    pool->queue_size = 0;
    pool->shutdown = false;
    pool->shutdown_immediate = false;
    pool->active = 0;
    pool->started = 0;
    pool->task_pool = NULL;

    // 分配线程数组内存
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    if (pool->threads == NULL) {
        goto err;
    }

    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0 ||
        pthread_cond_init(&(pool->empty), NULL) != 0) {
        goto err;
    }

    // 创建任务队列（容量：若queue_size==0则给一个初始容量）
    size_t initial_capacity = (queue_size > 0) ? (size_t)queue_size : 1024;
    pool->queue = ring_queue_create(initial_capacity, NULL);
    if (!pool->queue) {
        goto err;
    }

    // 创建任务内存池（尽量缓存任务对象）
    size_t task_node_size = sizeof(threadpool_task_t) + 64; // 预留对齐和头信息
    size_t task_pool_bytes = initial_capacity * task_node_size;
    pool->task_pool = memory_pool_create(task_pool_bytes, true);
    // 如果创建失败，回退到malloc/free

    // 创建工作线程
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, threadpool_worker, (void *)pool) != 0) {
            // 记录已创建线程数，随后销毁时只join这些
            break;
        }
        pool->started++;
    }

    if (pool->started == 0) {
        // 一个线程也没启动成功
        goto err;
    }
    return pool;

err:
    if (pool) {
        if (pool->threads) {
            // 如有部分线程已创建，触发立即关闭并等待
            if (pool->started > 0) {
                pthread_mutex_lock(&(pool->lock));
                pool->shutdown = true;
                pool->shutdown_immediate = true;
                pthread_cond_broadcast(&(pool->notify));
                pthread_mutex_unlock(&(pool->lock));
                for (i = 0; i < pool->started; i++) {
                    pthread_join(pool->threads[i], NULL);
                }
            }
            free(pool->threads);
        }
        if (pool->queue) {
            ring_queue_destroy(pool->queue);
        }
        if (pool->task_pool) {
            memory_pool_destroy(pool->task_pool);
        }
        // 销毁同步原语
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
        pthread_cond_destroy(&(pool->empty));
        free(pool);
    }
    return NULL;
}

/**
 * 向线程池添加任务
 */
int threadpool_add(threadpool_t *pool, threadpool_task_func function, void *argument) 
{
    threadpool_task_t *task;

    // 参数检查
    if (pool == NULL || function == NULL) {
        return THREADPOOL_INVALID;
    }

    // 获取锁
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREADPOOL_LOCK_FAILURE;
    }

    // 检查队列是否已满
    if (pool->max_queue_size > 0 && pool->queue_size >= pool->max_queue_size) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_QUEUE_FULL;
    }

    // 检查是否已关闭
    if (pool->shutdown) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_SHUTDOWN;
    }

    // 创建任务结构体
    if (pool->task_pool) {
        task = (threadpool_task_t *)memory_pool_alloc(pool->task_pool, sizeof(threadpool_task_t));
        if (!task) {
            // 内存池不足时回退到malloc
            task = (threadpool_task_t *)malloc(sizeof(threadpool_task_t));
        }
    } else {
        task = (threadpool_task_t *)malloc(sizeof(threadpool_task_t));
    }
    if (task == NULL) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_MEMORY_ERROR;
    }

    // 初始化任务
    task->function = function;
    task->argument = argument;
    // 入队，如果容量不足且无限制模式，尝试扩容
    if (ring_queue_is_full(pool->queue)) {
        if (pool->max_queue_size == 0) {
            size_t new_cap = ring_queue_capacity(pool->queue) * 2;
            if (new_cap == 0) new_cap = 1024;
            ring_queue_resize(pool->queue, new_cap);
        }
    }
    if (ring_queue_is_full(pool->queue)) {
        // 仍然满且有限制，报错并回收task
        if (pool->task_pool) memory_pool_free(pool->task_pool, task); else free(task);
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_QUEUE_FULL;
    }
    if (ring_queue_enqueue(pool->queue, task) != RING_QUEUE_SUCCESS) {
        // 极端情况下的失败
        if (pool->task_pool) memory_pool_free(pool->task_pool, task); else free(task);
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_QUEUE_FULL;
    }
    pool->queue_size++;

    // 通知一个等待的工作线程
    if (pthread_cond_signal(&(pool->notify)) != 0) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_LOCK_FAILURE;
    }

    // 释放锁
    pthread_mutex_unlock(&(pool->lock));
    return THREADPOOL_SUCCESS;
}

/**
 * 销毁线程池
 */
int threadpool_destroy(threadpool_t *pool, int flags) 
{
    int i, err = 0;
    (void)flags;

    if (pool == NULL) {
        return THREADPOOL_INVALID;
    }

    // 获取锁
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREADPOOL_LOCK_FAILURE;
    }

    // 检查是否已经关闭
    if (pool->shutdown) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_SHUTDOWN;
    }

    // 设置关闭标志
    pool->shutdown = true;
    pool->shutdown_immediate = (flags & THREADPOOL_IMMEDIATE) ? true : false;

    // 唤醒所有等待的工作线程
    if (pthread_cond_broadcast(&(pool->notify)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 优雅关闭：等待队列清空且无活跃任务
    if (!pool->shutdown_immediate) {
        while ((pool->queue_size > 0 || pool->active > 0) && err == 0) {
            if (pthread_cond_wait(&(pool->empty), &(pool->lock)) != 0) {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }
        }
    }

    // 释放锁，让出给join期间可能需要的同步（理论上不需要，但保持一致）
    if (pthread_mutex_unlock(&(pool->lock)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 等待所有线程结束
    for (i = 0; i < pool->started; i++) {
        if (pthread_join(pool->threads[i], NULL) != 0) {
            err = THREADPOOL_THREAD_FAILURE;
        }
    }

    // 清空任务队列（立即模式下释放未执行的任务）
    if (pthread_mutex_lock(&(pool->lock)) == 0) {
        if (pool->queue) {
            void *elem = NULL;
            while (!ring_queue_is_empty(pool->queue)) {
                if (ring_queue_peek(pool->queue, &elem) == RING_QUEUE_SUCCESS && elem) {
                    // 出队
                    ring_queue_dequeue(pool->queue);
                    // 释放任务
                    if (pool->task_pool) memory_pool_free(pool->task_pool, elem); else free(elem);
                } else {
                    break;
                }
            }
            pool->queue_size = 0;
        }
        pthread_mutex_unlock(&(pool->lock));
    } else {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 销毁互斥锁和条件变量
    if (pthread_mutex_destroy(&(pool->lock)) != 0 ||
        pthread_cond_destroy(&(pool->notify)) != 0 ||
        pthread_cond_destroy(&(pool->empty)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 释放内存
    if (pool->threads) {
        free(pool->threads);
    }
    if (pool->queue) {
        ring_queue_destroy(pool->queue);
    }
    if (pool->task_pool) {
        memory_pool_destroy(pool->task_pool);
    }
    free(pool);

    return err;
}