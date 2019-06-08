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

#include "platform.h"
#include "thread_local.h"

#if !defined(PLATFORM_OS_MAC_OS_X) && !defined(PLATFORM_OS_LINUX)
#error "Unsupported OS"
#endif

#define MAX_STACK_DEPTH 4
#define MAX_PAUSE_DEPTH 16

typedef void* (*Malloc)(size_t size);
typedef void* (*Realloc)(void* pointer, size_t size);
typedef void (*Free)(void* pointer);

#if defined(PLATFORM_OS_LINUX)
static Malloc native_malloc = nullptr;
static Realloc native_realloc = nullptr;
static Free native_free = nullptr;
#endif

#if defined(PLATFORM_OS_MAC_OS_X)
#define my_malloc my_malloc
#define my_free my_free
#define my_realloc my_realloc
#define native_malloc malloc
#define native_free free
#define native_realloc realloc
#elif defined(PLATFORM_OS_LINUX)
#define my_malloc malloc
#define my_free free
#define my_realloc realloc
#define native_malloc native_malloc
#define native_free native_free
#define native_realloc native_realloc
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
#define MAX_RANDOM_DELAY 1000
#define MAX_DELAY 1000000

#define MIN_DURATION 1
#define MAX_DURATION 100

static const char* strategy_names[4] = { "random", "step", "pulse", "none" };

static bool activated = false;
static unsigned int strategy = STRATEGY_RANDOM;
static unsigned int seed = 0;
static unsigned int duty_cycle = 1024;
static unsigned int delay = MIN_DELAY;
static unsigned int duration = MIN_DURATION;
static unsigned int malloc_number = 0;
static unsigned int malloc_seq_num = 0;

struct State {
    bool is_tracing;
    unsigned int paused[MAX_PAUSE_DEPTH + 1];
    unsigned int depth;
};

static thread_local State state{};

#if defined(PLATFORM_OS_MAC_OS_X)
static ThreadLocal<bool> initialized;
static ThreadLocal<bool> initializing;
#elif defined(PLATFORM_OS_LINUX)
static bool initialized;
#endif

struct Info {
    unsigned int seq_num;
    size_t size;
};

template<class T>
class mallocFreeAllocator {
public:
    typedef size_t size_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T value_type;

    template<class U>
    struct rebind {
        typedef mallocFreeAllocator<U> other;
    };

    mallocFreeAllocator() noexcept = default;

    template<class U>
    explicit mallocFreeAllocator(const mallocFreeAllocator<U>&) noexcept
    {
    }

    ~mallocFreeAllocator() noexcept = default;

    pointer allocate(size_type size)
    {
        if (!size)
            return nullptr;
        auto temp = reinterpret_cast<pointer>(nonFailingMalloc(size * sizeof(T)));
        if (!temp)
            throw std::bad_alloc();
        return temp;
    }

    void deallocate(pointer p, size_type) { nonFailingFree(p); }

    void construct(pointer p, const T& val) { new (reinterpret_cast<void*>(p)) T(val); }
    void destroy(pointer p) { p->~T(); }
};

static std::recursive_mutex mutex;
static std::unordered_map<void*, Info, std::hash<void*>, std::equal_to<void*>, mallocFreeAllocator<std::pair<void* const, Info>>> allocated;

extern "C" unsigned int deactivateOverthrower();

static void initialize()
{
    assert(!initialized);
#if defined(PLATFORM_OS_MAC_OS_X)
    initializing = true;
    state = {};
    initializing = false;
#elif defined(PLATFORM_OS_LINUX)
    native_malloc = (Malloc)dlsym(RTLD_NEXT, "malloc");
    native_realloc = (Realloc)dlsym(RTLD_NEXT, "realloc");
    native_free = (Free)dlsym(RTLD_NEXT, "free");
#endif
    initialized = true;
}

__attribute__((constructor)) static void banner()
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

__attribute__((destructor)) static void shutdown()
{
    if (!activated)
        return;

    fprintf(stderr, "overthrower has not been deactivated explicitly, doing it anyway.\n");

    deactivateOverthrower();
}

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
    if (file) {
        if (fread(&value, 1, sizeof(int), file) != sizeof(int))
            value = (min_val + max_val) / 2;
        fclose(file);
    }
    value = value % (max_val - min_val + (max_val == UINT_MAX ? 0 : 1));
    value += min_val;
    return value;
}

