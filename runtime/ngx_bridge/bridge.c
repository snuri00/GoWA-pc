#include <stdint.h>

typedef unsigned int (*gowa_ngx_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

__declspec(dllexport) __declspec(noinline) unsigned int gowa_ngx_call(void* fn, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    volatile unsigned int r = ((gowa_ngx_fn)fn)(a0, a1, a2, a3, a4, a5, a6, a7, a8);
    return r;
}
