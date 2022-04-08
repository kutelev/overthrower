#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <array>
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

#if defined(WITH_LIBUNWIND) // libunwind is basically available on Linux only.
#define UNW_LOCAL_ONLY
#include <cxxabi.h> // for __cxa_demangle
#include <libunwind.h>
#else
#include <execinfo.h>
#endif
#if defined(PLATFORM_OS_LINUX)
#include <dlfcn.h>
#endif

#include <mutex>
#include <unordered_map>

#if defined(PLATFORM_OS_MAC_OS_X)
#include "thread_local.h"
#endif

#if !defined(PLATFORM_OS_MAC_OS_X) && !defined(PLATFORM_OS_LINUX)
#error "Unsupported OS"
#endif

#if defined(PLATFORM_OS_LINUX)
#define MAX_STACK_DEPTH 7
#elif defined(PLATFORM_OS_MAC_OS_X)
#define MAX_STACK_DEPTH 5
#endif
#define MAX_STACK_DEPTH_VERBOSE 256
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

void* nonFailingMalloc(size_t size) noexcept;
void nonFailingFree(void* pointer) noexcept;

enum {
    STRATEGY_RANDOM = 0U,
    STRATEGY_STEP = 1U,
    STRATEGY_PULSE = 2U,
    STRATEGY_NONE = 3U,
};

#define MIN_DUTY_CYCLE 1
#define MAX_DUTY_CYCLE 4096

#define MIN_DELAY 0
#define MAX_RANDOM_DELAY 1000
#define MAX_DELAY 1000000

#define MIN_DURATION 1
#define MAX_DURATION 100

enum {
    VERBOSE_NO = 0U,
    VERBOSE_FAILED_ALLOCATIONS = 1U,
    VERBOSE_ALL_ALLOCATIONS = 2U,
};

static std::array<const char*, 4> g_strategy_names{ "random", "step", "pulse", "none" };

static bool g_activated = false;
static bool g_self_overthrow = false;
static unsigned int g_verbose_mode = VERBOSE_NO;
static unsigned int g_strategy = STRATEGY_RANDOM;
static unsigned int g_seed = 0;
static unsigned int g_duty_cycle = 1024;
static unsigned int g_delay = MIN_DELAY;
static unsigned int g_duration = MIN_DURATION;
static std::atomic<unsigned int> g_malloc_counter{};

struct State {
    bool is_tracing;
    unsigned int paused[MAX_PAUSE_DEPTH + 1];
    unsigned int depth;
};

static thread_local State g_state{};

#if defined(PLATFORM_OS_MAC_OS_X)
static ThreadLocal<bool> g_initialized;
static ThreadLocal<bool> g_initializing;
#elif defined(PLATFORM_OS_LINUX)
static bool g_initialized = false;
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
    typedef T& reference;
#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
    // This is falsely detected as unused.
    typedef T value_type;
#pragma clang diagnostic pop

    mallocFreeAllocator() noexcept = default;

    template<class U>
    explicit mallocFreeAllocator(const mallocFreeAllocator<U>&) noexcept
    {
    }

    ~mallocFreeAllocator() noexcept = default;

    pointer allocate(size_type size)
    {
        assert(size > 0);                                                          // Do not expect STL containers to allocate memory blocks having no size.
        auto temp = reinterpret_cast<pointer>(nonFailingMalloc(size * sizeof(T))); // NOLINT(bugprone-sizeof-expression)
        if (!temp) {
            throw std::bad_alloc(); // Real OOM
        }
        return temp;
    }

#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
    // This is falsely detected as unused.
    void deallocate(pointer p, size_type) noexcept
    {
        nonFailingFree(p);
    }
#pragma clang diagnostic pop

    void construct(pointer p, const T& val)
    {
        new (reinterpret_cast<void*>(p)) T(val);
    }
};

static std::recursive_mutex g_mutex;                                                                                                           // NOLINT
static std::unordered_map<void*, Info, std::hash<void*>, std::equal_to<void*>, mallocFreeAllocator<std::pair<void* const, Info>>> g_allocated; // NOLINT

