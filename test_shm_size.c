#include <stdio.h>
#include "config.h"

int main() {
    printf("Dimensione SharedMemory: %zu bytes (%.2f MB)\n", sizeof(SharedMemory), sizeof(SharedMemory) / (1024.0 * 1024.0));
    return 0;
}
