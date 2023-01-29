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

struct RDMeshInstance {
    RDMesh mesh;
    XMMATRIX transform;
};

struct RDPointLight {
    XMFLOAT3 position;
    XMFLOAT3 intensity;
};

struct RDDirectionalLight {
    XMFLOAT3 direction;
    XMFLOAT3 intensity;
};

struct RDCamera {
    XMMATRIX transform;
    f32 vertical_fov;
};

struct RDRenderInfo {
    RDCamera* camera;

    u32 num_point_lights;
    u32 num_directional_lights;
    RDPointLight* point_lights;
    RDDirectionalLight* directional_lights;

    u32 num_instances;
    RDMeshInstance* instances;
};

void rd_render(Renderer* r, RDRenderInfo* render_info);