extern "C" unsigned int deactivateOverthrower() noexcept;

static void initialize() noexcept
{
    assert(!g_initialized);
#if defined(PLATFORM_OS_MAC_OS_X)
    g_initializing = true;
    g_state = {};
    g_initializing = false;
#elif defined(PLATFORM_OS_LINUX)
    native_malloc = reinterpret_cast<decltype(native_malloc)>(dlsym(RTLD_NEXT, "malloc"));
    native_realloc = reinterpret_cast<decltype(native_realloc)>(dlsym(RTLD_NEXT, "realloc"));
    native_free = reinterpret_cast<decltype(native_free)>(dlsym(RTLD_NEXT, "free"));
#endif
    g_initialized = true;
}

__attribute__((constructor, used)) static void banner() noexcept
{
    fprintf(stderr, "overthrower is waiting for the activation signal ...\n");
    fprintf(stderr, "Invoke activateOverthrower and overthrower will start his job.\n");
}

__attribute__((destructor, used)) static void shutdown() noexcept
{
    if (!g_activated)
        return;

    fprintf(stderr, "overthrower has not been deactivated explicitly, doing it anyway.\n");

    deactivateOverthrower();
}

static int strToUnsignedLongInt(const char* str, unsigned long int* value) noexcept
{
    char* end_ptr;
    errno = 0;
    *value = strtoul(str, &end_ptr, 10);

    if (end_ptr[0] != '\0')
        return -1;

    if ((*value == 0 || *value == ULONG_MAX) && errno == ERANGE)
        return -1;

    return 0;
}

static unsigned int generateRandomValue(const unsigned int min_val, const unsigned int max_val) noexcept
{
    unsigned int value = (min_val + max_val) / 2;
    FILE* file = fopen("/dev/urandom", "rb");
    if (file) {
        unsigned int tmp_value;
        // Very unlikely that fread will ever read less than sizeof(int) bytes.
        // Still be ready to all kinds oddities.
        if (fread(&tmp_value, 1, sizeof(int), file) == sizeof(int)) {
            value = tmp_value;
        }
        fclose(file);
    }
    value = value % (max_val - min_val + (max_val == UINT_MAX ? 0 : 1));
    value += min_val;
    return value;
}

static unsigned int readValFromEnvVar(const char* env_var_name,
                                      unsigned int min_val,
                                      unsigned int max_val,
                                      unsigned int max_random_val = 0,
                                      unsigned int default_value = UINT_MAX) noexcept
{
    const char* env_var_val = getenv(env_var_name);
    unsigned long int value;

    if (default_value != UINT_MAX) {
        if (!env_var_val) {
            return default_value;
        }
        if (strToUnsignedLongInt(env_var_val, &value) || value < min_val || value > max_val) {
            fprintf(stderr, "%s has incorrect value (%s). Using a default value (%u).\n", env_var_name, env_var_val, default_value);
            return default_value;
        }
        return static_cast<unsigned int>(value);
    }

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

    return static_cast<unsigned int>(value);
}

