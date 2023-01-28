#include <string.h>
#include <stdio.h>

#include "gltf.h"
#include "platform.h"
#include "json.h"

struct Buffer {
    u32 len;
    void* memory;
};

struct BufferView {
    u32 buffer;
    u32 len;
    u32 offset;
};

enum GLType {
    GL_BYTE           = 0x1400,
    GL_UNSIGNED_BYTE  = 0x1401,
    GL_SHORT          = 0x1402,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_INT            = 0x1404,
    GL_UNSIGNED_INT   = 0x1405,
    GL_FLOAT          = 0x1406,
};

struct Accessor {
    u32 buffer_view;
    u32 offset;
    GLType component_type;
    u32 count;
    u32 component_count;

    void* get_memory(Vec<Buffer>* buffers, Vec<BufferView>* buffer_views) {
        void* base = buffers->at(buffer_views->at(buffer_view).buffer).memory;
        u32 buffer_offset = buffer_views->at(buffer_view).offset + offset;
        return (u8*)base + buffer_offset;
    }
};

GLTFResult gltf_load(Arena* arena, Renderer* renderer, const char* path) {
    Scratch scratch = get_scratch(arena);

    (void)renderer;

    char dir[512];
    strcpy_s(dir, sizeof(dir), path);
    sanitise_path(dir);

    char* path_last_slash = strrchr(dir, '/');
    if (path_last_slash) {
        path_last_slash[1] = '\0';
    }
    else {
        dir[0] = '\0';
    }

    FileContents file = pf_load_file(scratch.arena, path);
    JSON root = json_parse(scratch.arena, (char*)file.memory);

    char* version = root["asset"]["version"].as_string();
    (void)version;
    assert(strcmp(version, "2.0") == 0);

    Vec<Buffer> buffers = {};
    Vec<BufferView> buffer_views = {};
    Vec<Accessor> accessors = {};
    Vec<RDMesh> meshes = {};

    JSON json_buffers = root["buffers"];
    for (u32 i = 0; i < json_buffers.array_len(); ++i)
    {
        JSON json_buffer = json_buffers[i];

        char* uri = json_buffer["uri"].as_string();

        char buffer_path[512];
        sprintf_s(buffer_path, sizeof(buffer_path), "%s%s", dir, uri);

        FileContents buffer_contents = pf_load_file(scratch.arena, buffer_path);

        Buffer buffer = {};
        buffer.len = json_buffer["byteLength"].as_int();
        buffer.memory = buffer_contents.memory;

        assert(buffer.len == buffer_contents.size);

        buffers.push(buffer);
    }

    JSON json_buffer_views = root["bufferViews"];
    for (u32 i = 0; i < json_buffer_views.array_len(); ++i)
    {
        JSON json_buffer_view = json_buffer_views[i];

        BufferView buffer_view = {};
        buffer_view.buffer = json_buffer_view["buffer"].as_int();
        buffer_view.len = json_buffer_view["byteLength"].as_int();
        buffer_view.offset = json_buffer_view["byteOffset"].as_int();

        buffer_views.push(buffer_view);
    }

    JSON json_accessors = root["accessors"];
    for (u32 i = 0; i < json_accessors.array_len(); ++i) {
        JSON json_accessor = json_accessors[i];

        Accessor accessor = {};
        accessor.buffer_view = json_accessor["bufferView"].as_int();
        accessor.component_type = (GLType)json_accessor["componentType"].as_int();
        accessor.count = json_accessor["count"].as_int();

        if (json_accessor.has("byteOffset")) {
            accessor.offset = json_accessor["byteOffset"].as_int();
        }

        char* type = json_accessor["type"].as_string();

        if (strcmp(type, "SCALAR") == 0) {
            accessor.component_count = 1;
        }
        else if (strcmp(type, "VEC2") == 0) {
            accessor.component_count = 2;
        }
        else if (strcmp(type, "VEC3") == 0) {
            accessor.component_count = 3;
        }
        else if (strcmp(type, "VEC4") == 0) {
            accessor.component_count = 4;
        }
        else {
            assert(false && "gltf has invalid type");
        }

        accessors.push(accessor);
    }

    JSON json_meshes = root["meshes"];
    for (u32 i = 0; i < json_meshes.array_len(); ++i)
    {
        JSON json_mesh = json_meshes[i];
        JSON primitives = json_mesh["primitives"];

        for (u32 j = 0; j < primitives.array_len(); ++j)
        {
            scratch->save();

            JSON primitive = primitives[j];

            JSON attributes = primitive["attributes"];

            u32 pos_accessor_index = attributes["POSITION"].as_int();
            u32 norm_accessor_index = attributes["NORMAL"].as_int();
            u32 uv_accessor_index = attributes["TEXCOORD_0"].as_int();
            u32 indices_accessor_index = primitive["indices"].as_int();

            Accessor pos_accessor = accessors[pos_accessor_index];
            Accessor norm_accessor = accessors[norm_accessor_index];
            Accessor uv_accessor = accessors[uv_accessor_index];
            Accessor indices_accessor = accessors[indices_accessor_index];

            assert(pos_accessor.count == norm_accessor.count);
            assert(pos_accessor.count == uv_accessor.count);

            assert(pos_accessor.component_count == 3);
            assert(norm_accessor.component_count == 3);
            assert(uv_accessor.component_count == 2);

            assert(pos_accessor.component_type  == GL_FLOAT);
            assert(norm_accessor.component_type == GL_FLOAT);
            assert(uv_accessor.component_type   == GL_FLOAT);

            f32* pos_accessor_memory  = (f32*)pos_accessor.get_memory(&buffers, &buffer_views);
            f32* norm_accessor_memory = (f32*)norm_accessor.get_memory(&buffers, &buffer_views);
            f32* uv_accessor_memory   = (f32*)uv_accessor.get_memory(&buffers, &buffer_views);

            u32 vertex_count = pos_accessor.count;
            RDVertex* vertex_data = scratch->push_array<RDVertex>(vertex_count);

            for (u32 k = 0; k < vertex_count; ++k) {
                RDVertex vertex;

                vertex.pos.x = pos_accessor_memory[k * pos_accessor.component_count + 0];
                vertex.pos.y = pos_accessor_memory[k * pos_accessor.component_count + 1];
                vertex.pos.z = pos_accessor_memory[k * pos_accessor.component_count + 2];

                vertex.norm.x = norm_accessor_memory[k * norm_accessor.component_count + 0];
                vertex.norm.y = norm_accessor_memory[k * norm_accessor.component_count + 1];
                vertex.norm.z = norm_accessor_memory[k * norm_accessor.component_count + 2];

                vertex.uv.x = uv_accessor_memory[k * uv_accessor.component_count + 0];
                vertex.uv.y = uv_accessor_memory[k * uv_accessor.component_count + 1];

                vertex_data[k] = vertex;
            }

            void* indices_accessor_memory = indices_accessor.get_memory(&buffers, &buffer_views);

            u32 index_count = indices_accessor.count;
            u32* index_data = scratch->push_array<u32>(index_count);

            switch (indices_accessor.component_type) {
                default:
                    assert(false && "invalid gltf index type");
                    break;

                case GL_UNSIGNED_SHORT: {
                    u16* shorts = (u16*)indices_accessor_memory;
                    for (u32 k = 0; k < index_count; ++k) {
                        index_data[k] = shorts[k];
                    }
                } break;

                case GL_UNSIGNED_INT: {
                    memcpy(index_data, indices_accessor_memory, index_count * sizeof(u32));
                } break;
            }

            RDMesh mesh = rd_create_mesh(renderer, vertex_data, vertex_count, index_data, index_count);
            meshes.push(mesh);
            
            scratch->restore();
        }
    }
    
    GLTFResult result = {};
    result.num_meshes = meshes.len;
    result.meshes = arena->push_vec_contents(meshes);

    buffers.free();
    buffer_views.free();
    accessors.free();
    meshes.free();

    return result;
}
