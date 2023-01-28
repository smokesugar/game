#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "platform.h"
#include "renderer.h"
#include "gltf.h"

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
    }

    return DefWindowProcA(window, msg, w_param, l_param);
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

    Input input;
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&input);

    Renderer* renderer = rd_init(&arena, window);

    GLTFResult gltf_result = gltf_load(&arena, renderer, "models/car/scene.gltf");

    for (u32 i = 0; i < gltf_result.num_instances; ++i) {
        RDMeshInstance* instance = gltf_result.instances + i;
        instance->transform = instance->transform * XMMatrixScaling(0.01f, 0.01f, 0.01f);
    }

    Arena frame_arena = arena.sub_arena(1024 * 1024 * 10);

    while (true) {
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

        rd_render(renderer, gltf_result.num_instances, gltf_result.instances);
    }

    #if _DEBUG 
    {
        for (u32 i = 0; i < gltf_result.num_meshes; ++i) {
            rd_free_mesh(renderer, gltf_result.meshes[i]);
        }

        rd_free(renderer);
    }
    #endif

    return 0;
}