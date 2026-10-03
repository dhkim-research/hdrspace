#include <nanogui/shader.h>
#include "shader_impl.h"

NAMESPACE_BEGIN(nanogui)

std::string Buffer::to_string() const {
    std::string result = "Buffer[type=";
    switch (type) {
        case VertexBuffer: result += "vertex"; break;
        case FragmentBuffer: result += "fragment"; break;
        case UniformBuffer: result += "uniform"; break;
        case IndexBuffer: result += "index"; break;
        default: result += "unknown"; break;
    }
    result += ", dtype=";
    result += type_name(dtype);
    result += ", shape=[";
    for (size_t i = 0; i < ndim; ++i) {
        result += std::to_string(shape[i]);
        if (i + 1 < ndim)
            result += ", ";
    }
    result += "]]";
    return result;
}

// Accessor methods
RenderPass *Shader::render_pass() { return p->render_pass; }
std::string_view Shader::name() const { return p->name; }
Shader::BlendMode Shader::blend_mode() const { return p->blend_mode; }

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
uint32_t Shader::shader_handle() const { return p->shader_handle; }
#elif defined(NANOGUI_USE_METAL)
void *Shader::pipeline_state() const { return p->pipeline_state; }
#endif

#if defined(NANOGUI_USE_OPENGL)
uint32_t Shader::vertex_array_handle() const { return p->vertex_array_handle; }
#endif

NAMESPACE_END(nanogui)
