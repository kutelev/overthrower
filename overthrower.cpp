#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform.h"

#if defined(PLATFORM_OS_LINUX)
#include <dlfcn.h>
#endif
#include <execinfo.h>

#include <mutex>
#include <new>
#include <unordered_map>

#if defined(PLATFORM_OS_MAC_OS_X)
#include "thread_local.h"
#endif

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
static std::atomic<unsigned int> malloc_counter{};

struct State {
    bool is_tracing;
    unsigned int paused[MAX_PAUSE_DEPTH + 1];
    unsigned int depth;
};

static thread_local State state{};

#if defined(PLATFORM_OS_MAC_OS_X)
static ThreadLocal<bool> initialized;  // NOLINT
static ThreadLocal<bool> initializing; // NOLINT
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
        assert(size > 0); // Do not expect STL containers to allocate memory blocks having no size.
        auto temp = reinterpret_cast<pointer>(nonFailingMalloc(size * sizeof(T)));
        if (!temp)
            throw std::bad_alloc();
        return temp;
    }

    void deallocate(pointer p, size_type) { nonFailingFree(p); }

    void construct(pointer p, const T& val) { new (reinterpret_cast<void*>(p)) T(val); }
    void destroy(pointer p) { p->~T(); }
};

static std::recursive_mutex mutex;                                                                                                           // NOLINT
static std::unordered_map<void*, Info, std::hash<void*>, std::equal_to<void*>, mallocFreeAllocator<std::pair<void* const, Info>>> allocated; // NOLINT

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

__attribute__((constructor, used)) static void banner()
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

__attribute__((destructor, used)) static void shutdown()
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
    // Mac OS X implementation uses malloc inside printf generation of functions. If malloc fail, printf-like functions may crash.
    // To prevent crashes we have to force printf to do all his allocations before we activate the overthrower.
    static const int integer_number = 22708089;
    static const double floating_point_number = 22708089.862725008;
    char tmp_buf[1024];
    for (int i = 0; i < 1000; ++i)
        sprintf(tmp_buf, "%d%f\n", integer_number * i * i, floating_point_number * i * i);
    printf("overthrower have to print useless string to force printf to do all preallocations: %s", tmp_buf);
#endif

    malloc_counter = 0;

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
    state = {};

    fprintf(stderr, "overthrower got deactivation signal.\n");
    fprintf(stderr, "overthrower will not fail allocations anymore.\n");

    if (allocated.empty())
        return 0;

    fprintf(stderr, "overthrower has detected not freed memory blocks with following addresses:\n");
    for (const auto& v : allocated)
        fprintf(stderr, "0x%016" PRIxPTR "  -  %6d  -  %10zd\n", (uintptr_t)v.first, v.second.seq_num, v.second.size);

    fprintf(stderr, "^^^^^^^^^^^^^^^^^^  |  ^^^^^^  |  ^^^^^^^^^^\n");
    fprintf(stderr, "      pointer       |  malloc  |  block size\n");
    fprintf(stderr, "                    |invocation|\n");
    fprintf(stderr, "                    |  number  |\n");

    const auto blocks_leaked = static_cast<unsigned int>(allocated.size());
    allocated.clear();
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

static bool isTimeToFail(unsigned int malloc_seq_num)
{
    switch (strategy) {
        case STRATEGY_RANDOM:
            return rand() % duty_cycle == 0;
        case STRATEGY_STEP:
            return malloc_seq_num >= delay;
        case STRATEGY_PULSE:
            return malloc_seq_num > delay && malloc_seq_num <= delay + duration;
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
    if (count >= 4 && (strstr(symbols[3], "__cxa_allocate_exception") || strstr(symbols[2], "__cxa_allocate_exception"))) {
        // This branch is reachable with macOS 10.14 / Xcode 10 and older.
        // In newer environments some other mechanism seems to be used for allocating exception objects.
        is_in_white_list = true;
    }

    // __cxa_atexit is not supposed to be used explicitly but overthrower needs to be aware of existence of this function:
    // Allocations which come from __cxa_atexit shall not be failed by overthrower.
    // Memory which is allocated by __cxa_atexit shall not be treated as memory leak.
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
#if defined(PLATFORM_OS_MAC_OS_X)
    if (initializing)
        return nonFailingMalloc(size);
#endif

    if (!initialized)
        initialize();

    if (!activated)
        return nonFailingMalloc(size);

    const unsigned int depth = state.depth;
    assert(depth <= MAX_PAUSE_DEPTH);

    bool is_in_white_list = state.is_tracing;
    bool is_in_ignore_list = false;

    if (!state.is_tracing) {
        state.is_tracing = true;
        const unsigned int old_paused = state.paused[depth];
        state.paused[depth] = UINT_MAX;
        searchKnowledgeBase(is_in_white_list, is_in_ignore_list);
        state.paused[depth] = old_paused;
        state.is_tracing = false;
    }

    if (state.paused[depth]) {
        --state.paused[depth];
        return nonFailingMalloc(size);
    }

    const unsigned int malloc_seq_num = malloc_counter++;

    if (is_in_white_list || !size)
        return nonFailingMalloc(size);

    if (isTimeToFail(malloc_seq_num)) {
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = nonFailingMalloc(size);

    if (!pointer) {
        // Real OOM, in this case overthrower itself is being overthrown by an OS.
        return nullptr;
    }

    // is_in_ignore_list is never true alone on macOS that is why we do not use return ASAP approach at this place.
    if (!is_in_ignore_list) {
        // Register all allocations which are not in the ignore list.
        // All registered and not freed memory blocks are considered to be memory leaks.
        try {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            // Maybe I should have used emplace instead of insert but it is not possible due to incapability of GCC 4.8 dealing with it
            allocated.insert({ pointer, { malloc_seq_num, size } });
        }
        catch (const std::bad_alloc&) {
            // Real OOM, in this case overthrower itself is being overthrown by an OS.
            nonFailingFree(pointer);
            return nullptr;
        }
    }

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

    memcpy(new_ptr, pointer, std::min(old_size, size));
    my_free(pointer);

    return new_ptr;
}

#if defined(PLATFORM_OS_MAC_OS_X)
struct interpose_t {
    void* substitute;
    void* original;
};

__attribute__((used)) static const interpose_t interposing_functions[] __attribute__((section("__DATA, __interpose"))) = {
    { reinterpret_cast<void*>(my_malloc), reinterpret_cast<void*>(malloc) },   //
    { reinterpret_cast<void*>(my_realloc), reinterpret_cast<void*>(realloc) }, //
    { reinterpret_cast<void*>(my_free), reinterpret_cast<void*>(free) },       //
};
#endif
