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

class OverthrowerConfigurator {
public:
    OverthrowerConfigurator(unsigned int strategy = 0,
                            unsigned int seed = 0,
                            unsigned int duty_cycle = 1024,
                            unsigned int delay = 0,
                            unsigned int duration = 1);
    ~OverthrowerConfigurator();

    void setEnv(const char* name, unsigned int value);
    void unsetEnv(const char* name);
};

OverthrowerConfigurator::OverthrowerConfigurator(unsigned int strategy, unsigned int seed, unsigned int duty_cycle, unsigned int delay, unsigned int duration)
{
    setEnv("OVERTHROWER_STRATEGY", strategy);
    setEnv("OVERTHROWER_SEED", seed);
    setEnv("OVERTHROWER_DUTY_CYCLE", duty_cycle);
    setEnv("OVERTHROWER_DELAY", delay);
    setEnv("OVERTHROWER_DURATION", duration);
}

OverthrowerConfigurator::~OverthrowerConfigurator()
{
    unsetEnv("OVERTHROWER_STRATEGY");
    unsetEnv("OVERTHROWER_SEED");
    unsetEnv("OVERTHROWER_DUTY_CYCLE");
    unsetEnv("OVERTHROWER_DELAY");
    unsetEnv("OVERTHROWER_DURATION");
}

void OverthrowerConfigurator::setEnv(const char* name, unsigned int value)
{
    ASSERT_EQ(setenv(name, std::to_string(value).c_str(), 1), 0);
}

void OverthrowerConfigurator::unsetEnv(const char* name)
{
    ASSERT_EQ(unsetenv(name), 0);
}

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
    OverthrowerConfigurator overthrower_configurator(0);
    EXPECT_DEATH(fragileCodeWithOverthrower(), "");
}

TEST(Overthrower, MemoryLeak)
{
    OverthrowerConfigurator overthrower_configurator(3);
    activateOverthrower();
    void* buffer = malloc(128);
    forced_memset(buffer, 0, 128);
    EXPECT_EQ(deactivateOverthrower(), 1);
    free(buffer);
}

TEST(Overthrower, Pause)
{
    OverthrowerConfigurator overthrower_configurator(0);
    activateOverthrower();
    pauseOverthrower(0);
    fragileCode();
    resumeOverthrower();
    EXPECT_EQ(deactivateOverthrower(), 0);
}