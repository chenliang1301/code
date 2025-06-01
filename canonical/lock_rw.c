#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

// 缓存结构体，包含数据和读写锁
typedef struct {
    char *data;
    pthread_rwlock_t lock;
} Cache;

Cache global_cache;  // 全局缓存对象

// 初始化缓存及其读写锁
void init_cache() {
    global_cache.data = NULL;
    if (pthread_rwlock_init(&global_cache.lock, NULL) != 0) {
        fprintf(stderr, "缓存读写锁初始化失败\n");
        exit(EXIT_FAILURE);
    }
}

// 缓存读线程函数（高频读操作）
void* cache_reader(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 3; i++) {
        if (pthread_rwlock_rdlock(&global_cache.lock) == 0) {
            printf("读线程 %d: 缓存内容 = %s\n", id,
                   global_cache.data ? global_cache.data : "(空)");
            pthread_rwlock_unlock(&global_cache.lock);
        } else {
            fprintf(stderr, "读线程 %d: 获取读锁失败\n", id);
        }
        sleep(1);
    }
    return NULL;
}

// 缓存写线程函数（低频写操作）
void* cache_writer(void *arg) {
    int id = *(int*)arg;
    char buf[32];
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_wrlock(&global_cache.lock) == 0) {
            free(global_cache.data);
            snprintf(buf, sizeof(buf), "新数据版本%d", i + 1);
            global_cache.data = strdup(buf);
            printf("写线程 %d: 更新缓存为 '%s'\n", id, global_cache.data);
            pthread_rwlock_unlock(&global_cache.lock);
        } else {
            fprintf(stderr, "写线程 %d: 获取写锁失败\n", id);
        }
        sleep(3);
    }
    return NULL;
}

// 全局配置结构体，包含配置项和读写锁
typedef struct {
    int log_level;
    int max_connections;
    pthread_rwlock_t lock;
} AppConfig;

// 全局配置对象，初始化默认值和锁
AppConfig config = {1, 100, PTHREAD_RWLOCK_INITIALIZER};

// 配置读线程函数（高频读操作）
void* config_reader(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 3; i++) {
        if (pthread_rwlock_rdlock(&config.lock) == 0) {
            printf("配置读线程 %d: 日志级别=%d, 最大连接数=%d\n",
                   id, config.log_level, config.max_connections);
            pthread_rwlock_unlock(&config.lock);
        } else {
            fprintf(stderr, "配置读线程 %d: 获取读锁失败\n", id);
        }
        sleep(1);
    }
    return NULL;
}

// 配置写线程函数（低频写操作）
void* config_writer(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_wrlock(&config.lock) == 0) {
            config.log_level++;
            config.max_connections += 10;
            printf("配置写线程 %d: 更新为 日志级别=%d, 最大连接数=%d\n",
                   id, config.log_level, config.max_connections);
            pthread_rwlock_unlock(&config.lock);
        } else {
            fprintf(stderr, "配置写线程 %d: 获取写锁失败\n", id);
        }
        sleep(5);
    }
    return NULL;
}

int main() {
    init_cache();  // 初始化缓存和锁

    pthread_t readers[2], writers[1];
    pthread_t config_readers[2], config_writers[1];
    int ids[3] = {1, 2, 3};  // 线程编号

    // 启动缓存读线程
    for (int i = 0; i < 2; i++)
        if (pthread_create(&readers[i], NULL, cache_reader, &ids[i]) != 0)
            fprintf(stderr, "创建缓存读线程失败\n");
    // 启动缓存写线程
    if (pthread_create(&writers[0], NULL, cache_writer, &ids[0]) != 0)
        fprintf(stderr, "创建缓存写线程失败\n");

    // 启动配置读线程
    for (int i = 0; i < 2; i++)
        if (pthread_create(&config_readers[i], NULL, config_reader, &ids[i]) != 0)
            fprintf(stderr, "创建配置读线程失败\n");
    // 启动配置写线程
    if (pthread_create(&config_writers[0], NULL, config_writer, &ids[0]) != 0)
        fprintf(stderr, "创建配置写线程失败\n");

    // 等待所有线程结束
    for (int i = 0; i < 2; i++) {
        pthread_join(readers[i], NULL);
        pthread_join(config_readers[i], NULL);
    }
    pthread_join(writers[0], NULL);
    pthread_join(config_writers[0], NULL);

    // 清理资源
    pthread_rwlock_destroy(&global_cache.lock); // 销毁缓存读写锁
    free(global_cache.data);                    // 释放缓存数据
    pthread_rwlock_destroy(&config.lock);       // 销毁配置读写锁

    return 0;
}

/*
ASCII结构图，体现读写锁并发控制：
这段代码通过pthread读写锁（pthread_rwlock_t）和多线程，
分别模拟了缓存和全局配置的高并发读、低频写场景，实现了
多个线程可以安全并发读取、但写入时互斥的资源访问控制。
代码展示了读写锁的典型用法：读操作可并发，写操作独占，
适用于读多写少的场景，是多线程并发编程中提升性能和保证
数据一致性的常用技术。

+-------------------+      +-------------------+      +-------------------+
|  cache_reader 1   |      |  cache_reader 2   |      |   cache_writer    |
+-------------------+      +-------------------+      +-------------------+
          |                         |                         |
          |-------------------------|-------------------------|
                                    |
                                    v
                        +-------------------------+
                        |   global_cache.lock     |  <--- 读写锁
                        +-------------------------+
                                    ^
                                    |
+-------------------+      +-------------------+      +-------------------+
| config_reader 1   |      | config_reader 2   |      |  config_writer    |
+-------------------+      +-------------------+      +-------------------+
          |                         |                         |
          |-------------------------|-------------------------|
                                    |
                                    v
                        +-------------------------+
                        |    config.lock          |  <--- 读写锁
                        +-------------------------+

说明：
- 每组 reader 线程可以并发获取同一个资源的读锁，实现高并发读。
- writer 线程获取写锁时会独占资源，阻塞其他读写线程，实现写互斥。
- 通过 pthread_rwlock_t 读写锁，保证了数据一致性和并发性能。
*/