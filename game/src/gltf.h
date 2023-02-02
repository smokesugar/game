#pragma once

#include "renderer.h"

struct GLTFResult {
    u32 num_instances;
    RDMeshInstance* instances;
    u32 num_meshes;
    RDMesh* meshes;
    u32 num_textures;
    RDTexture* textures;
};

GLTFResult gltf_load(Arena* arena, Renderer* renderer, RDUploadContext* upload_context, const char* path);

