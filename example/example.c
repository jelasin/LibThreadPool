#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "../Include/Threadpool.h"
#include <string.h>

// 互斥锁，用于打印输出
pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

bool map[30]; // 任务完成标志数组
__thread size_t thread_id; // 线程ID

// 任务函数
void task_function(void *arg) 
{
    int task_id = *(int*)arg;
    thread_id = pthread_self();

    // 模拟任务执行时间 (0.1-1秒)
    int sleep_time = (rand() % 900 + 100) * 1000;
    usleep(sleep_time);
    
    // 线程安全地打印结果
    pthread_mutex_lock(&print_lock);
    map[task_id] = true; // 标记任务完成
    printf("任务 %d 由线程 %lu 完成，耗时 %.2f 秒\n", task_id, thread_id, sleep_time/1000000.0);
    pthread_mutex_unlock(&print_lock);
    
    free(arg); // 释放参数内存
}

int main() 
{
    // 初始化随机数生成器
    srand(time(NULL));
    
    printf("线程池演示程序启动\n");
    
    // 创建线程池 (8个工作线程，最大队列长度100)
    threadpool_t *pool = threadpool_create(8, 100);
    if (pool == NULL) {
        fprintf(stderr, "线程池创建失败\n");
        return 1;
    }
    
    printf("线程池创建成功\n");
    memset(map, 0, sizeof(map)); // 初始化任务完成标志数组
    // 添加30个任务到线程池
    int i;
    for (i = 0; i < 30; i++) {
        int *task_id = (int*)malloc(sizeof(int));
        if (task_id == NULL) {
            fprintf(stderr, "内存分配失败\n");
            continue;
        }
        
        *task_id = i;
        
        printf("添加任务 %d 到线程池\n", i);
        int ret = threadpool_add(pool, task_function, task_id);
        
        if (ret != 0) {
            fprintf(stderr, "任务 %d 添加失败，错误码: %d\n", i, ret);
            free(task_id);
        }
        
        // 短暂延迟，方便观察
        usleep(50000); // 50毫秒
    }
    
    printf("等待任务完成...\n");

    // 销毁线程池 (优雅模式，等待所有任务完成)
    printf("销毁线程池...\n");
    threadpool_destroy(pool, THREADPOOL_GRACEFUL);
    for (int i = 0; i < 30; i++) {
        printf("%s", map[i] ? "[x]" : "[ ]");
        if (i % 10 == 9) {
            printf("\n");
        }
    }
    printf("程序结束\n");
    return 0;
}