#include <stdio.h>
#include <stdlib.h>

typedef struct Allocation {
    void* ptr;
    size_t size;
    const char* file;
    const char* func;
    int line;
    struct Allocation* next;
} Allocation;

typedef struct MemoryContext {
    size_t allocated_memory;
    Allocation* allocations;
} MemoryContext;

void* my_malloc(size_t size, const char* file, const char* func, int line, MemoryContext* context) {
    void* ptr = malloc(size);
    if (ptr) {
        context->allocated_memory += size;

        // 记录分配信息
        Allocation* new_alloc = (Allocation*)malloc(sizeof(Allocation));
        new_alloc->ptr = ptr;
        new_alloc->size = size;
        new_alloc->file = file;
        new_alloc->func = func;
        new_alloc->line = line;
        new_alloc->next = context->allocations;
        context->allocations = new_alloc;
    }
    return ptr;
}

void my_free(void* ptr, const char* file, const char* func, int line, MemoryContext* context) {
    if (ptr) {
        // 查找并移除分配记录
        Allocation** current = &context->allocations;
        while (*current) {
            if ((*current)->ptr == ptr) {
                Allocation* to_free = *current;
                *current = (*current)->next;

                context->allocated_memory -= to_free->size;
                free(to_free);
                break;
            }
            current = &(*current)->next;
        }

        // 释放内存
        free(ptr);
    }
}

void report_leaks(MemoryContext* context) {
    Allocation* current = context->allocations;
    while (current) {
        printf("[LEAK] %zu bytes allocated in file %s, function %s, line %d\n",
               current->size, current->file, current->func, current->line);
        current = current->next;
    }
}

#define malloc(size, context) my_malloc(size, __FILE__, __FUNCTION__, __LINE__, context)
#define free(ptr, context) my_free(ptr, __FILE__, __FUNCTION__, __LINE__, context)

int main() {
    MemoryContext context = {0, NULL}; // 初始化内存上下文

    // 模拟内存分配和释放
    char* buffer1 = (char*)malloc(100, &context);
    char* buffer2 = (char*)malloc(200, &context);
    char* buffer3 = (char*)malloc(300, &context);

    free(buffer1, &context); // 正常释放
    // buffer2 和 buffer3 未释放，模拟内存泄漏

    // 检查是否有内存泄漏
    if (context.allocated_memory > 0) {
        report_leaks(&context);
    } else {
        printf("No memory leaks detected.\n");
    }

    return 0;
}

/*!<
如果不使用全局变量来记录内存泄漏，可以通过将内存分配记录存储在一个上下文结构中
，并将该上下文作为参数传递给相关的分配和释放函数。以下是修改后的代码示
*/