extern "C" __attribute__((visibility("default"))) void activateOverthrower() noexcept
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

    g_malloc_counter = 0;

    fprintf(stderr, "overthrower got activation signal.\n");
    fprintf(stderr, "overthrower will use following parameters for failing allocations:\n");
    g_strategy = readValFromEnvVar("OVERTHROWER_STRATEGY", STRATEGY_RANDOM, STRATEGY_NONE, STRATEGY_PULSE);
    fprintf(stderr, "Strategy = %s\n", g_strategy_names[g_strategy]);
    if (g_strategy == STRATEGY_RANDOM) {
        g_seed = readValFromEnvVar("OVERTHROWER_SEED", 0, UINT_MAX);
        g_duty_cycle = readValFromEnvVar("OVERTHROWER_DUTY_CYCLE", MIN_DUTY_CYCLE, MAX_DUTY_CYCLE);
        srand(g_seed);
        fprintf(stderr, "Duty cycle = %u\n", g_duty_cycle);
        fprintf(stderr, "Seed = %u\n", g_seed);
    }
    else if (g_strategy != STRATEGY_NONE) {
        g_delay = readValFromEnvVar("OVERTHROWER_DELAY", MIN_DELAY, MAX_DELAY, MAX_RANDOM_DELAY);
        fprintf(stderr, "Delay = %u\n", g_delay);
        if (g_strategy == STRATEGY_PULSE) {
            g_duration = readValFromEnvVar("OVERTHROWER_DURATION", MIN_DURATION, MAX_DURATION);
            fprintf(stderr, "Duration = %u\n", g_duration);
        }
    }

    g_self_overthrow = getenv("OVERTHROWER_SELF_OVERTHROW") != nullptr;
    fprintf(stderr, "Self overthrow mode = %s\n", g_self_overthrow ? "enabled" : "disabled");

    g_verbose_mode = readValFromEnvVar("OVERTHROWER_VERBOSE", VERBOSE_NO, VERBOSE_ALL_ALLOCATIONS, 0U, VERBOSE_NO);
    fprintf(stderr, "Verbose mode = %u\n", g_verbose_mode);

    g_activated = true;
}

extern "C" __attribute__((visibility("default"))) unsigned int deactivateOverthrower() noexcept
{
    g_self_overthrow = false;
    g_activated = false;
    g_state = {};

    fprintf(stderr, "overthrower got deactivation signal.\n");
    fprintf(stderr, "overthrower will not fail allocations anymore.\n");

    if (g_allocated.empty())
        return 0;

    fprintf(stderr, "overthrower has detected not freed memory blocks with following addresses:\n");
    for (const auto& v : g_allocated)
        fprintf(stderr, "0x%016" PRIxPTR "  -  %6d  -  %10zd\n", (uintptr_t)v.first, v.second.seq_num, v.second.size);

    fprintf(stderr, "^^^^^^^^^^^^^^^^^^  |  ^^^^^^  |  ^^^^^^^^^^\n");
    fprintf(stderr, "      pointer       |  malloc  |  block size\n");
    fprintf(stderr, "                    |invocation|\n");
    fprintf(stderr, "                    |  number  |\n");

    const auto blocks_leaked = static_cast<unsigned int>(g_allocated.size());
    g_allocated.clear();
    return blocks_leaked;
}

extern "C" __attribute__((visibility("default"))) void pauseOverthrower(unsigned int duration) noexcept
{
#if defined(PLATFORM_OS_MAC_OS_X)
    if (!g_initialized)
        initialize();
#endif

    duration = duration == 0 ? UINT_MAX : duration;

    if (g_state.depth == MAX_PAUSE_DEPTH) {
        fprintf(stderr, "pause stack overflow detected.\n");
        g_state.paused[MAX_PAUSE_DEPTH] = duration;
        return;
    }

    g_state.paused[++g_state.depth] = duration;
}

extern "C" __attribute__((visibility("default"))) void resumeOverthrower() noexcept
{
    if (g_state.depth == 0) {
        fprintf(stderr, "pause stack underflow detected.\n");
        return;
    }

    --g_state.depth;
}

static bool isTimeToFail(unsigned int malloc_seq_num) noexcept
{
    switch (g_strategy) {
        case STRATEGY_RANDOM:
            return rand() % g_duty_cycle == 0; // NOLINT
        case STRATEGY_STEP:
            return malloc_seq_num >= g_delay;
        case STRATEGY_PULSE:
            return malloc_seq_num > g_delay && malloc_seq_num <= g_delay + g_duration;
        case STRATEGY_NONE:
        default: // Just to make static code analyzers fully happy.
            return false;
    }
}

