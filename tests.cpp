#include <atomic>
#include <mutex>
#include <numeric>
#include <thread>

#include "platform.h"

#if defined(PLATFORM_OS_LINUX)
#include <dlfcn.h>
#endif

#include <gtest/gtest.h>

#include "overthrower.h"
#include "thread_local.h"

#define STRATEGY_RANDOM 0U
#define STRATEGY_STEP 1U
#define STRATEGY_PULSE 2U
#define STRATEGY_NONE 3U

#define VERBOSE_NO 0U
#define VERBOSE_FAILED_ALLOCATIONS 1U
#define VERBOSE_ALL_ALLOCATIONS 2U

static void* (*volatile forced_memset)(void*, int, size_t) = memset;

GTEST_API_ int main(int argc, char** argv)
{
    if (!activateOverthrower || !deactivateOverthrower || !pauseOverthrower || !resumeOverthrower) {
        fprintf(stderr, "Seems like overthrower has not been injected or not fully available. Nothing to do.\n");
        return EXIT_FAILURE;
    }

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

class AbstractOverthrowerConfigurator {
protected:
    AbstractOverthrowerConfigurator() = default;

public:
    virtual ~AbstractOverthrowerConfigurator();

    static void enableSelfOverthrowMode() { ASSERT_EQ(setenv("OVERTHROWER_SELF_OVERTHROW", "", 0), 0); }
    static void setVerboseMode(unsigned int mode) { ASSERT_EQ(setenv("OVERTHROWER_VERBOSE", std::to_string(mode).c_str(), 0), 0); }

    static void setEnv(const char* name, const char* value) { ASSERT_EQ(setenv(name, value, 1), 0); }
    static void setEnv(const char* name, unsigned int value) { ASSERT_EQ(setenv(name, std::to_string(value).c_str(), 1), 0); }
    static void unsetEnv(const char* name) { ASSERT_EQ(unsetenv(name), 0); }
};

AbstractOverthrowerConfigurator::~AbstractOverthrowerConfigurator()
{
    for (const char* name : { "OVERTHROWER_STRATEGY",
                              "OVERTHROWER_SEED",
                              "OVERTHROWER_DUTY_CYCLE",
                              "OVERTHROWER_DELAY",
                              "OVERTHROWER_DURATION",
                              "OVERTHROWER_SELF_OVERTHROW",
                              "OVERTHROWER_VERBOSE" }) {
        unsetEnv(name);
    }
}

class OverthrowerConfiguratorRandom : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorRandom()
        : OverthrowerConfiguratorRandom(1024)
    {
    }

    explicit OverthrowerConfiguratorRandom(unsigned int duty_cycle)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_RANDOM);
        setEnv("OVERTHROWER_SEED", 0U);
        setEnv("OVERTHROWER_DUTY_CYCLE", duty_cycle);
    }
};

class OverthrowerConfiguratorStep : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorStep() = delete;
    explicit OverthrowerConfiguratorStep(unsigned int delay)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_STEP);
        setEnv("OVERTHROWER_DELAY", delay);
    }
};

class OverthrowerConfiguratorPulse : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorPulse() = delete;
    OverthrowerConfiguratorPulse(unsigned int delay, unsigned int duration)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_PULSE);
        setEnv("OVERTHROWER_DELAY", delay);
        setEnv("OVERTHROWER_DURATION", duration);
    }
};

class OverthrowerConfiguratorNone : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorNone() { setEnv("OVERTHROWER_STRATEGY", STRATEGY_NONE); }
};

class OverthrowerRandomParameters : public AbstractOverthrowerConfigurator {
public:
    OverthrowerRandomParameters()
    {
        for (const char* name : { "OVERTHROWER_STRATEGY", "OVERTHROWER_SEED", "OVERTHROWER_DUTY_CYCLE", "OVERTHROWER_DELAY", "OVERTHROWER_DURATION" }) {
            setParameterToInvalidValue(name);
        }
    }

protected:
    static void setParameterToInvalidValue(const char* name)
    {
        if ((rand() % 4) != 0) {
            return;
        }
        if ((rand() % 2) == 0) {
            setEnv(name, "123456789012345678901234567890"); // Enormously big value which will never fit into integer value.
        }
        else {
            setEnv(name, "not_a_number"); // Invalid value which can not be converted to integer at all.
        }
    }
};

