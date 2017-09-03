#include <src/gmock-all.cc>
#include <src/gtest-all.cc>

#include "platform.h"

extern "C" {
void activateOverthrower() __attribute__((weak));
unsigned int deactivateOverthrower() __attribute__((weak));
void pauseOverthrower(unsigned int duration) __attribute__((weak));
void resumeOverthrower() __attribute__((weak));
}

void* (*volatile forced_memset)(void*, int, size_t) = memset;

GTEST_API_ int main(int argc, char** argv)
{
    if (!activateOverthrower || !deactivateOverthrower || !pauseOverthrower || !resumeOverthrower) {
        fprintf(stderr, "Seems like overthrower has not been injected or not fully available. Nothing to do.");
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
        setEnv("OVERTHROWER_STRATEGY", 0);
        setEnv("OVERTHROWER_SEED", 0);
        setEnv("OVERTHROWER_DUTY_CYCLE", duty_cycle);
    }
};

class OverthrowerConfiguratorStep : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorStep() = delete;
    OverthrowerConfiguratorStep(unsigned int delay)
    {
        setEnv("OVERTHROWER_STRATEGY", 1);
        setEnv("OVERTHROWER_DELAY", delay);
    }
};

class OverthrowerConfiguratorPulse : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorPulse() = delete;
    OverthrowerConfiguratorPulse(unsigned int delay, unsigned int duration)
    {
        setEnv("OVERTHROWER_STRATEGY", 2);
        setEnv("OVERTHROWER_DELAY", delay);
        setEnv("OVERTHROWER_DURATION", duration);
    }
};

class OverthrowerConfiguratorNone : public AbstractOverthrowerConfigurator {
public:
    OverthrowerConfiguratorNone() { setEnv("OVERTHROWER_STRATEGY", 3); }
};

static void fragileCode()
{
    for (int i = 0; i < 100500; ++i) {
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

TEST(Overthrower, Pause)
{
    OverthrowerConfiguratorRandom overthrower_configurator;
    activateOverthrower();
    pauseOverthrower(0);
    fragileCode();
    resumeOverthrower();
    EXPECT_EQ(deactivateOverthrower(), 0);
}