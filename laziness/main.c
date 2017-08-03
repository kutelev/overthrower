#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void activateOverthrower() __attribute__ ((weak));

int main(int argc, char** argv)
{
    int i;
    char tmp[1024];

    // Meaningless lines below activate overthrower if preloaded
    if (activateOverthrower != NULL)
        activateOverthrower();

    for (i = 0; i < 100500; ++i) {
        void* data = malloc(100500);
        memset(data, 0, 100500);
        sprintf(tmp, "data = %p\n", data);
        free(data);
    }

    printf("%s", tmp);

    return 0;
}
