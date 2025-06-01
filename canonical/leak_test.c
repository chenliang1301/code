#include <stdlib.h>

int main() {
    char *leak1 = malloc(100); // 未释放
    char *leak2 = malloc(200); // 未释放
    free(leak1);               // 正常释放
    return 0;
}