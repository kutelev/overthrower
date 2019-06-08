#ifndef UUID_C16C71B8_1C78_11E9_9351_473D761C7137
#define UUID_C16C71B8_1C78_11E9_9351_473D761C7137

#include <cinttypes>

#include <pthread.h>

template<typename T>
class ThreadLocal final {
    static_assert(sizeof(T) <= sizeof(uintptr_t), "Inappropriate data type");

public:
    ThreadLocal() { pthread_key_create(&key, nullptr); }
    ~ThreadLocal() { pthread_key_delete(key); }

    ThreadLocal& operator=(T value) { set(value); return *this; }
    operator T() const { return get(); }

private:
    void set(const T value) { pthread_setspecific(key, reinterpret_cast<void*>(value)); }
    T get() const { return (T)(pthread_getspecific(key)); }

    pthread_key_t key{};
};

#endif
