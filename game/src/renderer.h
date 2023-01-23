#pragma once

#include "common.h"

struct Renderer;

Renderer* rd_init(Arena* arena);
void rd_free(Renderer* r);