#pragma once

#include "common.h"

struct Renderer;

Renderer* rd_init(Arena* arena, void* window);
void rd_free(Renderer* r);

void rd_render(Renderer* r);