static void fragileCode(unsigned int iterations = 1024)
{
    for (unsigned int i = 0; i < iterations; ++i) {
        char* string = strdup("string");
        forced_memset(string, 0, 6);
        free(string);
    }
}

static void fragileCodeWithOverthrower()
{
    activateOverthrower();
    fragileCode();
    deactivateOverthrower();
}

static unsigned int failureCounter(unsigned int iterations,
                                   std::string& pattern,
                                   std::atomic<unsigned int>* malloc_seq_num = nullptr,
                                   std::mutex* mutex = nullptr)
{
    unsigned int counter = 0;
    for (unsigned int i = 0; i < iterations; ++i) {
        if (mutex)
            mutex->lock();
        char* string = strdup("string");
        if (string)
            forced_memset(string, 0, 6);
        else
            ++counter;
        if (!malloc_seq_num)
            pattern.push_back(string ? '+' : '-');
        else
            pattern[(*malloc_seq_num)++] = string ? '+' : '-';
        if (mutex)
            mutex->unlock();
        free(string);
    }
    return counter;
}

static std::string generateExpectedPattern(unsigned int strategy, unsigned int iterations, unsigned int delay = 0, unsigned int duration = 1)
{
    std::string pattern;
    if (strategy == STRATEGY_STEP) {
        for (unsigned int i = 0; i < delay && pattern.size() < iterations; ++i)
            pattern.push_back('+');
        while (pattern.size() < iterations)
            pattern.push_back('-');
    }
    else if (strategy == STRATEGY_PULSE) {
        for (unsigned int i = 0; i < delay && pattern.size() < iterations; ++i)
            pattern.push_back('+');
        for (unsigned int i = 0; i < duration && pattern.size() < iterations; ++i)
            pattern.push_back('-');
        while (pattern.size() < iterations)
            pattern.push_back('+');
    }
    return pattern;
}

TEST(ThreadLocal, Boolean)
{
    static constexpr unsigned int thread_count = 128;

    std::thread threads[thread_count];
    ThreadLocal<bool> thread_local_bool;

    auto threadRoutine = [&thread_local_bool]() {
        EXPECT_FALSE(thread_local_bool);
        EXPECT_EQ(thread_local_bool, false);
        for (bool value : { true, false, true }) {
            thread_local_bool = value;
            if (value)
                EXPECT_TRUE(thread_local_bool);
            else
                EXPECT_FALSE(thread_local_bool);
            EXPECT_EQ(thread_local_bool, value);
        }
    };

    threadRoutine();

    for (auto& thread : threads)
        thread = std::thread(threadRoutine);

    for (auto& thread : threads)
        thread.join();
}

TEST(FragileCode, WithoutOverthrower)
{
    fragileCode();
}

TEST(FragileCode, WithOverthrower)
{
    OverthrowerConfiguratorRandom overthrower_configurator;
    EXPECT_DEATH(fragileCodeWithOverthrower(), "");
}

TEST(Overthrower, MemoryLeak)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    for (unsigned int block_count : { 1, 2, 3 }) {
        void* buffers[3];
        activateOverthrower();
        for (unsigned int i = 0; i < block_count; ++i) {
            buffers[i] = malloc(128);
            forced_memset(buffers[i], 0, 128);
        }
        EXPECT_EQ(deactivateOverthrower(), block_count);
        for (unsigned int i = 0; i < block_count; ++i)
            free(buffers[i]);
    }
}

TEST(Overthrower, DoubleActivation)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    activateOverthrower();
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);

    activateOverthrower();
    buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    EXPECT_EQ(deactivateOverthrower(), 1);
    free(buffer);
}

TEST(Overthrower, DoubleDeactivation)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);
    EXPECT_EQ(deactivateOverthrower(), 0);

    activateOverthrower();
    buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    EXPECT_EQ(deactivateOverthrower(), 1);
    free(buffer);
}

