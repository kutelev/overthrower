#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>

typedef void* (*Malloc)(size_t size);
typedef void (*Free)(void* pointer);

static Malloc native_malloc = NULL;
static Free native_free = NULL;

void* nonFailingMalloc(size_t size);

#define STRATEGY_RANDOM 0
#define STRATEGY_STEP 1
#define STRATEGY_PULSE 2

#define MIN_DUTY_CYCLE 1
#define MAX_DUTY_CYCLE 4096

#define MIN_DELAY 0
#define MAX_DELAY 1000

#define MIN_DURATION 1
#define MAX_DURATION 100

static const char* strategy_names[3] = { "random", "step", "pulse" };

static unsigned int activated = 0;
static unsigned int paused = 0;
static unsigned int strategy = STRATEGY_RANDOM;
static unsigned int seed = 0;
static unsigned int duty_cycle = 1024;
static unsigned delay = MIN_DELAY;
static unsigned duration = MIN_DURATION;
static unsigned int malloc_number = 0;

#if !defined(__APPLE__)
template<class T>
class mallocFreeAllocator {
public:
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T value_type;

    template<class U>
    struct rebind {
        typedef mallocFreeAllocator<U> other;
    };

    mallocFreeAllocator() throw() {}
    mallocFreeAllocator(const mallocFreeAllocator&) throw() {}

    template<class U>
    mallocFreeAllocator(const mallocFreeAllocator<U>&) throw()
    {
    }

    ~mallocFreeAllocator() throw() {}

    pointer address(reference x) const { return &x; }
    const_pointer address(const_reference x) const { return &x; }

    pointer allocate(size_type s, void const* = 0)
    {
        if (0 == s)
            return NULL;
        pointer temp = (pointer)nonFailingMalloc(s * sizeof(T));
        if (temp == NULL)
            throw std::bad_alloc();
        return temp;
    }

    void deallocate(pointer p, size_type) { native_free(p); }
    size_type max_size() const throw() { return std::numeric_limits<size_t>::max() / sizeof(T); }
    void construct(pointer p, const T& val) { new ((void*)p) T(val); }
    void destroy(pointer p) { p->~T(); }
};

static std::mutex mutex;
static std::unordered_set<void*, std::hash<void*>, std::equal_to<void*>, mallocFreeAllocator<void*>> allocated;
#endif

__attribute__((constructor)) static void banner()
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

#if !defined(__APPLE__)
static void initialize()
{
    if (native_malloc == NULL) {
        native_malloc = (Malloc)dlsym(RTLD_NEXT, "malloc");
        native_free = (Free)dlsym(RTLD_NEXT, "free");
    }
}
#endif

static int strToUnsignedLongInt(const char* str, unsigned long int* value)
{
    char* end_ptr;
    *value = strtoul(str, &end_ptr, 10);

    if (end_ptr[0] != '\0')
        return -1;

    if ((*value == 0 || *value == ULONG_MAX) && errno == ERANGE)
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
    value = value % (max_val - min_val + (max_val == UINT_MAX ? 0 : 1));
    value += min_val;
    return value;
}

static unsigned int readValFromEnvVar(const char* env_var_name, const unsigned int min_val, const unsigned int max_val)
{
    const char* env_var_val = getenv(env_var_name);
    unsigned long int value;

    if (env_var_val == NULL) {
        const unsigned int random_value = generateRandomValue(min_val, max_val);
        fprintf(stderr, "%s environment variable not set. Using a random value (%u).\n", env_var_name, random_value);
        return random_value;
    }
    else if (strToUnsignedLongInt(env_var_val, &value) || value < min_val || value > max_val) {
        const unsigned int random_value = generateRandomValue(min_val, max_val);
        fprintf(stderr, "%s has incorrect value (%s). Using a random value (%u).\n", env_var_name, env_var_val, random_value);
        return random_value;
    }

    return (unsigned int)value;
}

extern "C" void activateOverthrower()
{
#if defined(__APPLE__)
    // Mac OS X implementation uses malloc inside printf.
    // To prevent crashes we have to force printf to do all his allocations before we activated the overthrower.
    static const int integer_number = 22708089;
    static const double floating_point_number = 22708089.862725008;
    char tmp_buf[1024];
    for (int i = 0; i < 1000; ++i)
        sprintf(tmp_buf, "%d%f\n", integer_number * i * i, floating_point_number * i * i);
#endif

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

extern "C" int deactivateOverthrower()
{
    activated = 0;

    fprintf(stderr, "overthrower got deactivation signal.\n");
    fprintf(stderr, "overthrower will not fail allocations anymore.\n");

#if defined(__APPLE__)
    return 0;
#else
    return allocated.size();
#endif
}

extern "C" void pauseOverthrower(unsigned int duration)
{
    paused = duration == 0 ? UINT_MAX : duration;
}

extern "C" void resumeOverthrower()
{
    paused = 0;
}

static int isTimeToFail()
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

void* nonFailingMalloc(size_t size)
{
#if defined(__APPLE__)
    return malloc(size);
#else
    return native_malloc ? native_malloc(size) : malloc(size);
#endif
}

#if defined(__APPLE__)
void* my_malloc(size_t size)
#else
void* malloc(size_t size)
#endif
{
    void* pointer;

#if !defined(__APPLE__)
    if (native_malloc == NULL)
        initialize();
#endif

    if ((activated != 0) && (paused == 0) && (size != 0) && isTimeToFail())
        return NULL;

    if (paused)
        --paused;
    
    pointer = nonFailingMalloc(size);

#if !defined(__APPLE__)
    if (activated != 0 && pointer != NULL) {
        std::lock_guard<std::mutex> lock(mutex);
        allocated.insert(pointer);
    }
#endif

    return pointer;
}

#if defined(__APPLE__)
void my_free(void* pointer)
#else
void free(void* pointer)
#endif
{
#if !defined(__APPLE__)
    if (activated != 0 && pointer != NULL) {
        std::lock_guard<std::mutex> lock(mutex);
        allocated.erase(pointer);
    }
#endif

#if defined(__APPLE__)
    free(pointer);
#else
    native_free(pointer);
#endif
}

#if defined(__APPLE__)
typedef struct interpose_s {
    void* substitute;
    void* original;
} interpose_t;

__attribute__((used)) static const interpose_t interposing_functions[]
    __attribute__((section("__DATA, __interpose"))) = { { (void*)my_malloc, (void*)malloc }, { (void*)my_free, (void*)free } };
#endif
