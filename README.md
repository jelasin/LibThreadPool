# LibThreadPool - C语言线程池库

一个轻量级、高效、线程安全的C语言线程池实现，适用于需要处理大量并发任务的应用程序。

## 功能特点

- **高性能**：高效的任务调度和线程管理
- **线程安全**：使用互斥锁和条件变量确保线程安全
- **灵活配置**：可自定义线程数量和任务队列大小
- **两种销毁模式**：支持优雅退出和即时退出
- **简单API**：易于集成到现有项目中
- **纯C实现**：无外部依赖，跨平台兼容

## 目录结构

```text
LibThreadPool/
├── Include/
│   └── Threadpool.h   # 线程池头文件
├── Src/
│   ├── Threadpool.c   # 线程池实现
│   └── main.c         # 示例程序
└── README.md          # 本文档
```

## 编译和安装

### 前提条件

- GCC编译器
- POSIX线程库(pthread)

### 编译

```bash
# 进入项目目录
cd LibThreadPool

# 编译库和示例程序
gcc -o threadpool_demo Src/Threadpool.c Src/main.c -I./Include -lpthread

# 运行示例程序
./threadpool_demo
```

## API文档

### 创建线程池

```c
threadpool_t *threadpool_create(int thread_count, int queue_size);
```

**参数**:

- `thread_count`: 工作线程数量 (如果小于等于0，则使用默认值4)
- `queue_size`: 任务队列最大容量 (0表示无限)

**返回值**:

- 成功: 返回线程池指针
- 失败: 返回NULL

### 添加任务

```c
int threadpool_add(threadpool_t *pool, threadpool_task_func function, void *argument);
```

**参数**:

- `pool`: 线程池指针
- `function`: 任务函数指针
- `argument`: 任务函数参数

**返回值**:

- `THREADPOOL_SUCCESS` (0): 成功
- 其他错误码: 添加失败

### 销毁线程池

```c
int threadpool_destroy(threadpool_t *pool, int flags);
```

**参数**:

- `pool`: 线程池指针
- `flags`: 销毁模式
  - `THREADPOOL_GRACEFUL`: 等待所有任务完成后销毁
  - `THREADPOOL_IMMEDIATE`: 立即销毁，取消队列中的任务,可能存在条件竞争导致内存泄漏的风险,现已关闭该功能.

**返回值**:

- `THREADPOOL_SUCCESS` (0): 成功
- 其他错误码: 销毁失败

## 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include "Threadpool.h"

// 任务函数
void example_task(void *arg) {
    int task_id = *(int*)arg;
    printf("任务 %d 正在执行\n", task_id);
    free(arg); // 释放参数内存
}

int main() {
    // 创建8线程的线程池，队列容量为100
    threadpool_t *pool = threadpool_create(8, 100);
    if (pool == NULL) {
        fprintf(stderr, "线程池创建失败\n");
        return 1;
    }
    
    // 添加10个任务到线程池
    int i;
    for (i = 0; i < 10; i++) {
        int *task_id = (int*)malloc(sizeof(int));
        if (task_id == NULL) continue;
        
        *task_id = i;
        threadpool_add(pool, example_task, task_id);
    }
    
    // 优雅地销毁线程池 (等待所有任务完成)
    threadpool_destroy(pool, THREADPOOL_GRACEFUL);
    
    return 0;
}
```

## 错误处理

线程池库定义了以下错误码:

- `THREADPOOL_SUCCESS` (0): 成功
- `THREADPOOL_INVALID` (-1): 无效参数
- `THREADPOOL_LOCK_FAILURE` (-2): 锁操作失败
- `THREADPOOL_QUEUE_FULL` (-3): 任务队列已满
- `THREADPOOL_SHUTDOWN` (-4): 线程池已关闭
- `THREADPOOL_THREAD_FAILURE` (-5): 线程创建失败
- `THREADPOOL_MEMORY_ERROR` (-6): 内存分配失败

## 注意事项

1. 任务参数需要由调用者动态分配内存，并在任务函数中释放
2. 优雅退出模式会等待所有已提交任务完成
3. 任务函数应避免长时间阻塞，以免影响线程池性能

## 许可证

本项目采用 MIT 许可证
