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

    void* get_memory(Buffer* buffers, BufferView* buffer_views) {
        void* base = buffers[buffer_views[buffer_view].buffer].memory;
        u32 buffer_offset = buffer_views[buffer_view].offset + offset;
        return (u8*)base + buffer_offset;
    }
};

struct MeshGroup {
    u32 start;
    u32 count;
};

struct Node {
    XMMATRIX transform;
    u32 num_children;
    u32* children;
    MeshGroup mesh_group;
};

static void process_node(Node node, Node* nodes, Vec<RDMesh>* meshes, XMMATRIX parent_transform, Vec<RDMeshInstance>* instances) {
    XMMATRIX transform = node.transform * parent_transform;

    for (u32 i = 0; i < node.mesh_group.count; ++i) {
        RDMeshInstance instance;
        instance.mesh = meshes->at(i + node.mesh_group.start);
        instance.transform = transform;
        instances->push(instance);
    }

    for (u32 i = 0; i < node.num_children; ++i) {
        process_node(nodes[node.children[i]], nodes, meshes, transform, instances);
    }
}

static XMVECTOR json_to_xmvector(JSON arr) {
    assert(arr.array_len() <= 4);

    f32 vector_as_floats[4] = {};
    for (u32 i = 0; i < arr.array_len(); ++i) {
        JSON component = arr[i];
        vector_as_floats[i] = component.type == JSON_FLOAT ? component.as_float() : (f32)component.as_int();
    }

    return *((XMVECTOR*)vector_as_floats);
}

GLTFResult gltf_load(Arena* arena, Renderer* renderer, RDUploadContext* upload_context, const char* path) {
    Scratch scratch = get_scratch(arena);

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

    JSON json_buffers = root["buffers"];
    Buffer* buffers = scratch->push_array<Buffer>(json_buffers.array_len());
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

        buffers[i] = buffer;
    }

    JSON json_buffer_views = root["bufferViews"];
    BufferView* buffer_views = scratch->push_array<BufferView>(json_buffer_views.array_len());
    for (u32 i = 0; i < json_buffer_views.array_len(); ++i)
    {
        JSON json_buffer_view = json_buffer_views[i];

        BufferView buffer_view = {};
        buffer_view.buffer = json_buffer_view["buffer"].as_int();
        buffer_view.len = json_buffer_view["byteLength"].as_int();

        if (json_buffer_view.has("byteOffset")) {
            buffer_view.offset = json_buffer_view["byteOffset"].as_int();
        }

        buffer_views[i] = buffer_view;
    }

    JSON json_accessors = root["accessors"];
    Accessor* accessors = scratch->push_array<Accessor>(json_accessors.array_len());
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

        accessors[i] = accessor;
    }

    JSON json_meshes = root["meshes"];
    Vec<RDMesh> meshes = {};
    MeshGroup* mesh_groups = scratch->push_array<MeshGroup>(json_meshes.array_len());
    for (u32 i = 0; i < json_meshes.array_len(); ++i)
    {
        JSON json_mesh = json_meshes[i];
        JSON primitives = json_mesh["primitives"];

        MeshGroup mesh_group = {};
        mesh_group.start = meshes.len;
        mesh_group.count = primitives.array_len();

        for (u32 j = 0; j < mesh_group.count; ++j)
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

            f32* pos_accessor_memory  = (f32*)pos_accessor.get_memory(buffers, buffer_views);
            f32* norm_accessor_memory = (f32*)norm_accessor.get_memory(buffers, buffer_views);
            f32* uv_accessor_memory   = (f32*)uv_accessor.get_memory(buffers, buffer_views);

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

            void* indices_accessor_memory = indices_accessor.get_memory(buffers, buffer_views);

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

            RDMesh mesh = rd_create_mesh(renderer, upload_context, vertex_data, vertex_count, index_data, index_count);
            meshes.push(mesh);
            
            scratch->restore();
        }

        mesh_groups[i] = mesh_group;
    }

    JSON json_nodes = root["nodes"];
    Node* nodes = scratch->push_array<Node>(json_nodes.array_len());
    for (u32 i = 0; i < json_nodes.array_len(); ++i)
    {
        JSON json_node = json_nodes[i];
        Node node = {};
        
        if (json_node.has("children"))
        {
            JSON children = json_node["children"];

            node.num_children = children.array_len(); 
            node.children = scratch->push_array<u32>(node.num_children);

            for (u32 j = 0; j < children.array_len(); ++j) {
                node.children[j] = children[j].as_int();
            }
        }

        if (json_node.has("matrix"))
        {
            JSON json_matrix = json_node["matrix"];
            assert(json_matrix.array_len() == 16);

            f32 matrix_as_floats[16];

            for (u32 j = 0; j < 16; ++j) {
                JSON component = json_matrix[j];
                matrix_as_floats[j] = component.type == JSON_FLOAT ? component.as_float() : (f32)component.as_int();
            }

            node.transform = XMMATRIX(matrix_as_floats);
        }
        else {
            XMVECTOR translation = {};
            XMVECTOR rotation = XMQuaternionIdentity();
            XMVECTOR scaling = { 1.0f, 1.0f, 1.0f };

            if (json_node.has("translation")) {
                translation = json_to_xmvector(json_node["translation"]);
            }

            if (json_node.has("rotation")) {
                rotation = json_to_xmvector(json_node["rotation"]);
            }

            if (json_node.has("scale")) {
                scaling = json_to_xmvector(json_node["scale"]);
            }

            XMMATRIX scaling_matrix = XMMatrixScalingFromVector(scaling);
            XMMATRIX rotation_matrix = XMMatrixRotationQuaternion(rotation);
            XMMATRIX translation_matrix = XMMatrixTranslationFromVector(translation);

            node.transform = scaling_matrix * rotation_matrix * translation_matrix;
        }

        if (json_node.has("mesh")) {
            int mesh_group_index = json_node["mesh"].as_int();
            node.mesh_group = mesh_groups[mesh_group_index];
        }

        nodes[i] = node;
    }

    JSON scenes = root["scenes"];
    Vec<RDMeshInstance> instances = {};
    for (u32 i = 0; i < scenes.array_len(); ++i)
    {
        JSON scene = scenes[i];
        JSON scene_nodes = scene["nodes"];
        for (u32 j = 0; j < scene_nodes.array_len(); ++j)
        {
            int node_index = scene_nodes[j].as_int();
            Node node = nodes[node_index];
            process_node(node, nodes, &meshes, XMMatrixIdentity(), &instances);
        }
    }
    
    GLTFResult result = {};
    result.num_meshes = meshes.len;
    result.meshes = arena->push_vec_contents(meshes);
    result.num_instances = instances.len;
    result.instances = arena->push_vec_contents(instances);

    meshes.free();
    instances.free();

    return result;
}
