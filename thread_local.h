#ifndef UUID_C16C71B8_1C78_11E9_9351_473D761C7137
#define UUID_C16C71B8_1C78_11E9_9351_473D761C7137

#include <cinttypes>

#include <pthread.h>

template<typename T>
class ThreadLocal final {
    static_assert(sizeof(T) <= sizeof(uintptr_t), "Inappropriate data type");

public:
    ThreadLocal() noexcept
    {
        // Theoretically pthread_key_create can fail but if this happens on this early stage there is nothing we can do to handle this.
        // Considering failed pthread_key_create invocations as disastrous cases and doing nothing regarding this.
        pthread_key_create(&key, nullptr);
    }
    ~ThreadLocal() { pthread_key_delete(key); }

    ThreadLocal& operator=(T value) noexcept
    {
        set(value);
        return *this;
    }
    bool operator==(bool other) const noexcept { return this->operator bool() == other; }
    explicit operator bool() const noexcept { return static_cast<bool>(get()); }

private:
    void set(const T value) noexcept { pthread_setspecific(key, reinterpret_cast<void*>(value)); }
    T get() const noexcept { return (T)(pthread_getspecific(key)); }

    pthread_key_t key{};
};

#endif
