#pragma once

#include <DirectXMath.h>
using namespace DirectX;

#include "common.h"

struct Renderer;

Renderer* rd_init(Arena* arena, void* window);
void rd_free(Renderer* r);

struct RDMesh {
    void* data;
    u32 generation;
};

struct RDVertex {
    XMFLOAT3 pos;
    XMFLOAT3 norm;
    XMFLOAT2 uv;
};

RDMesh rd_create_mesh(Renderer* r, RDVertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count);
void rd_free_mesh(Renderer* r, RDMesh mesh);

struct MeshInstance {
    RDMesh mesh;
    XMMATRIX transform;
};

void rd_render(Renderer* r, u32 instance_count, MeshInstance* instances);