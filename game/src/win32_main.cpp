#include <Windows.h>

#include "platform.h"
#include "renderer.h"

static Arena scratch_arenas[2];

void pf_msg_box(const char* msg) {
    MessageBoxA(0, msg, "Game", 0);
}

void pf_debug_log(const char* msg) {
    OutputDebugStringA(msg);
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

    while (true) {
        input.reset();
        
        MSG msg;
        while (PeekMessageA(&msg, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (input.window_closed) {
            break;
        }

        rd_render(renderer);
    }

    #if _DEBUG 
    rd_free(renderer);
    #endif

    return 0;
}