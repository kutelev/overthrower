#ifndef UUID_40B5F6F2_1336_11E9_BE5D_CF97C16D7468
#define UUID_40B5F6F2_1336_11E9_BE5D_CF97C16D7468

extern "C" {
void activateOverthrower() __attribute__((weak));
unsigned int deactivateOverthrower() __attribute__((weak));
void pauseOverthrower(unsigned int duration) __attribute__((weak));
void resumeOverthrower() __attribute__((weak));
}

#endif
