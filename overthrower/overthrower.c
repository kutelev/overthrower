#define _GNU_SOURCE

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>

typedef void* (*Malloc)(size_t size);

static Malloc native_malloc = NULL;

#define STRATEGY_RANDOM 0
#define STRATEGY_STEP 1
#define STRATEGY_PULSE 2

#define MIN_DUTY_CYCLE 1
#define MAX_DUTY_CYCLE 32768

#define MIN_DELAY 0
#define MAX_DELAY 1000

#define MIN_DURATION 1
#define MAX_DURATION 100

static const char* strategy_names[3] = { "random", "step", "pulse" };

static unsigned int activated = 0;
static unsigned int strategy = STRATEGY_RANDOM;
static unsigned int seed = 0;
static unsigned int duty_cycle = 1024;
static unsigned delay = MIN_DELAY;
static unsigned duration = MIN_DURATION;
static unsigned int malloc_number = 0;

__attribute__ ((constructor))
static void banner()
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

static void initialize()
{
    if (native_malloc == NULL)
        native_malloc = (Malloc)dlsym(RTLD_NEXT, "malloc");
}

static int strToLongInt(const char* str, long int* value)
{
    char* end_ptr;
    *value = strtol(str, &end_ptr, 10);

    if (end_ptr[0] != '\0')
        return -1;

    if ((*value == LONG_MIN || *value == LONG_MAX) && errno == ERANGE)
        return -1;

    return 0;
}

static unsigned int generateRandomValue(const unsigned int min_val, const unsigned int max_val)
{
    unsigned int value = (min_val + max_val) / 2;
    FILE* file = fopen("/dev/urandom", "rb");
    if (file != NULL) {
        if (fread(&value, 1, sizeof(int), file) != sizeof(int))
            value = (min_val + max_val) / 2;
        fclose(file);
    }
    value = value % (max_val - min_val);
    value += min_val;
    return value;
}

static unsigned int readValFromEnvVar(const char* env_var_name, const unsigned int min_val, const unsigned int max_val)
{
    const char* env_var_val = getenv(env_var_name);
    long int value;

    if (env_var_val == NULL) {
        const unsigned int random_value = generateRandomValue(min_val, max_val);
        fprintf(stderr, "%s environment variable not set. Using a random value (%u).\n", env_var_name, random_value);
        return random_value;
    }
    else if (strToLongInt(env_var_val, &value) || value < min_val || value > max_val) {
        const unsigned int random_value = generateRandomValue(min_val, max_val);
        fprintf(stderr, "%s has incorrect value (%s). Using a random value (%u).\n", env_var_name, env_var_val, random_value);
        return random_value;
    }

    return (unsigned int)value;
}

void activateOverthrower()
{
    fprintf(stderr, "overthrower got activation signal.\n");
    fprintf(stderr, "overthrower will use following parameters for failing allocations:\n");
    strategy = readValFromEnvVar("OVERTHROWER_STRATEGY", STRATEGY_RANDOM, STRATEGY_PULSE);
    fprintf(stderr, "Strategy = %s\n", strategy_names[strategy]);
    if (strategy == STRATEGY_RANDOM) {
        seed = readValFromEnvVar("OVERTHROWER_SEED", 0, UINT_MAX);
        duty_cycle = readValFromEnvVar("OVERTHROWER_DUTY_CYCLE", MIN_DUTY_CYCLE, MAX_DUTY_CYCLE);
        srand(seed);
        fprintf(stderr, "Duty cycle = %u\n", duty_cycle);
        fprintf(stderr, "Seed = %u\n", seed);
    }
    else {
        delay = readValFromEnvVar("OVERTHROWER_DELAY", MIN_DELAY, MAX_DELAY);
        fprintf(stderr, "Delay = %u\n", delay);
        if (strategy == STRATEGY_PULSE) {
            duration = readValFromEnvVar("OVERTHROWER_DURATION", MIN_DURATION, MAX_DURATION);
            fprintf(stderr, "Duration = %u\n", duration);
        }
    }
    activated = 1;
}

static int isTimeTofail()
{
    switch (strategy) {
        case STRATEGY_RANDOM:
            return ((unsigned int)rand() % duty_cycle == 0) ? 1 : 0;
        case STRATEGY_STEP:
            return (__sync_add_and_fetch(&malloc_number, 1) + 1 > delay) ? 1 : 0;
        case STRATEGY_PULSE: {
            unsigned int number = __sync_add_and_fetch(&malloc_number, 1) + 1;
            return (number > delay && number < delay + duration) ? 1 : 0;
        }
        default:
            assert(0);
            return 0;
    }
}

void* malloc(size_t size)
{
    if (native_malloc == NULL)
        initialize();

    if ((activated != 0) && (size != 0) && isTimeTofail())
        return NULL;

    return native_malloc(size);
}