static unsigned int readValFromEnvVar(const char* env_var_name, unsigned int min_val, unsigned int max_val, unsigned int max_random_val = 0)
{
    const char* env_var_val = getenv(env_var_name);
    unsigned long int value;

    if (!env_var_val) {
        const unsigned int random_value = generateRandomValue(min_val, max_random_val ? max_random_val : max_val);
        fprintf(stderr, "%s environment variable not set. Using a random value (%u).\n", env_var_name, random_value);
        return random_value;
    }
    else if (strToUnsignedLongInt(env_var_val, &value) || value < min_val || value > max_val) {
        const unsigned int random_value = generateRandomValue(min_val, max_random_val ? max_random_val : max_val);
        fprintf(stderr, "%s has incorrect value (%s). Using a random value (%u).\n", env_var_name, env_var_val, random_value);
        return random_value;
    }

    return (unsigned int)value;
}

extern "C" void activateOverthrower()
{
#if defined(PLATFORM_OS_MAC_OS_X)
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

    malloc_number = 0;
    malloc_seq_num = 0;

    fprintf(stderr, "overthrower got activation signal.\n");
    fprintf(stderr, "overthrower will use following parameters for failing allocations:\n");
    strategy = readValFromEnvVar("OVERTHROWER_STRATEGY", STRATEGY_RANDOM, STRATEGY_NONE, STRATEGY_PULSE);
    fprintf(stderr, "Strategy = %s\n", strategy_names[strategy]);
    if (strategy == STRATEGY_RANDOM) {
        seed = readValFromEnvVar("OVERTHROWER_SEED", 0, UINT_MAX);
        duty_cycle = readValFromEnvVar("OVERTHROWER_DUTY_CYCLE", MIN_DUTY_CYCLE, MAX_DUTY_CYCLE);
        srand(seed);
        fprintf(stderr, "Duty cycle = %u\n", duty_cycle);
        fprintf(stderr, "Seed = %u\n", seed);
    }
    else if (strategy != STRATEGY_NONE) {
        delay = readValFromEnvVar("OVERTHROWER_DELAY", MIN_DELAY, MAX_DELAY, MAX_RANDOM_DELAY);
        fprintf(stderr, "Delay = %u\n", delay);
        if (strategy == STRATEGY_PULSE) {
            duration = readValFromEnvVar("OVERTHROWER_DURATION", MIN_DURATION, MAX_DURATION);
            fprintf(stderr, "Duration = %u\n", duration);
        }
    }
    activated = true;
}

extern "C" unsigned int deactivateOverthrower()
{
    activated = false;

    fprintf(stderr, "overthrower got deactivation signal.\n");
    fprintf(stderr, "overthrower will not fail allocations anymore.\n");

    if (!allocated.empty()) {
        fprintf(stderr, "overthrower has detected not freed memory blocks with following addresses:\n");
        for (const auto& v : allocated)
            fprintf(stderr, "0x%016" PRIxPTR " - %6d - %6zd\n", (uintptr_t)v.first, v.second.seq_num, v.second.size);
    }

    const auto blocks_leaked = static_cast<unsigned int>(allocated.size());
    allocated.clear();
    state = {};
    return blocks_leaked;
}

extern "C" void pauseOverthrower(unsigned int duration)
{
#if defined(PLATFORM_OS_MAC_OS_X)
    if (!initialized)
        initialize();
#endif

    duration = duration == 0 ? UINT_MAX : duration;

    if (state.depth == MAX_PAUSE_DEPTH) {
        fprintf(stderr, "pause stack overflow detected.\n");
        state.paused[MAX_PAUSE_DEPTH] = duration;
        return;
    }

    state.paused[++state.depth] = duration;
}

extern "C" void resumeOverthrower()
{
    if (state.depth == 0) {
        fprintf(stderr, "pause stack underflow detected.\n");
        return;
    }

    --state.depth;
}

static bool isTimeToFail()
{
    switch (strategy) {
        case STRATEGY_RANDOM:
            return rand() % duty_cycle == 0;
        case STRATEGY_STEP:
            return __sync_add_and_fetch(&malloc_number, 1) > delay;
        case STRATEGY_PULSE: {
            const unsigned int number = __sync_add_and_fetch(&malloc_number, 1);
            return number > delay && number <= delay + duration;
        }
        case STRATEGY_NONE:
            return false;
        default:
            assert(false);
            return false;
    }
}