TEST(Overthrower, Deactivation)
{
    OverthrowerConfiguratorStep overthrower_configurator(0);
    activateOverthrower();
    pauseOverthrower(0);
    fragileCode();
    EXPECT_EQ(deactivateOverthrower(), 0);
    activateOverthrower();
    void* buffer = malloc(128);
    EXPECT_EQ(deactivateOverthrower(), 0);
    EXPECT_EQ(buffer, nullptr);
}

TEST(Overthrower, FreePreAllocated)
{
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, LongTermPause)
{
    OverthrowerConfiguratorRandom overthrower_configurator;
    activateOverthrower();
    pauseOverthrower(0);
    fragileCode();
    resumeOverthrower();
    EXPECT_EQ(deactivateOverthrower(), 0);
}

static void validateShortPauseCorrectness()
{
    static constexpr unsigned int duration_variants[] = { 1, 2, 3, 5 };
    static constexpr unsigned int iterations = 10;

    for (unsigned int duration : duration_variants) {
        pauseOverthrower(0);
        std::string expected_pattern = generateExpectedPattern(STRATEGY_STEP, iterations, duration);
        std::string real_pattern(iterations, '?');
        resumeOverthrower();
        real_pattern.resize(0);
        pauseOverthrower(duration);
        const unsigned int real_failure_count = failureCounter(iterations, real_pattern);
        resumeOverthrower();
        pauseOverthrower(0);
        EXPECT_EQ(real_failure_count, iterations - duration);
        EXPECT_EQ(real_pattern, expected_pattern);
        resumeOverthrower();
    }
}

TEST(Overthrower, SingleThreadShortTermPause)
{
    OverthrowerConfiguratorStep overthrower_configurator(0);
    activateOverthrower();
    validateShortPauseCorrectness();
    EXPECT_EQ(deactivateOverthrower(), 0);
}

#if defined(PLATFORM_OS_LINUX) || (defined(PLATFORM_OS_MAC_OS_X) && __apple_build_version__ >= 9000037)
// With Earlier Xcode versions std::thread constructor crashes instead of throwing an exception in OOM conditions
TEST(Overthrower, MultipleThreadsShortTermPause)
{
    static constexpr unsigned int thread_count = 128;

    OverthrowerConfiguratorStep overthrower_configurator(0);
    activateOverthrower();
    pauseOverthrower(0);

    std::thread threads[thread_count];

    for (auto& thread : threads)
        thread = std::thread(validateShortPauseCorrectness);

    for (auto& thread : threads)
        thread.join();

    resumeOverthrower();
    EXPECT_EQ(deactivateOverthrower(), 0);
}
#endif

TEST(Overthrower, NestedPause)
{
    static constexpr unsigned int max_recursive_depth = 4;
    static constexpr unsigned int max_depth = 16;

    OverthrowerConfiguratorStep overthrower_configurator(0);

    std::function<void(unsigned int)> recursive_function;
    recursive_function = [&recursive_function](unsigned int depth) {
        pauseOverthrower(1);
        fragileCode(1);
        if (depth < max_recursive_depth - 1)
            recursive_function(depth + 1);
        resumeOverthrower();
        pauseOverthrower(2);
        fragileCode(1);
        if (depth < max_recursive_depth - 1)
            recursive_function(depth + 1);
        fragileCode(1);
        void* buffer = malloc(128);
        resumeOverthrower();
        pauseOverthrower(0);
        ASSERT_EQ(buffer, nullptr);
        resumeOverthrower();
    };

    activateOverthrower();
    recursive_function(0);

    for (unsigned int i = 0; i < max_depth; ++i) {
        pauseOverthrower(1);
    }

    for (unsigned int i = 0; i < max_depth; ++i) {
        fragileCode(1);
        resumeOverthrower();
    }

    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, NestedPauseOverflowUnderflow)
{
    static constexpr unsigned int max_depth = 128;

    OverthrowerConfiguratorStep overthrower_configurator(0);
    activateOverthrower();

    for (unsigned int i = 0; i < max_depth; ++i) {
        pauseOverthrower(1);
        fragileCode(1);
    }

    pauseOverthrower(0);
    fragileCode(1);
    pauseOverthrower(1);
    fragileCode(1);
    void* buffer1 = malloc(128);
    resumeOverthrower();
    void* buffer2 = malloc(128);
    resumeOverthrower();

    for (unsigned int i = 0; i < max_depth * 2; ++i) {
        resumeOverthrower();
    }

    pauseOverthrower(1);
    fragileCode(1);
    resumeOverthrower();

    EXPECT_EQ(deactivateOverthrower(), 0);
    EXPECT_EQ(buffer1, nullptr);
    EXPECT_EQ(buffer2, nullptr);
}

