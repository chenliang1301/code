#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// Structure to store allocation information
typedef struct Allocation {
    void *ptr;
    size_t size;
    const char *file;
    const char *func;
    int line;
    struct Allocation *next;
} Allocation;

static Allocation *allocations = NULL; // Linked list to track allocations
static void *(*real_malloc)(size_t) = NULL;
static void (*real_free)(void *) = NULL;

// Add allocation to the tracking list
void add_allocation(void *ptr, size_t size, const char *file, const char *func, int line) {
    Allocation *new_alloc = (Allocation *)real_malloc(sizeof(Allocation));
    new_alloc->ptr = ptr;
    new_alloc->size = size;
    new_alloc->file = file;
    new_alloc->func = func;
    new_alloc->line = line;
    new_alloc->next = allocations;
    allocations = new_alloc;
}

// Remove allocation from the tracking list
void remove_allocation(void *ptr) {
    Allocation **current = &allocations;
    while (*current) {
        if ((*current)->ptr == ptr) {
            Allocation *to_free = *current;
            *current = (*current)->next;
            real_free(to_free);
            return;
        }
        current = &(*current)->next;
    }
}

// Custom malloc function
void *malloc(size_t size) {
    if (!real_malloc) {
        real_malloc = dlsym(RTLD_NEXT, "malloc");
    }
    void *ptr = real_malloc(size);
    add_allocation(ptr, size, "unknown", "unknown", 0); // Default values
    return ptr;
}

// Custom free function
void free(void *ptr) {
    if (!real_free) {
        real_free = dlsym(RTLD_NEXT, "free");
    }
    remove_allocation(ptr);
    real_free(ptr);
}

// Macro to track file, function, and line number
#define malloc(size) tracked_malloc(size, __FILE__, __func__, __LINE__)
void *tracked_malloc(size_t size, const char *file, const char *func, int line) {
    void *ptr = malloc(size);
    add_allocation(ptr, size, file, func, line);
    return ptr;
}

// Report memory leaks
void report_leaks() {
    Allocation *current = allocations;
    while (current) {
        fprintf(stderr, "Memory leak detected: %p (%zu bytes) allocated at %s:%s:%d\n",
                current->ptr, current->size, current->file, current->func, current->line);
        current = current->next;
    }
}

// Destructor to report leaks at program exit
__attribute__((destructor)) void cleanup() {
    report_leaks();
}