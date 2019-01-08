#include <numeric>

#include <src/gmock-all.cc>
#include <src/gtest-all.cc>

#include "overthrower.h"
#include "platform.h"

#define STRATEGY_RANDOM 0
#define STRATEGY_STEP 1
#define STRATEGY_PULSE 2
#define STRATEGY_NONE 3

static void* (*volatile forced_memset)(void*, int, size_t) = memset;

GTEST_API_ int main(int argc, char** argv)
{
    if (!activateOverthrower || !deactivateOverthrower || !pauseOverthrower || !resumeOverthrower) {
        fprintf(stderr, "Seems like overthrower has not been injected or not fully available. Nothing to do.\n");
        return EXIT_FAILURE;
    }

    testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}

class AbstractOverthrowerConfigurator {
protected:
    AbstractOverthrowerConfigurator() = default;

public:
    virtual ~AbstractOverthrowerConfigurator();

    void setEnv(const char* name, unsigned int value) { ASSERT_EQ(setenv(name, std::to_string(value).c_str(), 1), 0); }
    void unsetEnv(const char* name) { ASSERT_EQ(unsetenv(name), 0); }
};

AbstractOverthrowerConfigurator::~AbstractOverthrowerConfigurator()
{
    unsetEnv("OVERTHROWER_STRATEGY");
    unsetEnv("OVERTHROWER_SEED");
    unsetEnv("OVERTHROWER_DUTY_CYCLE");
    unsetEnv("OVERTHROWER_DELAY");
    unsetEnv("OVERTHROWER_DURATION");
}

class OverthrowerConfiguratorRandom : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorRandom()
        : OverthrowerConfiguratorRandom(1024)
    {
    }

    OverthrowerConfiguratorRandom(unsigned int duty_cycle)
    {
        setEnv("OVERTHROWER_STRATEGY", STRATEGY_RANDOM);
        setEnv("OVERTHROWER_SEED", 0);
        setEnv("OVERTHROWER_DUTY_CYCLE", duty_cycle);
    }
};

class OverthrowerConfiguratorStep : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorStep() = delete;
    OverthrowerConfiguratorStep(unsigned int delay)
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

static void fragileCode(unsigned int iterations = 100500)
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

static unsigned int failureCounter(unsigned int iterations, std::string& pattern)
{
    unsigned int counter = 0;
    for (unsigned int i = 0; i < iterations; ++i) {
        char* string = strdup("string");
        if (string)
            forced_memset(string, 0, 6);
        else
            ++counter;
        pattern.push_back(string ? '+' : '-');
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
    activateOverthrower();
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    EXPECT_EQ(deactivateOverthrower(), 1);
    free(buffer);
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

TEST(Overthrower, ShortTermPause)
{
    static const unsigned int duration_variants[] = { 1, 2, 3, 5 };
    static const unsigned int iterations = 10;

    std::string expected_pattern;
    std::string real_pattern(iterations, '?');

    OverthrowerConfiguratorStep overthrower_configurator(0);
    activateOverthrower();
    for (unsigned int duration : duration_variants) {
        pauseOverthrower(0);
        expected_pattern = generateExpectedPattern(STRATEGY_STEP, iterations, duration);
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
    EXPECT_EQ(deactivateOverthrower(), 0);
}

TEST(Overthrower, StrategyRandom)
{
    static const unsigned int duty_cycle_variants[] = { 1, 2, 3, 5, 10, 20, 30, 50, 100 };
    static const unsigned int expected_failure_count = 1000;

    std::string real_pattern;

    for (unsigned int duty_cycle : duty_cycle_variants) {
        const unsigned int iterations = duty_cycle * expected_failure_count;
        real_pattern.reserve(iterations);
        real_pattern.resize(0);
        OverthrowerConfiguratorRandom overthrower_configurator(duty_cycle);
        activateOverthrower();
        const unsigned int allowed_delta = duty_cycle == 1 ? 0 : expected_failure_count / 10;
        const unsigned int real_failure_count = failureCounter(iterations, real_pattern);
        EXPECT_EQ(deactivateOverthrower(), 0);
        EXPECT_GE(real_failure_count, expected_failure_count - allowed_delta);
        EXPECT_LE(real_failure_count, expected_failure_count + allowed_delta);
        if (duty_cycle == 1)
            continue;
        std::adjacent_difference(real_pattern.cbegin(), real_pattern.cend(), real_pattern.begin(), std::greater<char>());
        const unsigned int switch_count = std::accumulate(real_pattern.cbegin() + 1, real_pattern.cend(), 0);
        EXPECT_GE(switch_count, expected_failure_count * 9 / 20);
    }
}

TEST(Overthrower, StrategyStep)
{
    static const unsigned int delay_variants[] = { 0, 1, 2, 3, 5 };
    static const unsigned int iterations = 50;

    std::string expected_pattern;
    std::string real_pattern(iterations, '?');

    for (unsigned int delay : delay_variants) {
        expected_pattern = generateExpectedPattern(STRATEGY_STEP, iterations, delay);
        real_pattern.resize(0);
        OverthrowerConfiguratorStep overthrower_configurator(delay);
        activateOverthrower();
        const unsigned int failure_count = failureCounter(iterations, real_pattern);
        EXPECT_EQ(deactivateOverthrower(), 0);
        EXPECT_EQ(failure_count, iterations - delay);
        EXPECT_EQ(real_pattern, expected_pattern);
    }
}

TEST(Overthrower, StrategyPulse)
{
    static const unsigned int delay_variants[] = { 1, 2, 3, 5 };
    static const unsigned int duration_variants[] = { 1, 2, 3, 5 };
    static const unsigned int iterations = 50;

    std::string expected_pattern;
    std::string real_pattern(iterations, '?');

    for (unsigned int delay : delay_variants) {
        for (unsigned int duration : duration_variants) {
            expected_pattern = generateExpectedPattern(STRATEGY_PULSE, iterations, delay, duration);
            real_pattern.resize(0);
            OverthrowerConfiguratorPulse overthrower_configurator(delay, duration);
            activateOverthrower();
            const unsigned int failure_count = failureCounter(iterations, real_pattern);
            EXPECT_EQ(deactivateOverthrower(), 0);
            EXPECT_EQ(failure_count, duration);
            EXPECT_EQ(real_pattern, expected_pattern);
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

#if defined(PLATFORM_OS_LINUX)
TEST(Overthrower, ThrowingException)
{
    class CustomException {
    public:
        CustomException() = default;

        char placeholder[16384];
    };

    static const unsigned int iterations = 5000;
    unsigned int failure_count = 0;

    OverthrowerConfiguratorRandom overthrower_configurator(2);
    activateOverthrower();

    for (unsigned int i = 0; i < iterations; ++i) {
        try {
            throw CustomException();
        }
        catch (const CustomException&) {
            ++failure_count;
        }
    }

    EXPECT_EQ(deactivateOverthrower(), 0);

    EXPECT_GE(failure_count, iterations / 4);
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

extern "C" void* somePureCFunction();

TEST(Overthrower, PureC)
{
    OverthrowerConfiguratorStep overthrower_configurator(0);
    ASSERT_EQ(somePureCFunction(), nullptr);
}
