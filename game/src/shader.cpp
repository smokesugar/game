#include <atlbase.h>
#include <dxc/dxcapi.h>
#include <wrl/client.h>

#include "shader.h"
#include "platform.h"

Shader compile_shader(Arena* arena, const char* path, const char* entry_point, const char* target) {
    using namespace Microsoft::WRL;

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler> compiler;
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

    ComPtr<IDxcIncludeHandler> include_handler;
    utils->CreateDefaultIncludeHandler(&include_handler);

    wchar_t wide_path[512];
    wchar_t wide_entry[512];
    wchar_t wide_target[512];
    
    swprintf_s(wide_path, 512, L"%hs", path);
    swprintf_s(wide_entry, 512, L"%hs", entry_point);
    swprintf_s(wide_target, 512, L"%hs", target);

    ComPtr<IDxcBlobEncoding> source_blob;
    utils->LoadFile(wide_path, 0, &source_blob);

    if (!source_blob) {
        pf_debug_log("Failed to load shader: %s\n", path);
        return {};
    }

    LPCWSTR args[] = {
        L"-Zs", // Generate debug information
    };

    ComPtr<IDxcOperationResult> result;
    compiler->Compile(source_blob.Get(), wide_path, wide_entry, wide_target, args, ARRAY_LEN(args), 0, 0, include_handler.Get(), &result);

    ComPtr<IDxcBlobEncoding> errors;
    result->GetErrorBuffer(&errors);

    if (errors && errors->GetBufferSize() != 0) {
        ComPtr<IDxcBlobUtf8> errors_u8;
        errors->QueryInterface(IID_PPV_ARGS(&errors_u8));
        pf_debug_log("Errors in shader compilation:\n%s\n", (char*)errors_u8->GetStringPointer());
    }

    HRESULT status;
    result->GetStatus(&status);

    if (FAILED(status)) {
        pf_debug_log("Failed shader compilation.\n");
        return {};
    }

    ComPtr<IDxcBlob> shader_blob;
    result->GetResult(&shader_blob);
    assert(shader_blob);

    Shader shader = {};
    shader.len = shader_blob->GetBufferSize();
    shader.memory = arena->push(shader.len);
    memcpy(shader.memory, shader_blob->GetBufferPointer(), shader.len);

    return shader;
}
