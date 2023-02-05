#pragma once

#include "common.h"

struct Shader {
    u64 len;
    void* memory;
};

Shader compile_shader(Arena* arena, const char* path, const char* entry_point, const char* target);
