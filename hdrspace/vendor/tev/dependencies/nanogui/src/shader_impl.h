#pragma once

#include <nanogui/shader.h>
#include <tsl/robin_map.h>
#include <string>

NAMESPACE_BEGIN(nanogui)

enum BufferType {
    Unknown = 0,
    VertexBuffer,
    VertexTexture,
    VertexSampler,
    FragmentBuffer,
    FragmentTexture,
    FragmentSampler,
    UniformBuffer,
    IndexBuffer,
};

struct Buffer {
    void *buffer = nullptr;
    BufferType type = Unknown;
    VariableType dtype = VariableType::Invalid;
    int index = 0;
    size_t ndim = 0;
    size_t shape[3] { 0, 0, 0 };
    size_t size = 0;
    bool dirty = false;

    std::string to_string() const;
};


struct Shader::Impl {
    RenderPass* render_pass;
    std::string name;
    tsl::robin_map<std::string, Buffer, std::hash<std::string_view>, std::equal_to<>> buffers;
    Shader::BlendMode blend_mode;

    #if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
        uint32_t shader_handle = 0;
    #  if defined(NANOGUI_USE_OPENGL)
        uint32_t vertex_array_handle = 0;
        bool uses_point_size = false;
    #  endif
    #elif defined(NANOGUI_USE_METAL)
        void *pipeline_state;
    #endif
};

NAMESPACE_END(nanogui)
