#include "stdlib.h"

// Modern compilers treat `free(NULL)` as useless operation and remove it.
// Force compiler to invoke `free(NULL)`:
static void (*volatile forced_free)(void*) = free;

int main(int argc, const char** argv)
{
    // `overthrower` used to crash when `free(NULL)` was invoked before any preceding `malloc` operations.
    // The original problem was happening when `overthrower` was built on CentOS 7.9 using devtoolset-9 and then used on CentOS 7.2.
    // Invoke `free(NULL)` at the very beginning in order to check that `overthrower` does not crash anymore.
    forced_free(NULL);
}
