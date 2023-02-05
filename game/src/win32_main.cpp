#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "platform.h"
#include "renderer.h"
#include "gltf.h"
#include "maps.h"

static Arena scratch_arenas[2];

static i64 counter_start;
static i64 counter_freq;

static char* format_buf(Arena* arena, const char* fmt, va_list ap) {
    int len = vsnprintf(0, 0, fmt, ap);

    char* buf = arena->push_array<char>(len + 1);
    vsnprintf(buf, len + 1, fmt, ap);

    return buf;
}

void pf_msg_box(const char* fmt, ...) {
    Scratch scratch = get_scratch(0);

    va_list ap;
    va_start(ap, fmt);

    char* buf = format_buf(scratch.arena, fmt, ap);
    MessageBoxA(0, buf, "Game", 0);

    va_end(ap);
}

void pf_debug_log(const char* fmt, ...) {
    Scratch scratch = get_scratch(0);

    va_list ap;
    va_start(ap, fmt);

    char* buf = format_buf(scratch.arena, fmt, ap);
    OutputDebugStringA(buf);

    va_end(ap);
}

f32 pf_time() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    i64 delta = now.QuadPart - counter_start;
    f64 time = (f64)delta/(f64)counter_freq;

    return (f32)time;
}

FileContents pf_load_file(Arena* arena, const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    assert(file != INVALID_HANDLE_VALUE);

    LARGE_INTEGER file_size;
    GetFileSizeEx(file, &file_size);

    void* memory = arena->push(file_size.QuadPart + 1);
    DWORD bytes_read = 0;
    ReadFile(file, memory, (DWORD)file_size.QuadPart, &bytes_read, 0);
    assert(bytes_read == file_size.QuadPart);
    ((char*)memory)[file_size.QuadPart] = 0;
    
    CloseHandle(file);

    FileContents result;
    result.memory = memory;
    result.size = file_size.QuadPart;

    return result;
}

Scratch get_scratch(Arena* conflict) {
    Arena* arena = 0;

    for (int i = 0; i < ARRAY_LEN(scratch_arenas); ++i) {
        if (scratch_arenas + i != conflict) {
            arena = scratch_arenas + i;
            break;
        }
    }

    return Scratch(arena, arena->allocated);
}

struct Input {
    bool window_closed;
    f32 raw_mouse_dx;
    f32 raw_mouse_dy;
    bool keys_pressed[256];
    bool keys_released[256];

    void reset() {
        memset(this, 0, sizeof(*this));
    }
};

static LRESULT window_proc(HWND window, UINT msg, WPARAM w_param, LPARAM l_param) {
    Input* input = (Input*)GetWindowLongPtrA(window, GWLP_USERDATA);

    switch (msg) {
        case WM_CLOSE:
            input->window_closed = true;
            break;

        case WM_INPUT: {
            RAWINPUT raw_input;

            u32 raw_input_size = sizeof(raw_input);
            GetRawInputData((HRAWINPUT)l_param, RID_INPUT, &raw_input, &raw_input_size, sizeof(RAWINPUTHEADER));

            if (raw_input.header.dwType == RIM_TYPEMOUSE) {
                input->raw_mouse_dx += (f32)raw_input.data.mouse.lLastX;
                input->raw_mouse_dy += (f32)raw_input.data.mouse.lLastY;
            }
        } break;

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            input->keys_pressed[(int)w_param] = true;
            break;

        case WM_SYSKEYUP:
        case WM_KEYUP:
            input->keys_released[(int)w_param] = true;
            break;

        case WM_LBUTTONUP:
            input->keys_released[VK_LBUTTON] = true;
            break;

        case WM_LBUTTONDOWN:
            input->keys_pressed[VK_LBUTTON] = true;
            break;

        case WM_MBUTTONUP:
            input->keys_released[VK_MBUTTON] = true;
            break;

        case WM_MBUTTONDOWN:
            input->keys_pressed[VK_MBUTTON] = true;
            break;

        case WM_RBUTTONUP:
            input->keys_released[VK_RBUTTON] = true;
            break;

        case WM_RBUTTONDOWN:
            input->keys_pressed[VK_RBUTTON] = true;
            break;
    }

    return DefWindowProcA(window, msg, w_param, l_param);
}

