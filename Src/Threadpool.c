#include "Threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* 任务结构体 */
typedef struct threadpool_task {
    threadpool_task_func function; // 任务函数
    void *argument;                // 函数参数
    struct threadpool_task *next;  // 链表下一个节点
} threadpool_task_t;

/* 线程池结构体定义 */
struct threadpool_t {
    pthread_mutex_t lock;       // 互斥锁保护任务队列
    pthread_cond_t notify;      // 条件变量用于任务通知
    pthread_t *threads;         // 工作线程数组
    threadpool_task_t *head;    // 任务队列头
    threadpool_task_t *tail;    // 任务队列尾
    int thread_count;           // 线程数量
    int queue_size;             // 当前队列中任务数量
    int max_queue_size;         // 任务队列最大容量
    bool shutdown;              // 线程池关闭标志
};

/**
 * 工作线程函数
 */
static void *threadpool_worker(void *arg) 
{
    threadpool_t *pool = (threadpool_t *)arg;
    threadpool_task_t *task;

    while (1) {
        // 获取互斥锁
        pthread_mutex_lock(&(pool->lock));

        // 等待任务或关闭信号
        while ((pool->queue_size == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        // 如果线程池关闭且任务队列为空，则退出
        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&(pool->lock));
            pthread_exit(NULL);
        }

        // 获取队列中第一个任务
        task = pool->head;
        if (task != NULL) {
            // 更新队列
            pool->head = task->next;
            if (pool->head == NULL) {
                pool->tail = NULL;
            }
            pool->queue_size--;

            // 解锁，允许其他线程访问队列
            pthread_mutex_unlock(&(pool->lock));

            // 执行任务
            (*(task->function))(task->argument);
            
            // 任务完成后释放内存
            free(task);
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
    pool->head = NULL;
    pool->tail = NULL;
    pool->queue_size = 0;
    pool->shutdown = false;

    // 分配线程数组内存
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    if (pool->threads == NULL) {
        goto err;
    }

    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0) {
        goto err;
    }

    // 创建工作线程
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, threadpool_worker, (void *)pool) != 0) {
            threadpool_destroy(pool, THREADPOOL_IMMEDIATE);
            return NULL;
        }
    }

    return pool;

err:
    if (pool) {
        if (pool->threads) {
            free(pool->threads);
        }
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
    task = (threadpool_task_t *)malloc(sizeof(threadpool_task_t));
    if (task == NULL) {
        pthread_mutex_unlock(&(pool->lock));
        return THREADPOOL_MEMORY_ERROR;
    }

    // 初始化任务
    task->function = function;
    task->argument = argument;
    task->next = NULL;

    // 添加任务到队列尾部
    if (pool->queue_size == 0) {
        pool->head = task;
        pool->tail = task;
    } else {
        pool->tail->next = task;
        pool->tail = task;
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
    threadpool_task_t *task, *next;

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

    // 唤醒所有等待的工作线程
    if (pthread_cond_broadcast(&(pool->notify)) != 0 ||
        pthread_mutex_unlock(&(pool->lock)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 等待所有线程结束
    for (i = 0; i < pool->thread_count; i++) {
        if (pthread_join(pool->threads[i], NULL) != 0) {
            err = THREADPOOL_THREAD_FAILURE;
        }
    }

    // 清空任务队列
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    } else {
        task = pool->head;
        while (task != NULL) {
            next = task->next;
            
            // 如果是立即模式，直接释放任务
            // 如果是优雅模式，任务已在线程结束前处理完
            // 可能存在条件竞争导致内存泄漏的风险,现已关闭该功能.
            // if (flags & THREADPOOL_IMMEDIATE) {
            //     free(task);
            // }
            
            task = next;
        }
        
        pthread_mutex_unlock(&(pool->lock));
    }

    // 销毁互斥锁和条件变量
    if (pthread_mutex_destroy(&(pool->lock)) != 0 ||
        pthread_cond_destroy(&(pool->notify)) != 0) {
        err = THREADPOOL_LOCK_FAILURE;
    }

    // 释放内存
    if (pool->threads) {
        free(pool->threads);
    }
    free(pool);

    return err;
}