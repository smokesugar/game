#pragma once

#include <DirectXMath.h>
using namespace DirectX;

#define RESOURCE_HANDLE(name) struct name { void* data; u32 generation; };

#include "common.h"

struct Renderer;

Renderer* rd_init(Arena* arena, void* window);
void rd_free(Renderer* r);

struct RDUploadContext;
struct RDUploadStatus;
RDUploadContext* rd_open_upload_context(Renderer* r);
RDUploadStatus* rd_submit_upload_context(Renderer* r, RDUploadContext* upload_context);
bool rd_upload_status_finished(Renderer* r, RDUploadStatus* upload_status);
void rd_flush_upload(Renderer* r, RDUploadStatus* upload_status);

RESOURCE_HANDLE(RDMesh);
RESOURCE_HANDLE(RDTexture);

struct RDVertex {
    XMFLOAT3 pos;
    XMFLOAT3 norm;
    XMFLOAT2 uv;
};

RDMesh rd_create_mesh(Renderer* r, RDUploadContext* upload_context, RDVertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count);
void rd_free_mesh(Renderer* r, RDMesh mesh);

RDTexture rd_create_texture(Renderer* r, RDUploadContext* upload_context, u32 width, u32 height, void* contents);
void rd_free_texture(Renderer* r, RDTexture texture);

RDTexture rd_get_white_texture(Renderer* r);

struct RDMaterial {
    RDTexture albedo_texture;
    XMFLOAT3 albedo_factor;
};

struct RDMeshInstance {
    RDMesh mesh;
    RDMaterial material;
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