static bool key_down(int key) {
    return GetKeyState(key) & (1 << 16);
}

int CALLBACK WinMain(HINSTANCE h_instance, HINSTANCE, LPSTR, int) {
    LARGE_INTEGER counter_start_result;
    QueryPerformanceCounter(&counter_start_result);
    counter_start = counter_start_result.QuadPart;

    LARGE_INTEGER counter_freq_result;
    QueryPerformanceFrequency(&counter_freq_result);
    counter_freq = counter_freq_result.QuadPart;

    u64 main_mem_size = 1024 * 1024 * 1024;
    void* main_mem = VirtualAlloc(0, main_mem_size, MEM_COMMIT, PAGE_READWRITE);
    Arena arena = arena_init(main_mem, main_mem_size);

    for (int i = 0; i < ARRAY_LEN(scratch_arenas); ++i) {
        u64 scratch_mem_size = 1024 * 1024 * 1024;
        void* mem = VirtualAlloc(0, scratch_mem_size, MEM_COMMIT, PAGE_READWRITE);
        scratch_arenas[i] = arena_init(mem, scratch_mem_size);
    }

    HashMap<int, int> hash_map = {};

    for (int i = 0; i < 1024; ++i) {
        hash_map.insert(i, i);
    }

    for (int i = 0; i < 1024; ++i) {
        assert(hash_map[i] == i);
    }

    WNDCLASSA wc = {};
    wc.hInstance = h_instance;
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "smokesugar";

    RegisterClassA(&wc);

    HWND window = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Game",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        0,
        0,
        h_instance,
        0
    );

    ShowWindow(window, SW_MAXIMIZE);
    SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)window_proc);

    RAWINPUTDEVICE raw_input_mouse = {};
    raw_input_mouse.usUsagePage = 0x01;
    raw_input_mouse.usUsage = 0x02;
    raw_input_mouse.hwndTarget = window;

    RegisterRawInputDevices(&raw_input_mouse, 1, sizeof(RAWINPUTDEVICE));

    Input input;
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&input);

    Renderer* renderer = rd_init(&arena, window);
    RDUploadContext* upload_context = rd_open_upload_context(renderer);

    GLTFResult gltf_result = gltf_load(&arena, renderer, upload_context, "models/test_scene/scene.gltf");

    for (u32 i = 0; i < gltf_result.num_instances; ++i) {
        RDMeshInstance* instance = gltf_result.instances + i;
        instance->transform = instance->transform * XMMatrixScaling(0.5f, 0.5f, 0.5f);
    }

    RDUploadStatus* upload_status = rd_submit_upload_context(renderer, upload_context);

    Arena frame_arena = arena.sub_arena(1024 * 1024 * 10);

    f32 camera_yaw = 0.0f;
    f32 camera_pitch = 0.0f;
    XMVECTOR camera_position = {-3.0f, 2.0f, 5.0f};
    XMVECTOR camera_velocity = {};

    f32 last_time = pf_time();

    f32 accumulator = 0.0f;
    int faccumulator = 0;

    while (true) {
        f32 now = pf_time();
        f32 delta_time = now - last_time;
        last_time = now;

        accumulator += delta_time;
        faccumulator++;

        if (accumulator > 2.0f) {
            f32 dt = accumulator/(f32)faccumulator;
            pf_debug_log("FPS: %f\n", 1.0f/dt);
            accumulator = 0.0f;
            faccumulator = 0;
        }

        input.reset();
        frame_arena.reset();
        
        MSG msg;
        while (PeekMessageA(&msg, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (input.window_closed) {
            break;
        }

        if (input.keys_pressed[VK_RBUTTON]) {
            ShowCursor(false);

            RECT clip_rect = {};
            GetClientRect(window, &clip_rect);
            ClientToScreen(window, (POINT*)&clip_rect.left);
            ClientToScreen(window, (POINT*)&clip_rect.right);
            ClipCursor(&clip_rect);
        }

        if (input.keys_released[VK_RBUTTON]) {
            ShowCursor(true);
            ClipCursor(0);
        }

        bool camera_controlled = key_down(VK_RBUTTON);

        if (camera_controlled) {
            f32 mouse_look_sensitivity = 0.001f;

            camera_yaw   -= input.raw_mouse_dx * mouse_look_sensitivity;
            camera_pitch -= input.raw_mouse_dy * mouse_look_sensitivity;

            if (camera_pitch > PI32 * 0.49f) {
                camera_pitch = PI32 * 0.49f;
            }

            if (camera_pitch < -PI32 * 0.49f) {
                camera_pitch = -PI32 * 0.49f;
            }
        }

        XMMATRIX camera_rotation = XMMatrixRotationRollPitchYaw(camera_pitch, camera_yaw, 0.0f);

        if (camera_controlled) {
            XMVECTOR front = XMVector3Normalize(XMVector3Transform({0.0f, 0.0f, -1.0f, 0.0f}, camera_rotation));
            XMVECTOR up = {0.0f, 1.0f, 0.0f};
            XMVECTOR right = XMVector3Cross(front, up);

            XMVECTOR acceleration = {};
            
            if (key_down('W')) {
                acceleration += front;
            }

            if (key_down('S')) {
                acceleration -= front;
            }

            if (key_down('A')) {
                acceleration -= right;
            }

            if (key_down('D')) {
                acceleration += right;
            }

            if (key_down(VK_SPACE)) {
                acceleration += up;
            }

            if (key_down(VK_LSHIFT)) {
                acceleration -= up;
            }

            f32 acceleration_speed = 100.0f;
            if (XMVectorGetX(XMVector3Length(acceleration)) > 0.0f) {
                camera_velocity += XMVector3Normalize(acceleration) * acceleration_speed * delta_time;
            }
        }

        f32 friction = 10.0f;
        camera_velocity -= camera_velocity * friction * delta_time;
        camera_position += camera_velocity * delta_time;
        XMMATRIX camera_translation = XMMatrixTranslationFromVector(camera_position);

        RDCamera camera = {};
        camera.transform = camera_rotation * camera_translation;
        camera.vertical_fov = PI32 * 0.5f;

        RDPointLight point_lights[] = {
            {{cosf(now), 0.4f, sinf(now) + 5.0f}, {0.1f, 0.1f, 1.0f}},
            {{cosf(now + PI32), 0.4f, sinf(now + PI32) + 5.0f}, {1.0f, 0.1f, 0.1f}}
        };

        RDDirectionalLight directional_lights[] = {
            {{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        };

        RDRenderInfo render_info = {};
        render_info.camera = &camera;
        render_info.num_point_lights = ARRAY_LEN(point_lights);
        render_info.num_directional_lights = ARRAY_LEN(directional_lights);
        render_info.point_lights = point_lights;
        render_info.directional_lights = directional_lights;

        if (rd_upload_status_finished(renderer, upload_status)) {
            render_info.num_instances = gltf_result.num_instances;
            render_info.instances = gltf_result.instances;
        }

        rd_render(renderer, &render_info);
    }

    #if _DEBUG 
    {
        for (u32 i = 0; i < gltf_result.num_textures; ++i) {
            rd_free_texture(renderer, gltf_result.textures[i]);
        }

        for (u32 i = 0; i < gltf_result.num_meshes; ++i) {
            rd_free_mesh(renderer, gltf_result.meshes[i]);
        }

        rd_free(renderer);
    }
    #endif

    return 0;
}