void* nonFailingMalloc(size_t size)
{
#if defined(PLATFORM_OS_MAC_OS_X)
    return malloc(size);
#elif defined(PLATFORM_OS_LINUX)
    return native_malloc ? native_malloc(size) : malloc(size);
#endif
}

void nonFailingFree(void* pointer)
{
    native_free(pointer);
}

static void searchKnowledgeBase(bool& is_in_white_list, bool& is_in_ignore_list)
{
    void* callstack[MAX_STACK_DEPTH];
    const int count = backtrace(callstack, MAX_STACK_DEPTH);
    char** symbols = backtrace_symbols(callstack, count);

    if (!symbols) {
        is_in_white_list = true;
        is_in_ignore_list = true;
        return;
    }

    is_in_white_list = false;
    is_in_ignore_list = false;

#if defined(PLATFORM_OS_MAC_OS_X)
    if (count >= 4 && (strstr(symbols[3], "__cxa_allocate_exception") || strstr(symbols[2], "__cxa_allocate_exception")))
        is_in_white_list = true;
    if (count >= 4 && (strstr(symbols[3], "__cxa_atexit") || strstr(symbols[2], "__cxa_atexit"))) {
        is_in_white_list = true;
        is_in_ignore_list = true;
    }
#elif defined(PLATFORM_OS_LINUX)
    if (count >= 3 && (strstr(symbols[2], "__cxa_allocate_exception") || strstr(symbols[1], "__cxa_allocate_exception")))
        is_in_white_list = true;
    if (count >= 3 && strstr(symbols[2], "ld-linux"))
        is_in_ignore_list = true;
    if (count >= 4 && strstr(symbols[3], "dlerror"))
        is_in_ignore_list = true;
#endif

#if 0
    for (int i = 1; i < count; ++i )
        printf("%d: %s\n", i, symbols[i]);
#endif

    free(symbols);
}

void* my_malloc(size_t size)
{
    void* pointer;

#if defined(PLATFORM_OS_MAC_OS_X)
    if (initializing)
        return nonFailingMalloc(size);
#endif

    if (!initialized)
        initialize();

    bool is_in_white_list = state.is_tracing;
    bool is_in_ignore_list = false;

    const unsigned int effective_depth = state.depth > MAX_PAUSE_DEPTH ? MAX_PAUSE_DEPTH : state.depth;

    if (!state.is_tracing) {
        state.is_tracing = true;
        const unsigned int old_paused = state.paused[effective_depth];
        state.paused[effective_depth] = UINT_MAX;
        searchKnowledgeBase(is_in_white_list, is_in_ignore_list);
        state.paused[effective_depth] = old_paused;
        state.is_tracing = false;
    }

    if (!is_in_white_list && activated && !state.paused[effective_depth] && size && isTimeToFail()) {
        errno = ENOMEM;
        return nullptr;
    }

    pointer = nonFailingMalloc(size);

    if (!is_in_ignore_list && activated && !state.paused[effective_depth] && pointer) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        malloc_seq_num++;
        allocated.insert({ pointer, { malloc_seq_num, size } });
    }

    if (state.paused[effective_depth])
        --state.paused[effective_depth];

    return pointer;
}

void my_free(void* pointer)
{
    const int old_errno = errno;
    if (activated && pointer) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        allocated.erase(pointer);
    }

    native_free(pointer);
    errno = old_errno;
}

void* my_realloc(void* pointer, size_t size)
{
    if (!pointer)
        return my_malloc(size);

    if (!size) {
        my_free(pointer);
        return nullptr;
    }

    if (!allocated.count(pointer))
        return native_realloc(pointer, size);

    const size_t old_size = allocated.at(pointer).size;
    void* new_ptr = my_malloc(size);

    if (!new_ptr)
        return nullptr;

    memcpy(new_ptr, pointer, old_size < size ? old_size : size);
    my_free(pointer);

    return new_ptr;
}

#if defined(PLATFORM_OS_MAC_OS_X)
typedef struct interpose_s {
    void* substitute;
    void* original;
} interpose_t;

__attribute__((used)) static const interpose_t interposing_functions[] __attribute__((
    section("__DATA, __interpose"))) = { { (void*)my_malloc, (void*)malloc }, { (void*)my_realloc, (void*)realloc }, { (void*)my_free, (void*)free } };
#endif
