#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>

#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

#if defined(__APPLE__)
#define MAX_STACK_DEPTH 4
#else
#define MAX_STACK_DEPTH 3
#endif

typedef void* (*Malloc)(size_t size);
typedef void (*Free)(void* pointer);

#if !defined(__APPLE__)
static Malloc native_malloc = NULL;
static Free native_free = NULL;
#endif

void* nonFailingMalloc(size_t size);
void nonFailingFree(void* pointer);

#define STRATEGY_RANDOM 0
#define STRATEGY_STEP 1
#define STRATEGY_PULSE 2
#define STRATEGY_NONE 3

#define MIN_DUTY_CYCLE 1
#define MAX_DUTY_CYCLE 4096

#define MIN_DELAY 0
#define MAX_DELAY 1000

#define MIN_DURATION 1
#define MAX_DURATION 100

static const char* strategy_names[4] = { "random", "step", "pulse", "none" };

static unsigned int activated = 0;
static unsigned int paused = 0;
static unsigned int strategy = STRATEGY_RANDOM;
static unsigned int seed = 0;
static unsigned int duty_cycle = 1024;
static unsigned delay = MIN_DELAY;
static unsigned duration = MIN_DURATION;
static unsigned int malloc_number = 0;
static unsigned int malloc_seq_num = 0;

template<typename T>
class ThreadLocal final {
public:
    ThreadLocal() { pthread_key_create(&key, NULL); }
    ~ThreadLocal() { pthread_key_delete(key); }

    void operator=(const T* value) { set(value); }

    const T* operator*() const { return get(); }
    T* operator->() const { return get(); }
    operator bool() const { return get(); }
    operator T*() const { return get(); }

private:
    void set(const T* value) { pthread_setspecific(key, value); }
    T* get() const { return reinterpret_cast<T*>(pthread_getspecific(key)); }

    pthread_key_t key;
};

static ThreadLocal<void> is_tracing;

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

    void deallocate(pointer p, size_type) { nonFailingFree(p); }
    size_type max_size() const throw() { return std::numeric_limits<size_t>::max() / sizeof(T); }
    void construct(pointer p, const T& val) { new ((void*)p) T(val); }
    void destroy(pointer p) { p->~T(); }
};

static std::mutex mutex;
static std::unordered_map<void*, unsigned int, std::hash<void*>, std::equal_to<void*>, mallocFreeAllocator<std::pair<void* const, unsigned int>>> allocated;

extern "C" int deactivateOverthrower();

__attribute__((constructor)) static void banner()
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

__attribute__((destructor)) static void shutdown()
{
    if (activated == 0)
        return;

    fprintf(stderr, "overthrower has not been deactivated explicitly, doing it anyway.\n");

    deactivateOverthrower();
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
    // To prevent crashes we have to force printf to do all his allocations before
    // we activated the overthrower.
    static const int integer_number = 22708089;
    static const double floating_point_number = 22708089.862725008;
    char tmp_buf[1024];
    for (int i = 0; i < 1000; ++i)
        sprintf(tmp_buf, "%d%f\n", integer_number * i * i, floating_point_number * i * i);
    printf("overthrower have to print useless string to force printf to do all preallocations: %s", tmp_buf);
#endif

    fprintf(stderr, "overthrower got activation signal.\n");
    fprintf(stderr, "overthrower will use following parameters for failing allocations:\n");
    strategy = readValFromEnvVar("OVERTHROWER_STRATEGY", STRATEGY_RANDOM, STRATEGY_NONE);
    fprintf(stderr, "Strategy = %s\n", strategy_names[strategy]);
    if (strategy == STRATEGY_RANDOM) {
        seed = readValFromEnvVar("OVERTHROWER_SEED", 0, UINT_MAX);
        duty_cycle = readValFromEnvVar("OVERTHROWER_DUTY_CYCLE", MIN_DUTY_CYCLE, MAX_DUTY_CYCLE);
        srand(seed);
        fprintf(stderr, "Duty cycle = %u\n", duty_cycle);
        fprintf(stderr, "Seed = %u\n", seed);
    }
    else if (strategy != STRATEGY_NONE) {
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

    if (!allocated.empty()) {
        fprintf(stderr, "overthrower has detected not freed memory blocks with following addresses:\n");
        for (const auto& v : allocated)
            fprintf(stderr, "0x%016" PRIxPTR " - %6d\n", (uintptr_t)v.first, v.second);
    }
    return allocated.size();
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
        case STRATEGY_NONE:
            return 0;
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

void nonFailingFree(void* pointer)
{
#if defined(__APPLE__)
    return free(pointer);
#else
    return native_free(pointer);
#endif
}

static void searchKnowledgeBase(bool& is_in_white_list, bool& is_in_ignore_list)
{
    void* callstack[MAX_STACK_DEPTH];
    const int count = backtrace(callstack, MAX_STACK_DEPTH);
    char** symbols = backtrace_symbols(callstack, count);

    if (!symbols) {
        is_in_white_list = true;
        is_in_ignore_list = true;
    }

    is_in_white_list = false;
    is_in_ignore_list = false;

#if defined(__APPLE__)
    if (count >= 4 && strstr(symbols[3], "__cxa_allocate_exception"))
        is_in_white_list = true;
#else
    if (count >= 2 && strstr(symbols[1], "__cxa_allocate_exception"))
        is_in_white_list = true;
    if (count >= 3 && strstr(symbols[2], "ld-linux"))
        is_in_ignore_list = true;
#endif

#if 0
    for (int i = 1; i < count; ++i )
        printf("%s\n", symbols[i]);
#endif

    free(symbols);
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

    bool is_in_white_list = is_tracing;
    bool is_in_ignore_list = false;

    if (!is_tracing) {
        is_tracing = reinterpret_cast<void*>(-1);
        const unsigned int old_paused = paused;
        pauseOverthrower(0);
        searchKnowledgeBase(is_in_white_list, is_in_ignore_list);
        paused = old_paused;
        is_tracing = nullptr;
    }

    if (!is_in_white_list && (activated != 0) && (paused == 0) && (size != 0) && isTimeToFail()) {
        errno = ENOMEM;
        return NULL;
    }

    pointer = nonFailingMalloc(size);

    if (!is_in_ignore_list && activated != 0 && pointer != NULL && paused == 0) {
        std::lock_guard<std::mutex> lock(mutex);
        malloc_seq_num++;
        allocated.insert({ pointer, malloc_seq_num });
    }

    if (paused)
        --paused;

    return pointer;
}

#if defined(__APPLE__)
void my_free(void* pointer)
#else
void free(void* pointer)
#endif
{
    int old_errno = errno;
    if (activated != 0 && pointer != NULL) {
        std::lock_guard<std::mutex> lock(mutex);
        allocated.erase(pointer);
    }

#if defined(__APPLE__)
    free(pointer);
#else
    native_free(pointer);
#endif
    errno = old_errno;
}

#if defined(__APPLE__)
typedef struct interpose_s {
    void* substitute;
    void* original;
} interpose_t;

__attribute__((used)) static const interpose_t interposing_functions[]
    __attribute__((section("__DATA, __interpose"))) = { { (void*)my_malloc, (void*)malloc }, { (void*)my_free, (void*)free } };
#endif
