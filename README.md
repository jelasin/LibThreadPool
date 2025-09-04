# LibThreadPool - C语言线程池库

一个轻量级、高性能、线程安全的 C 线程池实现。当前已集成环形队列和内存池，以降低分配开销并提升吞吐。

## 特性

- 高性能调度：环形队列（Third/ring_queue）作为任务队列，入队/出队常数时间
- 低分配开销：内存池（Third/mempool）用于任务节点分配，失败时自动回退 malloc
- 线程安全：互斥锁 + 条件变量（支持“队列清空且无活跃任务”的等待）
- 灵活容量：queue_size=0 表示无限队列（自动扩容）；>0 表示有界队列
- 两种销毁模式：优雅（等待完成）/ 立即（取消队列中未执行任务）
- 简单 API：仅 3 个接口，易于集成

## 目录结构

```text
LibThreadPool/
├── Include/
│   └── Threadpool.h           # 线程池头文件
├── Src/
│   └── Threadpool.c           # 线程池实现
├── Third/                     # 第三方工具库（本仓库内）
│   ├── Include/
│   │   ├── mempool/memory_pool.h
│   │   └── ring_queue/ring_queue.h
│   └── Src/
│       ├── mempool/memory_pool.c
│       └── ring_queue/ring_queue.c
├── example/
│   └── example.c              # 示例程序
├── Makefile
├── Bin/                       # 产物（可执行/库）
└── Build/                     # 中间文件
```

## 构建与运行

前提：GCC、POSIX 线程库（pthread）。

使用 Makefile：

```bash
# 构建可执行示例（Bin/threadpool_demo）
make -j"$(nproc)" all

# 运行示例
./Bin/threadpool_demo

# 仅清理
make clean

# 生成静态库和共享库（包含 ring_queue & mempool 实现）
make -j"$(nproc)" static shared
```

将库集成到你的项目：

```bash
gcc your_app.c -I./Include -I./Third/Include -L./Bin -lthreadpool -lpthread -o your_app
```

## API

头文件：`Include/Threadpool.h`

```c
typedef void (*threadpool_task_func)(void *arg);

threadpool_t *threadpool_create(int thread_count, int queue_size);
int threadpool_add(threadpool_t *pool, threadpool_task_func function, void *argument);
int threadpool_destroy(threadpool_t *pool, int flags);

#define THREADPOOL_GRACEFUL  1  // 等待所有任务完成
#define THREADPOOL_IMMEDIATE 2  // 立即关闭，取消未执行任务
```

说明：

- thread_count <= 0 将使用默认线程数（实现目前默认 4）。
- queue_size=0 表示无限队列：队列容量按需自动扩容；>0 表示固定上限。
- threadpool_add 为非阻塞：当队列为有界且已满时返回 `THREADPOOL_QUEUE_FULL`。
- destroy(THREADPOOL_GRACEFUL)：等待队列清空且无活跃任务后回收资源。
- destroy(THREADPOOL_IMMEDIATE)：唤醒线程并尽快退出，队列中未执行的任务会被取消（仅释放任务节点结构）。

返回码：

- `THREADPOOL_SUCCESS` (0)
- `THREADPOOL_INVALID` (-1)
- `THREADPOOL_LOCK_FAILURE` (-2)
- `THREADPOOL_QUEUE_FULL` (-3)
- `THREADPOOL_SHUTDOWN` (-4)
- `THREADPOOL_THREAD_FAILURE` (-5)
- `THREADPOOL_MEMORY_ERROR` (-6)

## 示例

示例代码位于 `example/example.c`，可通过 `make run`（等价于 `make all && ./Bin/threadpool_demo`）运行。核心用法：

```c
// 创建线程池（8个工作线程，队列上限100；若传0则为无限队列）
threadpool_t *pool = threadpool_create(8, 100);

// 添加任务（参数建议由调用方管理其生命周期）
int *arg = malloc(sizeof(int));
*arg = 42;
threadpool_add(pool, task_fn, arg);

// 关闭线程池
threadpool_destroy(pool, THREADPOOL_GRACEFUL);
```

## 实现细节（性能）

- 任务队列：环形队列（Third/ring_queue），入队/出队 O(1)。无限队列通过按倍数扩容实现。
- 任务对象：使用内存池（Third/mempool）分配 `threadpool_task_t`；若内存池不足会回退到 `malloc`。
- 同步语义：使用 `notify` 与 `empty` 两个条件变量，确保优雅关闭时不会竞态退出。

## 注意事项

1. 任务函数参数（argument）的生命周期由调用方负责。当前立即关闭模式仅释放任务节点本身，不会自动释放 argument 指向的资源；如需在任务未执行时也清理参数，请在更高层统一管理或避免在 argument 中直接持有堆内存所有权。
2. 任务函数应避免长时间阻塞，或自行做超时/取消控制，以获得更好吞吐与收敛时间。
3. 在有界队列下，队满会返回 `THREADPOOL_QUEUE_FULL`；如需阻塞提交/超时提交，可扩展接口（当前实现不阻塞）。

## 调试（DEBUG 统计）

编译为调试模式（`make debug` 或自行添加 `-DDEBUG -g`），在线程池销毁时会输出内存池使用统计，便于确认是否走了内存池路径：

- alloc_fixed / alloc_pool / alloc_malloc：任务节点分配来源（固定大小类别 / 通用池 / 系统 malloc）
- free_pool_or_fixed / free_malloc：任务执行完成后的释放统计
- destroy_free_pool_or_fixed / destroy_free_malloc：销毁阶段（未执行任务）释放统计

正常情况下应主要为 alloc_fixed 与对应的 pool 释放计数，malloc 计数接近 0（除非内存池耗尽或未启用）。

## 许可证

MIT
