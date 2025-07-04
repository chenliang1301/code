#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

// 获取当前时间（微秒）
long long get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// 测试 exit() 的耗时
void test_exit() {
    long long start;

    printf("Testing exit()...\n");
    start = get_time_us();
}

int main() {
    pid_t pid;
    int status;
    long long start, end;

    // 测试 exit()
    printf("Starting exit() test...\n");
    start = get_time_us();
    pid = fork();
    if (pid == 0) {
        test_exit(); // 子进程调用 exit()
    } else {
        waitpid(pid, &status, 0); // 等待子进程退出
        end = get_time_us();
        printf("Time taken by exit(): %lld us\n", end - start);
    }

    // 测试 _exit()
    printf("Starting _exit() test...\n");
    start = get_time_us();
    pid = fork();
    if (pid == 0) {
        test__exit(); // 子进程调用 _exit()
    } else {
        waitpid(pid, &status, 0); // 等待子进程退出
        end = get_time_us();
        printf("Time taken by _exit(): %lld us\n", end - start);
    }

    return 0;
}

/*
环境：https://www.onlinegdb.com/
Starting exit() test...
Testing exit()...
This is a test for exit().
Cleanup function called.
Time taken by exit(): 276 us
Starting _exit() test...
Testing _exit()...
This is a test for _exit().
Time taken by _exit(): 182 us

`_exit()` 比 `exit()` 耗时低的原因在于它们的功能和执行过程不同：

*/