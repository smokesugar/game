#pragma once

#include "common.h"

enum JSONType {
    JSON_INVALID,
    JSON_NULL,
    JSON_FLOAT,
    JSON_INT,
    JSON_STRING,
    JSON_BOOLEAN,
    JSON_ARRAY,
    JSON_OBJECT
};

struct JSON {
    JSONType type;
    union {
        f32 _float;
        i32 _int;
        char* string;
        bool boolean;
        struct {
            u32 len;
            JSON* mem;
        } array;
        struct {
            u32 count;
            struct JSONPair* mem; 
        } object;
    } u;

    f32 as_float();
    i32 as_int();
    char* as_string();
    bool as_boolean();

    u32 array_len();
    JSON at(u32 i);

    u32 object_count();
    JSON at(const char* str);

    JSON operator[](u32 i) { return at(i); }
    JSON operator[](const char* str) { return at(str); }
};

struct JSONPair {
    char* name;
    JSON json;
};

JSON parse_json(Arena* arena, char* str);

