#pragma once

#include <stdint.h>
#include <assert.h>
#include <memory.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

struct Arena {
    u64 size;
    u64 allocated;
    void* mem;

    inline void* push(u64 s) {
        assert(size-allocated >= s && "arena out of memory");
        void* ptr = (u8*)mem + allocated;
        allocated += s;
        return ptr;
    }

    inline void* push_zero(u64 s) {
        void* ptr = push(s);
        memset(ptr, 0, s);
        return ptr;
    }

    template<typename T>
    inline T* push_type() {
        return (T*)push_zero(sizeof(T));
    }

    inline void reset() {
        allocated = 0;
    }
};

inline Arena arena_init(void* mem, u64 size) {
    Arena arena = {};
    arena.size = size;
    arena.mem = mem;
    return arena;
}

template<typename T>
struct Vec {
    T* mem;
    u32 cap;
    u32 len;

    void push(T t) {
        if (len + 1 > cap) {
            cap = cap < 8 ? 8 : cap * 2;
            mem = realloc(mem, cap * sizeof(T));
        }

        mem[len++] = t;
    }

    inline T pop() {
        assert(len > 0);
        return mem[--len];
    }

    inline T& at(u32 i) {
        assert(i < len);
        return mem[i];
    }

    inline T& operator[](u32 i) {
        return at(i);
    }

    inline void free() {
        ::free(mem);
        memset(this, 0, sizeof(*this));
    }
};