void* nonFailingMalloc(size_t size) noexcept
{
    if (g_self_overthrow && (rand() % 2) == 0) { // NOLINT
        // By doing this we emulate real OOM conditions where native malloc can really return nullptr.
        // This may happen if tests are run on a system which is running out of resources.
        return nullptr;
    }

#if defined(PLATFORM_OS_MAC_OS_X)
    return malloc(size);
#elif defined(PLATFORM_OS_LINUX)
    return native_malloc ? native_malloc(size) : malloc(size);
#endif
}

void nonFailingFree(void* pointer) noexcept
{
    native_free(pointer);
}

// returns is_in_white_list, is_in_ignore_list pair.
typedef std::pair<bool, bool> (
    *BacktraceCallback)(unsigned int depth, uintptr_t ip, uintptr_t sp, const char* library_name, const char* func_name, uintptr_t off);

#if defined(WITH_LIBUNWIND)
// ip - instruction pointer
// sp - stack pointer
static std::pair<bool, bool> printFrameInfo(unsigned int depth,
                                            uintptr_t ip,
                                            uintptr_t sp,
                                            const char* library_name,
                                            const char* func_name,
                                            uintptr_t off) noexcept
{
    fprintf(stderr, "#%-2u 0x%016" PRIxPTR " sp=0x%016" PRIxPTR " %s - %s + 0x%" PRIxPTR "\n", depth, ip, sp, library_name, func_name, off);
#else
static std::pair<bool, bool> printFrameInfo(unsigned int depth, uintptr_t, uintptr_t, const char*, const char* func_name, uintptr_t)
{
    // Only function name is known, nothing else.
    fprintf(stderr, "#%-2u %s\n", depth, func_name);
#endif
    return std::make_pair(false, false);
}

__attribute__((noinline)) static std::pair<bool, bool> traverseStack(BacktraceCallback callback) noexcept
{
#if defined(WITH_LIBUNWIND)
    unw_cursor_t cursor;
    unw_context_t context;
    Dl_info dl_info;

    unw_getcontext(&context);
    unw_init_local(&cursor, &context);

    for (unsigned int i = 0; unw_step(&cursor) > 0; ++i) {
        if (i < 1) {
            // Skip the topmost frame.
            continue;
        }

        if (g_verbose_mode == VERBOSE_NO && i >= MAX_STACK_DEPTH) {
            // No need to go deeper.
            break;
        }

        unw_word_t ip{};
        unw_word_t sp{};
        unw_word_t off;

        if (g_verbose_mode != VERBOSE_NO) {
            // These are needed to be populated only in verbose modes.
            unw_get_reg(&cursor, UNW_REG_IP, &ip);
            unw_get_reg(&cursor, UNW_REG_SP, &sp);
        }

        char symbol[256] = { "???" };
        char* name;

        int status = -UNW_EUNSPEC;
        while (status == -UNW_EUNSPEC) {
            // unw_get_proc_name may spontaneously report an error in multi-threaded environments.
            // If this kludge is removed the MultipleThreadsShortTermPause test randomly fails.
            status = unw_get_proc_name(&cursor, symbol, sizeof(symbol), &off);
        }
        if (status == 0) {
            name = abi::__cxa_demangle(symbol, nullptr, nullptr, &status);
            if (status != 0) {
                name = symbol;
            }
        }
        else {
            return std::make_pair(true, true); // Real OOM.
        }

        const char* file_name = "???";

        if (g_verbose_mode != VERBOSE_NO && dladdr(reinterpret_cast<void*>(ip + off), &dl_info) && dl_info.dli_fname && *dl_info.dli_fname) {
            file_name = dl_info.dli_fname;
        }

        const auto check_status = callback(i, static_cast<uintptr_t>(ip), static_cast<uintptr_t>(sp), file_name, name, static_cast<uintptr_t>(off));

        if (name != symbol)
            free(name);

        if (check_status.first || check_status.second)
            return check_status;
    }

    return std::make_pair(false, false);
#else
    void* callstack[MAX_STACK_DEPTH_VERBOSE];
    const int count = backtrace(callstack, callback == printFrameInfo ? MAX_STACK_DEPTH_VERBOSE : MAX_STACK_DEPTH);
    char** symbols = backtrace_symbols(callstack, count);

    if (!symbols) {
        // Real OOM
        return std::make_pair(true, true);
    }

    for (unsigned int depth = 1; depth < count; ++depth) {
        const auto check_status = callback(depth, 0U, 0U, nullptr, symbols[depth], 0U);
        if (check_status.first || check_status.second) {
            free(symbols);
            return check_status;
        }
    }

    free(symbols);

    return std::make_pair(false, false);
#endif
}

#if defined(PLATFORM_OS_MAC_OS_X)
__attribute__((noinline)) static std::pair<bool, bool> checker(unsigned int depth, uintptr_t, uintptr_t, const char*, const char* func_name, uintptr_t) noexcept
{
    if ((depth == 3 || depth == 4) && strstr(func_name, "__cxa_allocate_exception")) {
        // This branch is reachable with macOS 10.14 / Xcode 10 and older.
        // In newer environments some other mechanism seems to be used for allocating exception objects.
        return std::make_pair(true, false);
    }

    // __cxa_atexit is not supposed to be used explicitly but overthrower needs to be aware of existence of this function:
    // Allocations which come from __cxa_atexit shall not be failed by overthrower.
    // Memory which is allocated by __cxa_atexit shall not be treated as memory leak.
    if ((depth == 3 || depth == 4) && strstr(func_name, "__cxa_atexit")) {
        return std::make_pair(true, true);
    }

    return std::make_pair(false, false);
}
#elif defined(PLATFORM_OS_LINUX)
static std::pair<bool, bool> checker(unsigned int depth, uintptr_t, uintptr_t, const char*, const char* func_name, uintptr_t) noexcept
{
    static const auto compareFuncName = [](const char* a, const char* b) {
#if defined(WITH_LIBUNWIND)
        return strcmp(a, b) == 0;
#else
        return strstr(a, b) != nullptr;
#endif
    };

    if ((depth == 2 || depth == 3) && (compareFuncName(func_name, "__cxa_allocate_exception"))) {
        return std::make_pair(true, false);
    }
    if (compareFuncName(func_name, "_dl_map_object") || compareFuncName(func_name, "_dl_map_object_deps")) {
        // These two functions tend to leak, especially in OOM conditions.
        // https://sourceware.org/bugzilla/show_bug.cgi?id=2451
        // https://sourceware.org/legacy-ml/libc-alpha/2013-09/msg00150.html
        return std::make_pair(false, true);
    }
    if (depth == 5 && compareFuncName(func_name, "_dl_catch_exception")) {
        return std::make_pair(false, true);
    }
    if (depth == 2 && (compareFuncName(func_name, "_dl_signal_error") || compareFuncName(func_name, "_dl_exception_create"))) {
        return std::make_pair(true, true);
    }
    if ((depth == 4 || depth == 5) && compareFuncName(func_name, "dlerror")) {
        return std::make_pair(false, true);
    }
    if (compareFuncName(func_name, "__libpthread_freeres")) {
        // https://patches-gcc.linaro.org/patch/6525/
        return std::make_pair(false, true);
    }

    return std::make_pair(false, false);
}
#endif

// overthrower internal logic requires call stack depth to be deterministic,
// function "checker" (which is implemented above) will not work correctly if this requirement is not satisfied.
// Compilers may decide that the function "searchKnowledgeBase" is way too simple and inline it,
// this will break expectations and prevent overthrower from working correctly.
// In order to prevent compilers from inlining "searchKnowledgeBase" we use "__attribute__((noinline))".
__attribute__((noinline)) static void searchKnowledgeBase(bool& is_in_white_list, bool& is_in_ignore_list) noexcept
{
    const auto check_result = traverseStack(checker);
    is_in_white_list = check_result.first;
    is_in_ignore_list = check_result.second;
}

#if defined(PLATFORM_OS_LINUX)
__attribute__((visibility("default")))
#endif
void* my_malloc(size_t size) noexcept
{
#if defined(PLATFORM_OS_MAC_OS_X)
    if (g_initializing)
        return nonFailingMalloc(size);
#endif

    if (!g_initialized)
        initialize();

    if (!g_activated)
        return nonFailingMalloc(size);

    const unsigned int depth = g_state.depth;
    assert(depth <= MAX_PAUSE_DEPTH);

    bool is_in_white_list = g_state.is_tracing;
    bool is_in_ignore_list = false;

    if (!g_state.is_tracing) {
        g_state.is_tracing = true;
        const unsigned int old_paused = g_state.paused[depth];
        g_state.paused[depth] = UINT_MAX;
        searchKnowledgeBase(is_in_white_list, is_in_ignore_list);
        g_state.paused[depth] = old_paused;
        g_state.is_tracing = false;
    }

    if (g_state.paused[depth]) {
        --g_state.paused[depth];
        return nonFailingMalloc(size);
    }

    const unsigned int malloc_seq_num = g_malloc_counter++;

    if (is_in_white_list || !size)
        return nonFailingMalloc(size);

    if (isTimeToFail(malloc_seq_num)) {
        if (g_verbose_mode == VERBOSE_FAILED_ALLOCATIONS) {
            g_state.is_tracing = true;
            const unsigned int old_paused = g_state.paused[depth];
            g_state.paused[depth] = UINT_MAX;
            fprintf(stderr, "\n### Failed allocation, sequential number: %u ###\n", malloc_seq_num);
            traverseStack(printFrameInfo);
            g_state.paused[depth] = old_paused;
            g_state.is_tracing = false;
        }
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = nonFailingMalloc(size);

    if (!pointer) {
        return nullptr; // Real OOM
    }

    // is_in_ignore_list is never true alone on macOS that is why we do not use return ASAP approach at this place.
    if (!is_in_ignore_list) {
        // Register all allocations which are not in the ignore list.
        // All registered and not freed memory blocks are considered to be memory leaks.
        try {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            g_allocated.emplace(pointer, Info{ malloc_seq_num, size });
        }
        catch (const std::bad_alloc&) {
            // Real OOM
            nonFailingFree(pointer);
            errno = ENOMEM;
            return nullptr;
        }
        if (g_verbose_mode >= VERBOSE_FAILED_ALLOCATIONS) {
            g_state.is_tracing = true;
            const unsigned int old_paused = g_state.paused[depth];
            g_state.paused[depth] = UINT_MAX;
            fprintf(stderr, "\n### Successful allocation, sequential number: %u ###\n", malloc_seq_num);
            traverseStack(printFrameInfo);
            g_state.paused[depth] = old_paused;
            g_state.is_tracing = false;
        }
    }

    return pointer;
}

#if defined(PLATFORM_OS_LINUX)
__attribute__((visibility("default")))
#endif
void my_free(void* pointer) noexcept
{
    if (!pointer) {
        // Standard `free` is supposed to do nothing when `NULL` is tried to be freed.
        // Invoking `native_free(NULL)` at this point may be potentially disastrous, because `native_free` can be `NULL`.
        // `native_free` on Linux is initialized only on first `malloc` invocation.
        // If `free(NULL)` is invoked before any allocations are done using `malloc`, `native_free` is still pointing to `NULL` and can not be invoked.
        return;
    }

    if (g_activated) {
        const int old_errno = errno;
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_allocated.erase(pointer);
        errno = old_errno;
    }

    native_free(pointer);
}

#if defined(PLATFORM_OS_LINUX)
__attribute__((visibility("default")))
#endif
void* my_realloc(void* pointer, size_t size) noexcept
{
    if (!pointer)
        return my_malloc(size);

    if (!size) {
        my_free(pointer);
        return nullptr;
    }

    if (!g_allocated.count(pointer))
        return native_realloc(pointer, size);

    const size_t old_size = g_allocated.at(pointer).size;
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