TEST(Overthrower, PauseNotActivated)
{
    for (unsigned int i = 0U; i < 32U; ++i) {
        pauseOverthrower(1U);
    }

    std::thread thread = std::thread([]() {
        for (unsigned int i = 0U; i < 32U; ++i) {
            pauseOverthrower(1U);
        }
    });

    fragileCode();

    thread.join();
}

TEST(Overthrower, RandomParameters)
{
    constexpr unsigned int iteration_count = 128U;
    constexpr unsigned int allocation_count = 1024U;
    unsigned int strategy_random_chosen_times = 0U;
    unsigned int strategy_random_step_times = 0U;
    unsigned int strategy_random_pulse_times = 0U;

    for (unsigned int i = 0; i < iteration_count; ++i) {
        std::string real_pattern;
        real_pattern.reserve(allocation_count);

        OverthrowerRandomParameters overthrower_configurator;

        activateOverthrower();
        failureCounter(allocation_count, real_pattern);
        EXPECT_EQ(deactivateOverthrower(), 0U);

        std::adjacent_difference(real_pattern.cbegin(), real_pattern.cend(), real_pattern.begin(), std::not_equal_to<char>());
        const unsigned int switch_count = std::accumulate(real_pattern.cbegin() + 1U, real_pattern.cend(), 0U);

        // Note: switch_count can be 0 under the following conditions:
        // strategy = step
        // delay = 0

        if (switch_count == 1U) {
            ++strategy_random_step_times;
        }
        else if (switch_count == 2U) {
            ++strategy_random_pulse_times;
        }
        else if (switch_count > 2U) {
            ++strategy_random_chosen_times;
        }
        if (strategy_random_chosen_times >= 4U && strategy_random_step_times >= 4U && strategy_random_pulse_times >= 4U)
            break;
    }

    EXPECT_GT(strategy_random_chosen_times, 0U);
    EXPECT_GT(strategy_random_step_times, 0U);
    EXPECT_GT(strategy_random_pulse_times, 0U);
}

TEST(Overthrower, StrategyRandom)
{
#if defined(PLATFORM_OS_LINUX) || (defined(PLATFORM_OS_MAC_OS_X) && __apple_build_version__ >= 9000037)
    static constexpr unsigned int thread_count_variants[] = { 1, 2, 8 };
#else
    static constexpr unsigned int thread_count_variants[] = { 1 };
#endif
    static constexpr unsigned int duty_cycle_variants[] = { 1, 2, 3, 5, 10, 20, 30 };
    static constexpr unsigned int expected_failure_count = 1024;

    for (unsigned int thread_count : thread_count_variants) {
        for (unsigned int duty_cycle : duty_cycle_variants) {
            const unsigned int iterations = duty_cycle * expected_failure_count;
            const unsigned int allowed_delta = duty_cycle == 1 ? 0 : expected_failure_count / 10;

            std::string real_pattern(iterations, '?');
            std::atomic<unsigned int> real_failure_count{};
            std::atomic<unsigned int> malloc_seq_num{};
            std::vector<std::thread> threads;
            volatile bool start_flag = false;

            OverthrowerConfiguratorRandom overthrower_configurator(duty_cycle);

            if (thread_count == 1) {
                activateOverthrower();
                real_failure_count = failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num);
            }
            else {
                for (unsigned int i = 0; i < thread_count; ++i) {
                    threads.emplace_back(std::thread([&]() {
                        while (!start_flag) {
                        }
                        real_failure_count += failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num);
                    }));
                }
                activateOverthrower();
                start_flag = true;
            }

            for (auto& thread : threads)
                thread.join();
            threads.clear();

            EXPECT_EQ(deactivateOverthrower(), 0);
            EXPECT_GE(real_failure_count, expected_failure_count - allowed_delta);
            EXPECT_LE(real_failure_count, expected_failure_count + allowed_delta);
            if (duty_cycle == 1)
                continue;
            std::adjacent_difference(real_pattern.cbegin(), real_pattern.cend(), real_pattern.begin(), std::not_equal_to<char>());
            const unsigned int switch_count = std::accumulate(real_pattern.cbegin() + 1U, real_pattern.cend(), 0U);
            EXPECT_GE(switch_count, expected_failure_count * 9 / 10);
            EXPECT_LE(switch_count, expected_failure_count * 11 / 5);
        }
    }
}

