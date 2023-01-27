#pragma once

#include "common.h"

void pf_msg_box(const char* fmt, ...);
void pf_debug_log(const char* fmt, ...);
f32 pf_time();

struct FileContents {
    u64 size;
    void* memory;
};

FileContents pf_load_file(Arena* arena, const char* path);
