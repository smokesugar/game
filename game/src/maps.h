#pragma once

#include "common.h"

#define MAX_LOAD_FACTOR 0.5f

inline u64 fn1va_hash_bytes(void* bytes, size_t len) {
    u64 hash = 0xcbf29ce484222325;

    for (size_t i = 0; i < len; ++i) {
        hash ^= ((u8*)bytes)[i];
        hash *= 0x100000001b3;
    }

    return hash;
}

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

template<typename K, typename V>
inline void raw_hash_map_insert(u32 cap, K* keys, V* values, bool* occupation, K key, V value) {
    u64 hash = fn1va_hash_bytes(&key, sizeof(key));
    u32 i = hash % cap;

    for (u32 j = 0; j < cap; ++j) {
        if (!occupation[i]) {
            keys[i] = key;
            values[i] = value;
            occupation[i] = true;
            return;
        }
        i = (i+1) % cap;
    }

    assert(false && "insufficient space in hash table");
}

inline int raw_dictionary_find(u32 cap, char** keys, const char* key) {
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

template<typename K>
inline int raw_hash_map_find(u32 cap, K* keys, bool* occupation, K key) {
    u64 hash = fn1va_hash_bytes(&key, sizeof(key));
    u32 i = hash % cap;

    for (u32 j = 0; j < cap; ++j) {
        if (occupation[i] && memcmp(&keys[i], &key, sizeof(key)) == 0) {
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

        if (load_factor() > MAX_LOAD_FACTOR) {
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
        return raw_dictionary_find(cap, keys, key) != -1;
    }

    T& at(const char* key) {
        int index = raw_dictionary_find(cap, keys, key);
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

template<typename K, typename V>
struct HashMap {
    u32 cap;
    u32 count;
    K* keys;
    V* values;
    bool* occupation;

    f32 load_factor() {
        return (f32)count/(f32)cap;
    }

    void insert(K key, V value) {
        if (cap == 0) {
            cap = 8;
            keys = (K*)calloc(cap, sizeof(keys[0]));
            values = (V*)calloc(cap, sizeof(values[0]));
            occupation = (bool*)calloc(cap, sizeof(occupation[0]));
        }

        assert(!has(key));

        if (load_factor() > MAX_LOAD_FACTOR) {
            u32 new_cap = cap * 2;

            K* new_keys = (K*)calloc(new_cap, sizeof(keys[0]));
            V* new_values = (V*)calloc(new_cap, sizeof(values[0]));
            bool* new_occupation = (bool*)calloc(new_cap, sizeof(occupation[0]));

            for (u32 i = 0; i < cap; ++i) {
                if (occupation[i]) {
                    raw_hash_map_insert(new_cap, new_keys, new_values, new_occupation, keys[i], values[i]);
                }
            }

            ::free(keys);
            ::free(values);
            ::free(occupation);

            cap = new_cap;
            keys = new_keys;
            values = new_values;
            occupation = new_occupation;
        }

        raw_hash_map_insert(cap, keys, values, occupation, key, value);

        count++;
    }

    bool has(K key) {
        return raw_hash_map_find(cap, keys, occupation, key) != -1;
    }

    V& at(K key) {
        int index = raw_hash_map_find(cap, keys, occupation, key);
        assert(index != -1);
        return values[index];
    }

    V& operator[](K key) {
        return at(key);
    }

    void free() {
        ::free(keys);
        ::free(values);
        ::free(occupation);
        memset(this, 0, sizeof(*this));
    }
};

template<typename T, u32 C>
struct StaticSet {
    T mem[C];
    bool occupation[C];

    void insert(T t) {
        u64 hash = fn1va_hash_bytes(&t, sizeof(t));
        u32 i = hash % C;

        int first_empty = -1;

        for (u32 j = 0; j < C; ++j) {
            if (occupation[i]) {
                if (memcmp(&mem[i], &t, sizeof(T)) == 0) {
                    return;
                }
            }
            else {
                if (first_empty == -1) {
                    first_empty = i;
                }
            }

            i = (i + 1) % C;
        }

        assert(first_empty != -1);
        assert(first_empty < C);

        mem[first_empty] = t;
        occupation[first_empty] = true;
    }

    void has(T t) {
        u64 hash = fn1va_hash_bytes(&t, sizeof(t));
        u32 i = hash % C;

        for (u32 j = 0; j < C; ++j) {
            if (occupation[i] && memcmp(&mem[i], &t, sizeof(T)) == 0) {
                return true;
            }

            i = (i + 1) % C;
        }

        return false;
    }

    template<typename F>
    inline void for_each(F f) {
        for (u32 i = 0; i < C; ++i) {
            if (occupation[i]) {
                f(mem[i]);
            }
        }
    }
};