TEST(Overthrower, StrategyStep)
{
#if defined(PLATFORM_OS_LINUX) || (defined(PLATFORM_OS_MAC_OS_X) && __apple_build_version__ >= 9000037)
    static constexpr unsigned int thread_count_variants[] = { 1, 2, 8 };
#else
    static constexpr unsigned int thread_count_variants[] = { 1 };
#endif
    static constexpr unsigned int delay_variants[] = { 0, 1, 2, 3, 5 };
    static constexpr unsigned int iterations = 64;

    std::mutex mutex;

    for (bool with_mutex : { true, false }) {
        for (unsigned int thread_count : thread_count_variants) {
            if (thread_count == 1)
                with_mutex = false;
            for (unsigned int delay : delay_variants) {
                const std::string expected_pattern = generateExpectedPattern(STRATEGY_STEP, iterations, delay);

                std::string real_pattern(iterations, '?');
                std::atomic<unsigned int> real_failure_count{};
                std::atomic<unsigned int> malloc_seq_num{};
                std::vector<std::thread> threads;
                volatile bool start_flag = false;

                OverthrowerConfiguratorStep overthrower_configurator(delay);

                if (thread_count == 1) {
                    activateOverthrower();
                    real_failure_count = failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num, with_mutex ? &mutex : nullptr);
                }
                else {
                    for (unsigned int i = 0; i < thread_count; ++i) {
                        threads.emplace_back(std::thread([&]() {
                            while (!start_flag) {
                            }
                            real_failure_count += failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num, with_mutex ? &mutex : nullptr);
                        }));
                    }
                    activateOverthrower();
                    start_flag = true;
                }

                for (auto& thread : threads)
                    thread.join();
                threads.clear();

                EXPECT_EQ(deactivateOverthrower(), 0);
                EXPECT_EQ(real_failure_count, iterations - delay);
                if (with_mutex)
                    EXPECT_EQ(real_pattern, expected_pattern);
            }
        }
    }
}

TEST(Overthrower, StrategyPulse)
{
#if defined(PLATFORM_OS_LINUX) || (defined(PLATFORM_OS_MAC_OS_X) && __apple_build_version__ >= 9000037)
    static constexpr unsigned int thread_count_variants[] = { 1, 2, 8 };
#else
    static constexpr unsigned int thread_count_variants[] = { 1 };
#endif
    static constexpr unsigned int delay_variants[] = { 1, 2, 3, 5 };
    static constexpr unsigned int duration_variants[] = { 1, 2, 3, 5 };
    static constexpr unsigned int iterations = 64;

    std::mutex mutex;

    for (bool with_mutex : { true, false }) {
        for (unsigned int thread_count : thread_count_variants) {
            if (thread_count == 1)
                with_mutex = false;
            for (unsigned int delay : delay_variants) {
                for (unsigned int duration : duration_variants) {
                    const std::string expected_pattern = generateExpectedPattern(STRATEGY_PULSE, iterations, delay, duration);

                    std::string real_pattern(iterations, '?');
                    std::atomic<unsigned int> real_failure_count{};
                    std::atomic<unsigned int> malloc_seq_num{};
                    std::vector<std::thread> threads;
                    volatile bool start_flag = false;

                    OverthrowerConfiguratorPulse overthrower_configurator(delay, duration);

                    if (thread_count == 1) {
                        activateOverthrower();
                        real_failure_count = failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num, with_mutex ? &mutex : nullptr);
                    }
                    else {
                        for (unsigned int i = 0; i < thread_count; ++i) {
                            threads.emplace_back(std::thread([&]() {
                                while (!start_flag) {
                                }
                                real_failure_count += failureCounter(iterations / thread_count, real_pattern, &malloc_seq_num, with_mutex ? &mutex : nullptr);
                            }));
                        }
                        activateOverthrower();
                        start_flag = true;
                    }

                    for (auto& thread : threads)
                        thread.join();
                    threads.clear();

                    EXPECT_EQ(deactivateOverthrower(), 0);
                    EXPECT_EQ(real_failure_count, duration);
                    if (with_mutex)
                        EXPECT_EQ(real_pattern, expected_pattern);
                }
            }
        }
    }
}

