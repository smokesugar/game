#pragma once

#include "common.h"

inline u64 fn1va_hash_string(const char* str) {
    u64 hash = 0xcbf29ce484222325;

    for (const char* c = str; *c; ++c) {
        hash ^= *c;
        hash *= 0x100000001b3;
    }

    return hash;
}

template<typename T>
inline void raw_dictionary_insert(u32 cap, char** keys, T* values, char* key, T value) {
    u64 hash = fn1va_hash_string(key);
    u32 i = hash % cap;

    for (u32 j = 0; j < cap; ++j) {
        if (!keys[i]) {
            keys[i] = key;
            values[i] = value;
            return;
        }
        i = (i+1) % cap;
    }

    assert(false && "insufficient space in hash table");
}

inline int find_index(u32 cap, char** keys, const char* key) {
    u64 hash = fn1va_hash_string(key);
    u32 i = hash % cap;

    for (u32 j = 0; j < cap; ++j) {
        if (keys[i] && strcmp(keys[i], key) == 0) {
            return i;
        }

        i = (i+1) % cap;
    }

    return -1;
}

template <typename T>
struct Dictionary {
    u32 cap;
    u32 count;
    char** keys;
    T* values;

    f32 load_factor() {
        return (f32)count/(f32)cap;
    }

    void insert(const char* key, T value) {
        if (cap == 0) {
            cap = 8;
            keys = (char**)calloc(cap, sizeof(keys[0]));
            values = (T*)calloc(cap, sizeof(values[0]));
        }

        assert(!has(key));

        if (load_factor() > 0.5f) {
            u32 new_cap = cap * 2;

            char** new_keys = (char**)calloc(new_cap, sizeof(keys[0]));
            T* new_values = (T*)calloc(new_cap, sizeof(values[0]));

            for (u32 i = 0; i < cap; ++i) {
                if (keys[i]) {
                    raw_dictionary_insert(new_cap, new_keys, new_values, keys[i], values[i]);
                }
            }

            ::free(keys);
            ::free(values);

            cap = new_cap;
            keys = new_keys;
            values = new_values;
        }

        raw_dictionary_insert(cap, keys, values, _strdup(key), value);

        count++;
    }

    bool has(const char* key) {
        return find_index(cap, keys, key) != -1;
    }

    T& at(const char* key) {
        int index = find_index(cap, keys, key);
        assert(index != -1);
        return values[index];
    }

    T& operator[](const char* key) {
        return at(key);
    }

    void free() {
        for (u32 i = 0; i < cap; ++i) {
            if (keys[i]) {
                ::free(keys[i]);
            }
        }

        ::free(keys);
        ::free(values);
        memset(this, 0, sizeof(*this));
    }
};
