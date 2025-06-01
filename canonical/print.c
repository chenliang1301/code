#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#define LOOP_COUNT 10000

// 获取当前时间（微秒）
long long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

int main() {
    long long start, end;
    int i;

    // printf 耗时测试
    start = get_time_us();
    for (i = 0; i < LOOP_COUNT; ++i) {
        printf("printf test line %d\n", i);
    }
    fflush(stdout); // 确保全部输出
    end = get_time_us();
    printf("Total time for %d printf: %lld us\n", LOOP_COUNT, end - start);

    // syslog 耗时测试
    openlog("demo", LOG_PID | LOG_CONS, LOG_USER);
    start = get_time_us();
    for (i = 0; i < LOOP_COUNT; ++i) {
        syslog(LOG_INFO, "syslog test line %d", i);
    }
    end = get_time_us();
    printf("Total time for %d syslog: %ld us\n", LOOP_COUNT, end - start);
    closelog();

    return 0;
}
/*!
运行环境：https://www.onlinegdb.com/online_c_compiler
结果：syslog比printf快得多,100倍。
Total time for 10000 printf: 6800567 us
Total time for 10000 syslog: 60457 us

`syslog` 比 `printf` 快得多，主要原因如下：

1. **输出目标不同**  
   - `printf` 默认输出到终端（标准输出），每次调用都可能触发终端刷新，终端 I/O 通常很慢。
   - `syslog` 通常将日志写入内存缓冲区，由系统后台的 syslog 服务异步处理，写入速度更快。

2. **缓冲机制不同**  
   - `printf` 的缓冲区较小，频繁刷新到屏幕，I/O 开销大。
   - `syslog` 采用系统级缓冲和异步写入，减少了每次调用的等待时间。

3. **I/O 类型不同**  
   - 终端输出属于“字符设备”I/O，速度慢。
   - syslog 通常写入本地 socket 或内存，效率高。

4. **后台处理**  
   - `syslog` 由守护进程统一管理日志，应用进程只需将消息交给 syslog 服务即可返回，无需等待实际写盘或显示。

**总结**：  
`syslog` 速度快，是因为它主要是内存操作和异步处理，而 `printf` 直接涉及慢速的终端输出。实际生产环境中，日志量大时推荐用 syslog 这类系统日志接口。
*/