TEST(Overthrower, StrategyNone)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    fragileCode();
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, SettingErrno)
{
    static const unsigned int iterations = 50;
    unsigned int failure_count = 0;

    OverthrowerConfiguratorRandom overthrower_configurator(2);
    activateOverthrower();

    for (unsigned int i = 0; i < iterations; ++i) {
        errno = 0;
        void* buffer = malloc(128);
        pauseOverthrower(0);
        if (buffer) {
            forced_memset(buffer, 0, 128);
            EXPECT_EQ(errno, 0);
        }
        else {
            ++failure_count;
            EXPECT_EQ(errno, ENOMEM);
        }
        resumeOverthrower();
        const int old_errno = errno;
        free(buffer);
        pauseOverthrower(0);
        EXPECT_EQ(errno, old_errno);
        resumeOverthrower();
    }

    EXPECT_EQ(deactivateOverthrower(), 0);

    EXPECT_GE(failure_count, iterations / 4);
}

TEST(Overthrower, PreservingErrnoWithoutOverthrower)
{
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    errno = 100500;
    free(buffer);
    EXPECT_EQ(errno, 100500);
}

TEST(Overthrower, PreservingErrnoWithOverthrower)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    errno = 100500;
    free(buffer);
    EXPECT_EQ(errno, 100500);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, ThrowingException)
{
    class CustomException : public std::exception {
    public:
        CustomException() = default;

        char placeholder[262144]{};
    };

    std::function<unsigned int(unsigned int, unsigned int)> recursiveFunction;
    recursiveFunction = [&recursiveFunction](unsigned int value, unsigned int depth) {
        if (depth == 32U) {
            throw CustomException();
        }

        if (value == 0U) {
            return 0U;
        }
        else {
            return value + recursiveFunction(value - 1U, depth + 1U);
        }
    };

    static const unsigned int iterations = 5000;
    unsigned int failure_count = 0;

    ASSERT_EQ(recursiveFunction(3U, 0U), 6U);

    OverthrowerConfiguratorRandom overthrower_configurator(2);
    activateOverthrower();

    for (unsigned int i = 0; i < iterations; ++i) {
        try {
            const unsigned int result = recursiveFunction(32U, 0U);
            ASSERT_EQ(result, 528U);
        }
        catch (const CustomException&) {
            ++failure_count;
        }
    }

    EXPECT_EQ(deactivateOverthrower(), 0);

    EXPECT_GE(failure_count, iterations / 4);
}

#if defined(PLATFORM_OS_LINUX) || (defined(PLATFORM_OS_MAC_OS_X) && __apple_build_version__ >= 9000037)
// With Earlier Xcode versions std::thread constructor crashes instead of throwing an exception in OOM conditions
TEST(Overthrower, CreatingThreads)
{
    static const unsigned int duty_cycle_variants[] = { 1, 2, 3, 5, 10, 20, 30, 50, 100 };
    static constexpr unsigned int thread_count = 128;

    std::atomic<unsigned int> success_count{};
    unsigned int failure_count = 0;

    std::thread threads[thread_count];

    auto thread_routine = [&success_count]() { ++success_count; };

    for (unsigned int duty_cycle : duty_cycle_variants) {
        OverthrowerConfiguratorRandom overthrower_configurator(duty_cycle);
        activateOverthrower();

        for (auto& thread : threads) {
            try {
                thread = std::thread(thread_routine);
            }
            catch (const std::bad_alloc&) {
                ++failure_count;
            }
        }

        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }

        pauseOverthrower(0);
        if (duty_cycle == 1) {
            EXPECT_EQ(success_count, 0);
            EXPECT_EQ(failure_count, thread_count);
        }
        resumeOverthrower();

        EXPECT_EQ(deactivateOverthrower(), 0);
    }

    EXPECT_GT(success_count, 0);
    EXPECT_GT(failure_count, 0);
    EXPECT_GT(failure_count, thread_count);
    EXPECT_EQ(success_count + failure_count, thread_count * (sizeof(duty_cycle_variants) / sizeof(duty_cycle_variants[0])));
}
#endif

