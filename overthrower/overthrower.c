#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/syscall.h>

typedef void* (*Malloc)(size_t size);
typedef void (*Free)(void*);

static Malloc native_malloc = NULL;
static Free native_free = NULL;

static int activated = 0;
static unsigned int seed = 0;

static void initialize()
{
    if (native_malloc == NULL)
        native_malloc = (Malloc)dlsym(RTLD_NEXT, "malloc");
    if (native_free == NULL)
        native_free = (Free)dlsym(RTLD_NEXT, "free");
    
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Allocate 0 bytes using malloc and overthrower will start his job.\n");
}

void* malloc(size_t size)
{
    if (native_malloc == NULL)
        initialize();

    if (activated == 0 && size == 0) {
        FILE* file = fopen("/dev/urandom", "rb");
        if (file != NULL) {
            if (fread(&seed, 1, sizeof(seed), file) != sizeof(seed))
                seed = 0;
            fclose(file);
        }

        srand(seed);

        fprintf(stderr, "overthrower got activation signal.\n");
        fprintf(stderr, "overthrower will use following parameters for failing allocations:\n");
        fprintf(stderr, "Duty cycle = %d\n", DUTY_CYCLE);
        fprintf(stderr, "Seed = %u\n", seed);
        activated = 1;
    }

    if ((activated != 0) && (size != 0) && (rand() % DUTY_CYCLE == 0))
        return NULL;

    return native_malloc(size);
}

void free(void* ptr)
{
    if (native_free == NULL)
        initialize();
    
    native_free(ptr);
}
