#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <pthread.h>
#include <stdbool.h>

/* 线程池错误码 */
typedef enum {
    THREADPOOL_SUCCESS = 0,      // 成功
    THREADPOOL_INVALID = -1,     // 无效参数
    THREADPOOL_LOCK_FAILURE = -2,// 锁操作失败
    THREADPOOL_QUEUE_FULL = -3,  // 任务队列已满
    THREADPOOL_SHUTDOWN = -4,    // 线程池已关闭
    THREADPOOL_THREAD_FAILURE = -5,// 线程创建失败
    THREADPOOL_MEMORY_ERROR = -6 // 内存分配失败
} threadpool_error_t;

/* 任务函数类型 */
typedef void (*threadpool_task_func)(void *arg);

/* 线程池结构体 */
typedef struct threadpool_t threadpool_t;

/**
 * 创建线程池
 * 
 * @param thread_count 线程数量
 * @param queue_size 任务队列大小上限 (0表示无限)
 * @return 成功返回线程池指针，失败返回NULL
 */
threadpool_t *threadpool_create(int thread_count, int queue_size);

/**
 * 向线程池添加任务
 * 
 * @param pool 线程池指针
 * @param function 任务函数
 * @param argument 任务参数
 * @return 成功返回0，失败返回错误码
 */
int threadpool_add(threadpool_t *pool, threadpool_task_func function, void *argument);

/**
 * 销毁线程池
 * 
 * @param pool 线程池指针
 * @param flags 关闭模式
 * @return 成功返回0，失败返回错误码
 */
int threadpool_destroy(threadpool_t *pool, int flags);

/* 线程池销毁模式 */
#define THREADPOOL_GRACEFUL 1  // 等待所有任务完成后销毁
#define THREADPOOL_IMMEDIATE 2 // 立即销毁，取消队列中的任务

#endif // __THREADPOOL_H__