TEST(Overthrower, ReallocNonFailing)
{
    static const size_t sizes[] = { 2,     4,     8,      16,     32,     64,      128,     256,     512,     1024,     2048,     4096,     8192,     16384,
                                    32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216, 33554432, 67108864, 134217728 };
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    void* buffer = malloc(1);
    forced_memset(buffer, 0, 1);
    ASSERT_NE(buffer, nullptr);
    for (size_t size : sizes) {
        buffer = realloc(buffer, size);
        ASSERT_NE(buffer, nullptr);
        forced_memset(buffer, 0, size);
    }
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);

    buffer = malloc(128);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 128);
    activateOverthrower();
    buffer = realloc(buffer, 256);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 256);
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, ReallocFailing)
{
    static const size_t sizes[] = { 2,     4,     8,      16,     32,     64,      128,     256,     512,     1024,     2048,     4096,     8192,     16384,
                                    32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216, 33554432, 67108864, 134217728 };
    OverthrowerConfiguratorRandom overthrower_configurator(2);
    activateOverthrower();
    void* buffer = nullptr;
    while (!buffer)
        buffer = malloc(1);
    forced_memset(buffer, 0, 1);
    for (size_t size : sizes) {
        void* new_buffer = realloc(buffer, size);
        if (!new_buffer) {
            EXPECT_EQ(errno, ENOMEM);
            continue;
        }
        buffer = new_buffer;
        forced_memset(buffer, 0, size);
    }
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, ReallocAllocate)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    void* buffer = realloc(nullptr, 128);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 128);
    buffer = realloc(buffer, 256);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 256);
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);

    buffer = realloc(nullptr, 128);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 128);
    activateOverthrower();
    buffer = realloc(buffer, 256);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 256);
    free(buffer);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, ReallocDeallocateWithoutOverthrower)
{
    void* buffer = realloc(nullptr, 128);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 128);
    buffer = realloc(buffer, 256);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 256);
    buffer = realloc(buffer, 0);
    ASSERT_EQ(buffer, nullptr);
}

TEST(Overthrower, ReallocDeallocateWithOverthrower)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();
    void* buffer = realloc(nullptr, 128);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 128);
    buffer = realloc(buffer, 256);
    ASSERT_NE(buffer, nullptr);
    forced_memset(buffer, 0, 256);
    buffer = realloc(buffer, 0);
    ASSERT_EQ(buffer, nullptr);
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, ReallocGrowShrink)
{
    constexpr unsigned int iteration_count = 128;
    constexpr size_t min_size = 128;
    constexpr size_t max_size = 1024;

    size_t prev_size = min_size + (rand() % (max_size - min_size + 1));

    std::vector<uint8_t> data(max_size);
    std::generate_n(data.begin(), prev_size, []() { return rand() % 256; });

    OverthrowerConfiguratorRandom overthrower_configurator(2);
    activateOverthrower();

    pauseOverthrower(1);
    void* buffer = malloc(prev_size);
    resumeOverthrower();
    memcpy(buffer, &data[0], prev_size);

    for (unsigned int i = 0; i < iteration_count; ++i) {
        const size_t new_size = min_size + (rand() % (max_size - min_size + 1));
        void* new_buffer = realloc(buffer, new_size);
        if (!new_buffer) {
            pauseOverthrower(0);
            EXPECT_EQ(memcmp(buffer, &data[0], prev_size), 0);
            resumeOverthrower();
            continue;
        }

        pauseOverthrower(0);
        EXPECT_EQ(memcmp(new_buffer, &data[0], std::min(prev_size, new_size)), 0);
        resumeOverthrower();

        std::generate_n(data.begin(), new_size, []() { return rand() % 256; });
        memcpy(new_buffer, &data[0], new_size);

        prev_size = new_size;
        buffer = new_buffer;
    }

    ASSERT_NE(buffer, nullptr);
    free(buffer);

    EXPECT_EQ(deactivateOverthrower(), 0);
}

