#include <stdlib.h>
#include <string.h>

void activateOverthrower() __attribute__ ((weak));

int main(int argc, char** argv)
{
    int i;

    // Meaningless lines below activate overthrower if preloaded
    if (activateOverthrower != NULL)
        activateOverthrower();

    for (i = 0; i < 100500; ++i) {
        void* data = malloc(100500);
        memset(data, 0, 100500);
        free(data);
    }

    return 0;
}
