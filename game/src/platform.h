#pragma once

#include "common.h"

void pf_msg_box(const char* msg);
void pf_debug_log(const char* msg);
f32 pf_time();

struct FileContents {
    u64 size;
    void* memory;
};

FileContents pf_load_file(Arena* arena, const char* path);
