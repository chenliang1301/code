#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define PARKING_SPACES 3  // 总车位数量
#define CARS 5            // 总车辆数

sem_t parking_sem;        // 信号量表示可用车位

// 车辆行为模拟
void park_car(int car_id) {
    printf("Vehicle %d enters the parking lot | ", car_id);
    sleep(1);  // 模拟停车时长（1秒）
    printf("Vehicle %d leaves the parking lot\n", car_id);
}

// 线程函数：车辆尝试停车
void* car_thread(void* arg) {
    int car_id = *(int*)arg;
    free(arg); // 释放分配的内存

    sem_wait(&parking_sem);    // 等待获取车位（P操作）
    park_car(car_id);
    sem_post(&parking_sem);    // 释放车位（V操作）

    return NULL;
}

int main() {
    pthread_t cars[CARS];

    // 初始化信号量：初始有3个可用车位
    if (sem_init(&parking_sem, 0, PARKING_SPACES) != 0) {
        perror("sem_init failed");
        return 1;
    }

    // 创建5个线程模拟车辆
    for (int i = 0; i < CARS; i++) {
        int* car_id = malloc(sizeof(int));
        if (!car_id) {
            perror("malloc failed");
            return 1;
        }
        *car_id = i + 1;  // 车辆编号从1开始
        if (pthread_create(&cars[i], NULL, car_thread, car_id) != 0) {
            perror("pthread_create failed");
            free(car_id);
            return 1;
        }
    }

    // 等待所有车辆线程结束
    for (int i = 0; i < CARS; i++) {
        pthread_join(cars[i], NULL);
    }

    sem_destroy(&parking_sem);  // 销毁信号量
    return 0;
}

/*
这段代码通过信号量和多线程模拟了一个有3个车位、5辆车的停车场，
保证同一时刻最多只有3辆车能进入停车场，其余车辆需等待，体现了
信号量在并发资源访问控制中的典型应用。附带的ASCII图形直观展
示了线程竞争、进入、离开和补位的过程。
*/