extern "C" void* somePureCFunction();

TEST(Overthrower, PureC)
{
    OverthrowerConfiguratorStep overthrower_configurator(0);
    ASSERT_EQ(somePureCFunction(), nullptr);
}

TEST(Overthrower, ImplicitDeactivation)
{
    auto subprocess = []() {
        OverthrowerConfiguratorNone overthrower_configurator;
        activateOverthrower();
        exit(1);
    };

    EXPECT_EXIT(subprocess(), ::testing::ExitedWithCode(1), "overthrower has not been deactivated explicitly, doing it anyway.");
}

extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* d); // NOLINT
extern void* __dso_handle;                                            // NOLINT

static void exitFunction(void*)
{
    static std::atomic<unsigned int> counter{};
    if (++counter == 64) {
        // This function is expected to be invoked exactly 64 times.
        // If it is not we are dealing with an error.
        fprintf(stderr, "Exiting ...\n");
    }
}

TEST(Overthrower, AtExit)
{
    auto subprocess = []() {
        OverthrowerConfiguratorStep overthrower_configurator(0);
        OverthrowerConfiguratorStep::setVerboseMode(2U);
        activateOverthrower();
        // __cxa_atexit start allocating after it is invoked dozens of times. Invoke this 64 times.
        for (int i = 0; i < 64; ++i) {
            __cxa_atexit(exitFunction, nullptr, __dso_handle);
        }
        exit(static_cast<int>(deactivateOverthrower()));
    };

    EXPECT_EXIT(subprocess(), ::testing::ExitedWithCode(0), "Exiting ...");
}

#if defined(PLATFORM_OS_LINUX)
TEST(Overthrower, DlError)
{
    OverthrowerConfiguratorNone overthrower_configurator;
    activateOverthrower();

    void* handle = dlopen("non_existing_library.so", RTLD_NOW);
    ASSERT_EQ(handle, nullptr);
    const char* error = dlerror();
    ASSERT_NE(error, nullptr);

    EXPECT_EQ(deactivateOverthrower(), 0U);
}
#endif

TEST(Overthrower, SelfOverthrow)
{
    constexpr unsigned int allocation_count = 16384U;

    std::string real_pattern;
    real_pattern.reserve(allocation_count);

    OverthrowerConfiguratorRandom overthrower_configurator(2U);
    OverthrowerConfiguratorRandom::enableSelfOverthrowMode();
    activateOverthrower();

    failureCounter(allocation_count, real_pattern);

    EXPECT_EQ(deactivateOverthrower(), 0U);

    const unsigned int failure_count = std::count(real_pattern.cbegin(), real_pattern.cend(), '-');

    std::adjacent_difference(real_pattern.cbegin(), real_pattern.cend(), real_pattern.begin(), std::not_equal_to<char>());
    const unsigned int switch_count = std::accumulate(real_pattern.cbegin() + 1U, real_pattern.cend(), 0U);

    EXPECT_GT(switch_count, allocation_count / 8U);
    EXPECT_GT(failure_count, allocation_count * 2U / 3U);
}

TEST(Overthrower, VerboseMode) {
    constexpr unsigned int allocation_count = 16U;

    for (bool enable_self_overthrow_mode : {false, true}) {
        for (unsigned int verbose_mode : { VERBOSE_NO, VERBOSE_FAILED_ALLOCATIONS, VERBOSE_ALL_ALLOCATIONS }) {
            std::string real_pattern;
            real_pattern.reserve(allocation_count);

            OverthrowerConfiguratorRandom overthrower_configurator(2U);
            if (enable_self_overthrow_mode) {
                OverthrowerConfiguratorRandom::enableSelfOverthrowMode();
            }
            OverthrowerConfiguratorRandom::setVerboseMode(verbose_mode);
            activateOverthrower();
            failureCounter(allocation_count, real_pattern);
            EXPECT_EQ(deactivateOverthrower(), 0U);
        }
    }
}
