#include <cstdlib>
#include <dlfcn.h>

#include "overthrower.h"

int main(int, const char**)
{
    setenv("OVERTHROWER_STRATEGY", "3", 1); // STRATEGY_NONE

    activateOverthrower();

    auto* handle = dlopen("libleaking_library.so", RTLD_NOW);
    if (!handle) {
        deactivateOverthrower();
        return EXIT_FAILURE;
    }
    dlclose(handle);

    if (deactivateOverthrower() != 0U) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
