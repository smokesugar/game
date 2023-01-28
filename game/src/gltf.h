#pragma once

#include "renderer.h"

struct GLTFResult {
    u32 num_meshes;
    RDMesh* meshes;
};

GLTFResult gltf_load(Arena* arena, Renderer* renderer, const char* path);

