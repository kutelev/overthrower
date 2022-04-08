#include <cstdlib>
#include <memory>

class LeakingObject {
public:
    LeakingObject()
    {
        // Allocate magic amount of bytes and never free them.
        m_never_freed_block = malloc(731465028);
        // We need to do something with a pointer, otherwise smart compilers simply optimize out `malloc` invocation.
        if (m_never_freed_block == nullptr) {
            throw std::bad_alloc();
        }
    }

    ~LeakingObject() { uselessGetter(); }

    void* uselessGetter() const { return m_never_freed_block; }

private:
    void* m_never_freed_block;
};

static LeakingObject g_leaking_object; // NOLINT
