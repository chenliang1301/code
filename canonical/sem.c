#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define PARKING_SPACES 3  // �ܳ�λ����
#define CARS 5            // �ܳ�����

sem_t parking_sem;        // �ź�����ʾ���ó�λ

// ������Ϊģ��
void park_car(int car_id) {
    printf("Vehicle %d enters the parking lot | ", car_id);
    sleep(1);  // ģ��ͣ��ʱ����1�룩
    printf("Vehicle %d leaves the parking lot\n", car_id);
}

// �̺߳�������������ͣ��
void* car_thread(void* arg) {
    int car_id = *(int*)arg;
    free(arg); // �ͷŷ�����ڴ�

    sem_wait(&parking_sem);    // �ȴ���ȡ��λ��P������
    park_car(car_id);
    sem_post(&parking_sem);    // �ͷų�λ��V������

    return NULL;
}

int main() {
    pthread_t cars[CARS];

    // ��ʼ���ź�������ʼ��3�����ó�λ
    if (sem_init(&parking_sem, 0, PARKING_SPACES) != 0) {
        perror("sem_init failed");
        return 1;
    }

    // ����5���߳�ģ�⳵��
    for (int i = 0; i < CARS; i++) {
        int* car_id = malloc(sizeof(int));
        if (!car_id) {
            perror("malloc failed");
            return 1;
        }
        *car_id = i + 1;  // ������Ŵ�1��ʼ
        if (pthread_create(&cars[i], NULL, car_thread, car_id) != 0) {
            perror("pthread_create failed");
            free(car_id);
            return 1;
        }
    }

    // �ȴ����г����߳̽���
    for (int i = 0; i < CARS; i++) {
        pthread_join(cars[i], NULL);
    }

    sem_destroy(&parking_sem);  // �����ź���
    return 0;
}

/*
��δ���ͨ���ź����Ͷ��߳�ģ����һ����3����λ��5������ͣ������
��֤ͬһʱ�����ֻ��3�����ܽ���ͣ���������೵����ȴ���������
�ź����ڲ�����Դ���ʿ����еĵ���Ӧ�á�������ASCIIͼ��ֱ��չ
ʾ���߳̾��������롢�뿪�Ͳ�λ�Ĺ��̡�
*/
