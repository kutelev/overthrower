# Overthrower

[![Build Status](https://travis-ci.org/kutelev/overthrower.svg?branch=master)](https://travis-ci.org/kutelev/overthrower)

Overthrower is a library which capable of intercepting `malloc`/`realloc` invocations and fail them based on a chosen pattern.

This library is supposed to be used in out of memory tests of other libraries/applications.

Supported operating systems are:
* Linux
* macOS

Overthrower uses `LD_PRELOAD` mechanism on Linux, `DYLD_INSERT_LIBRARIES` is used for same purposes on macOS.

# Usage scenario

If a behaviour of some parts of any application/library is planned to be validated in out of memory conditions some way of failing certain allocations is required.
By default, when preloaded, Overthrower is an dormant state - it does fail any allocations.
In order to force Overthrower to start failing allocations it needs to be activated.
When no further failures are required Overthrower can be deactivated.
Overthrower can be activated/deactivated as many times as required.

In order to start using this library a being tested application needs to declare the following weak symbols:
```cpp
void activateOverthrower() __attribute__((weak));
unsigned int deactivateOverthrower() __attribute__((weak));
```

These two functions along with two additional optional are declared in the `overthrower.h` header file.

When Overthrower is preloaded using `LD_PRELOAD` (or `DYLD_INSERT_LIBRARIES`) pointers to these functions are overridden with those which are implemented in Overthrower.  

When activated, Overthrower can be paused/unpaused using the following functions:
```cpp
void pauseOverthrower(unsigned int duration) __attribute__((weak));
void resumeOverthrower() __attribute__((weak));
``` 

On Linux, nothing but exporting `LD_PRELOAD` is required. on macOS a being tested application needs to be linker with the following additional flags:
```
-Wl,-U,_activateOverthrower -Wl,-U,_deactivateOverthrower -Wl,-U,_pauseOverthrower -Wl,-U,_resumeOverthrower
```

Also, macOS requires exporting the `DYLD_FORCE_FLAT_NAMESPACE` environment variable, this variable has to be set to `1`.

By default, when activated, Overthrower uses random strategy with random parameters for failing allocations.
A necessary strategy can be chosen and configured via the following environment variables:
* `OVERTHROWER_STRATEGY`
* `OVERTHROWER_SEED`
* `OVERTHROWER_DELAY`
* `OVERTHROWER_DURATION`

	
| Variable | Possible values | Description |
| - | - | - |
| `OVERTHROWER_STRATEGY` | `0` - `random`, `1` - `step`, `2` - `pulse`, `3` - `none` | Strategy to use. |
| `OVERTHROWER_SEED` | Any 32-bit unsigned integer value. | A seed to initialize a generator of pseudo random numbers. Affects only `random` strategy. |
| `OVERTHROWER_DUTY_CYCLE` | `[1;4096]` | Determines percentage of allocations which will be failed, 1 - 100% of allocations will fail, 2 - 50%. Affects only `random` strategy. |
| `OVERTHROWER_DELAY` | `[0;1000000]` | Delay before Overthrower starts failing allocations. Affects `step` and `pulse` strategies. |
| `OVERTHROWER_DURATION` | `[1;100]` | Count of allocations to fail. Affects only `pulse` strategy. |

**Note:** `none` strategy does not fail any allocation. It only checks that all memory blocks which are allocated using either `malloc` or `free` are freed using `free`.
All other strategies also perform this validation. If any memory blocks are not freed a user is informed about it. 

# Strategies

## Random

`random` strategy uses a generator of pseudo random numbers to decide whether to fail the current allocation or not.

```cpp
static bool isTimeToFail(unsigned int malloc_seq_num)
{
    return rand() % duty_cycle == 0;
}
```

If only strategy is chosen explicitly, `OVERTHROWER_SEED` and `OVERTHROWER_DUTY_CYCLE` values are randomly generated.

Percentage of allocations which are failed by this strategy can be calculated the following way: `percentage = 100% / duty_cycle.`

## Step

`step` strategy starts failing all allocations after a specified delay, if this parameter is not specified explicitly it is picked randomly from the following range: `[0;1000]`.
Value `0` effectively forces Overthrower to fail all allocations.

```cpp
static bool isTimeToFail(unsigned int malloc_seq_num)
{
    return malloc_seq_num >= delay;
}
```

```
<--- delay --->
--------------+
              |
              | All further allocations fail
              |
              +------------------------------
```

## Pulse

When `pulse` strategy is chosen, certain count of allocations fail after a specified delay.

```cpp
static bool isTimeToFail(unsigned int malloc_seq_num)
{
    return malloc_seq_num > delay && malloc_seq_num <= delay + duration;
}
```

```
<--- delay --->
--------------+                +------------------------------
              |                |
              |                | All further allocations pass
              |                |
              +----------------+
              <--- duration --->
```

If `OVERTHROWER_DELAY` and `OVERTHROWER_DURATION` parameters are not provided explicitly they are picked randomly the following way:
* `OVERTHROWER_DELAY` randomly picked from range `[0;1000]`.
* `OVERTHROWER_DURATION` randomly picked from range `[1;100]`.

## None

As it was written before, `none` strategy does not fail any allocations.
This can be used when you want to check whether there are any memory leaks in a being tested code or not.
