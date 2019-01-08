#include <memory.h>
#include <stdlib.h>

#include "overthrower.h"

static void* (*volatile forced_memset)(void*, int, size_t) = memset;

static void* mallocMemsetFree()
{
    void* ptr = malloc(128);
    if (ptr)
        forced_memset(ptr, 0, 128);
    free(ptr);
    return ptr;
}

void* somePureCFunction()
{
    mallocMemsetFree();
    activateOverthrower();
    pauseOverthrower(0);
    mallocMemsetFree();
    resumeOverthrower();
    void* ptr = mallocMemsetFree();
    deactivateOverthrower();
    return ptr;
}
