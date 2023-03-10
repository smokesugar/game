#pragma once

#include <stdint.h>
#include <assert.h>
#include <memory.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a)/sizeof(a[0]))

#define PI32 3.1415926535f 

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

inline void sanitise_path(char* str) {
    for (char* c = str; *c; ++c) {
        if (*c == '\\') {
            *c = '/';
        }
    }
}

template<typename T>
struct Vec {
    T* mem;
    u32 cap;
    u32 len;

    T& push(T t) {
        if (len + 1 > cap) {
            cap = cap < 8 ? 8 : cap * 2;
            mem = (T*)realloc(mem, cap * sizeof(T));
        }

        mem[len++] = t;
        return mem[len-1];
    }

    bool empty() {
        return len == 0;
    }

    void clear() {
        len = 0;
    }

    T pop() {
        assert(len > 0);
        return mem[--len];
    }

    void remove_by_patch(u32 i) {
        assert(i < len);
        mem[i] = mem[--len];
    }

    T& at(u32 i) {
        assert(i < len);
        return mem[i];
    }

    T& operator[](u32 i) {
        return at(i);
    }

    void free() {
        ::free(mem);
        memset(this, 0, sizeof(*this));
    }
};

template<typename T, u32 C>
struct StaticVec {
    T mem[C];
    u32 len;

    T& push(T t) {
        assert(len < C);
        mem[len++] = t;
        return mem[len-1];
    }

    bool empty() {
        return len == 0;
    }

    void clear() {
        len = 0;
    }

    T pop() {
        assert(len > 0);
        return mem[--len];
    }

    void remove_by_patch(u32 i) {
        assert(i < len);
        mem[i] = mem[--len];
    }

    T& at(u32 i) {
        assert(i < len);
        return mem[i];
    }

    T& operator[](u32 i) {
        return at(i);
    }
};

inline struct Arena arena_init(void* mem, u64 size);

struct Arena {
    u64 size;
    u64 allocated;
    void* mem;

    u64 save_state;

    void* push(u64 s) {
        if (s == 0) {
            return 0;
        }

        s = (s + 7) & ~7;

        assert(size-allocated >= s && "arena out of memory");

        void* ptr = (u8*)mem + allocated;
        allocated += s;

        return ptr;
    }

    void* push_zero(u64 s) {
        void* ptr = push(s);

        if (ptr) {
            memset(ptr, 0, s);
        }

        return ptr;
    }

    template<typename T>
    T* push_type() {
        return (T*)push_zero(sizeof(T));
    }

    template<typename T>
    T* push_array(u32 count) {
        return (T*)push_zero(sizeof(T) * count);
    }

    template<typename T>
    T* push_vec_contents(Vec<T> vec) {
        u64 s = vec.len * sizeof(T);
        void* p = push(s);
        memcpy(p, vec.mem, s);
        return (T*)p;
    }

    Arena sub_arena(u64 s) {
        return arena_init(push(s), s);
    }

    inline void reset() {
        allocated = 0;
    }

    void save() {
        save_state = allocated;
    }

    void restore() {
        allocated = save_state;
    }
};

template<typename T>
struct PoolAllocator {
    struct Node {
        T x;
        Node* next;
    };

    Arena* arena;
    Node* free_list;

    void init(Arena* arena_backing) {
        arena = arena_backing;
    }

    T* alloc() {
        Node* n;

        if (!free_list) {
            n = arena->push_type<Node>();
        }
        else {
            n = free_list;
            free_list = n->next;
            memset(n, 0, sizeof(*n));
        }

        return (T*)n;
    }

    void free(T* x) {
        Node* node = (Node*)x;
        node->next = free_list;
        free_list = node;
    }
};

inline Arena arena_init(void* mem, u64 size) {
    Arena arena = {};
    arena.size = size;
    arena.mem = mem;
    return arena;
}

struct Scratch {
    Arena* arena;
    u64 state;

    Scratch(Arena* arena, u64 state)
        : arena(arena), state(state)
    {
    }

    Scratch(Scratch& other) = delete;
    Scratch& operator=(Scratch& other) = delete;

    ~Scratch() {
        arena->allocated = state;
    }

    Arena* operator->() {
        return arena;
    }
};

Scratch get_scratch(Arena* conflict);

template<typename A, typename B>
struct Pair {
    A a; 
    B b;
};

template<typename T>
inline void swap(T& a, T& b) {
    T temp = a;
    a = b;
    b = temp;
}
