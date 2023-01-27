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
    } as;
};

struct JSONPair {
    char* name;
    JSON json;
};

JSON parse_json(Arena* arena, char* str);

