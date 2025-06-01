#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

// ����ṹ�壬�������ݺͶ�д��
typedef struct {
    char *data;
    pthread_rwlock_t lock;
} Cache;

Cache global_cache;  // ȫ�ֻ������

// ��ʼ�����漰���д��
void init_cache() {
    global_cache.data = NULL;
    if (pthread_rwlock_init(&global_cache.lock, NULL) != 0) {
        fprintf(stderr, "�����д����ʼ��ʧ��\n");
        exit(EXIT_FAILURE);
    }
}

// ������̺߳�������Ƶ��������
void* cache_reader(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 3; i++) {
        if (pthread_rwlock_rdlock(&global_cache.lock) == 0) {
            printf("���߳� %d: �������� = %s\n", id,
                   global_cache.data ? global_cache.data : "(��)");
            pthread_rwlock_unlock(&global_cache.lock);
        } else {
            fprintf(stderr, "���߳� %d: ��ȡ����ʧ��\n", id);
        }
        sleep(1);
    }
    return NULL;
}

// ����д�̺߳�������Ƶд������
void* cache_writer(void *arg) {
    int id = *(int*)arg;
    char buf[32];
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_wrlock(&global_cache.lock) == 0) {
            free(global_cache.data);
            snprintf(buf, sizeof(buf), "�����ݰ汾%d", i + 1);
            global_cache.data = strdup(buf);
            printf("д�߳� %d: ���»���Ϊ '%s'\n", id, global_cache.data);
            pthread_rwlock_unlock(&global_cache.lock);
        } else {
            fprintf(stderr, "д�߳� %d: ��ȡд��ʧ��\n", id);
        }
        sleep(3);
    }
    return NULL;
}

// ȫ�����ýṹ�壬����������Ͷ�д��
typedef struct {
    int log_level;
    int max_connections;
    pthread_rwlock_t lock;
} AppConfig;

// ȫ�����ö��󣬳�ʼ��Ĭ��ֵ����
AppConfig config = {1, 100, PTHREAD_RWLOCK_INITIALIZER};

// ���ö��̺߳�������Ƶ��������
void* config_reader(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 3; i++) {
        if (pthread_rwlock_rdlock(&config.lock) == 0) {
            printf("���ö��߳� %d: ��־����=%d, ���������=%d\n",
                   id, config.log_level, config.max_connections);
            pthread_rwlock_unlock(&config.lock);
        } else {
            fprintf(stderr, "���ö��߳� %d: ��ȡ����ʧ��\n", id);
        }
        sleep(1);
    }
    return NULL;
}

// ����д�̺߳�������Ƶд������
void* config_writer(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_wrlock(&config.lock) == 0) {
            config.log_level++;
            config.max_connections += 10;
            printf("����д�߳� %d: ����Ϊ ��־����=%d, ���������=%d\n",
                   id, config.log_level, config.max_connections);
            pthread_rwlock_unlock(&config.lock);
        } else {
            fprintf(stderr, "����д�߳� %d: ��ȡд��ʧ��\n", id);
        }
        sleep(5);
    }
    return NULL;
}

int main() {
    init_cache();  // ��ʼ���������

    pthread_t readers[2], writers[1];
    pthread_t config_readers[2], config_writers[1];
    int ids[3] = {1, 2, 3};  // �̱߳��

    // ����������߳�
    for (int i = 0; i < 2; i++)
        if (pthread_create(&readers[i], NULL, cache_reader, &ids[i]) != 0)
            fprintf(stderr, "����������߳�ʧ��\n");
    // ��������д�߳�
    if (pthread_create(&writers[0], NULL, cache_writer, &ids[0]) != 0)
        fprintf(stderr, "��������д�߳�ʧ��\n");

    // �������ö��߳�
    for (int i = 0; i < 2; i++)
        if (pthread_create(&config_readers[i], NULL, config_reader, &ids[i]) != 0)
            fprintf(stderr, "�������ö��߳�ʧ��\n");
    // ��������д�߳�
    if (pthread_create(&config_writers[0], NULL, config_writer, &ids[0]) != 0)
        fprintf(stderr, "��������д�߳�ʧ��\n");

    // �ȴ������߳̽���
    for (int i = 0; i < 2; i++) {
        pthread_join(readers[i], NULL);
        pthread_join(config_readers[i], NULL);
    }
    pthread_join(writers[0], NULL);
    pthread_join(config_writers[0], NULL);

    // ������Դ
    pthread_rwlock_destroy(&global_cache.lock); // ���ٻ����д��
    free(global_cache.data);                    // �ͷŻ�������
    pthread_rwlock_destroy(&config.lock);       // �������ö�д��

    return 0;
}

/*
ASCII�ṹͼ�����ֶ�д���������ƣ�
��δ���ͨ��pthread��д����pthread_rwlock_t���Ͷ��̣߳�
�ֱ�ģ���˻����ȫ�����õĸ߲���������Ƶд������ʵ����
����߳̿��԰�ȫ������ȡ����д��ʱ�������Դ���ʿ��ơ�
����չʾ�˶�д���ĵ����÷����������ɲ�����д������ռ��
�����ڶ���д�ٵĳ������Ƕ��̲߳���������������ܺͱ�֤
����һ���Եĳ��ü�����

+-------------------+      +-------------------+      +-------------------+
|  cache_reader 1   |      |  cache_reader 2   |      |   cache_writer    |
+-------------------+      +-------------------+      +-------------------+
          |                         |                         |
          |-------------------------|-------------------------|
                                    |
                                    v
                        +-------------------------+
                        |   global_cache.lock     |  <--- ��д��
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
                        |    config.lock          |  <--- ��д��
                        +-------------------------+

˵����
- ÿ�� reader �߳̿��Բ�����ȡͬһ����Դ�Ķ�����ʵ�ָ߲�������
- writer �̻߳�ȡд��ʱ���ռ��Դ������������д�̣߳�ʵ��д���⡣
- ͨ�� pthread_rwlock_t ��д������֤������һ���ԺͲ������ܡ�
*/