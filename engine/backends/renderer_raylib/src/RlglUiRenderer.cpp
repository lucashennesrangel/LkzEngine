#include "RlglUiRenderer.hpp"

#include <algorithm>
#include <cstdint>

#include <RmlUi/Core.h>
#include <raylib.h>
#include <rlgl.h>
#include <spdlog/spdlog.h>

namespace lkz::raylib_detail {

namespace {

// Geometria da RmlUi: posição em pixels (origem no canto superior esquerdo), cor com
// alfa pré-multiplicado e coordenada de textura. "vertex*" são os nomes que a raylib
// liga aos locais padrão (0 posição, 1 textura, 3 cor).
constexpr const char* uiVertexShader = R"(#version 330
in vec2 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;

uniform mat4 projection;
uniform mat4 transform;
uniform vec2 translation;

out vec2 uv;
out vec4 color;

void main() {
    uv = vertexTexCoord;
    color = vertexColor;
    gl_Position = projection * transform * vec4(vertexPosition + translation, 0.0, 1.0);
}
)";

constexpr const char* uiFragmentShader = R"(#version 330
in vec2 uv;
in vec4 color;

uniform sampler2D texture0;

out vec4 finalColor;

void main() {
    finalColor = texture(texture0, uv) * color;
}
)";

Matrix toRaylib(const float* columnMajor) {
    Matrix m{};
    float* fields[16] = {&m.m0, &m.m1, &m.m2,  &m.m3,  &m.m4,  &m.m5,  &m.m6,  &m.m7,
                         &m.m8, &m.m9, &m.m10, &m.m11, &m.m12, &m.m13, &m.m14, &m.m15};
    for (int i = 0; i < 16; ++i) {
        *fields[i] = columnMajor[i];
    }
    return m;
}

/// RenderInterface da RmlUi sobre o rlgl (OpenGL 3.3 da raylib): geometria em VAOs,
/// texturas da raylib, recorte por scissor e transformações por matriz.
} // namespace

class RlglUiRenderer::Renderer final : public ui_rmlui::UiRenderer {
public:
    bool initialize() override {
        shader_ = LoadShaderFromMemory(uiVertexShader, uiFragmentShader);
        projectionLocation_ = GetShaderLocation(shader_, "projection");
        transformLocation_ = GetShaderLocation(shader_, "transform");
        translationLocation_ = GetShaderLocation(shader_, "translation");
        textureLocation_ = GetShaderLocation(shader_, "texture0");
        return IsShaderValid(shader_);
    }

    void release() override {
        if (IsShaderValid(shader_)) {
            UnloadShader(shader_);
        }
        shader_ = {};
    }

    void beginFrame(int width, int height) override {
        height_ = height;
        rlDrawRenderBatchActive(); // o que a raylib tiver no lote 2D vai antes
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        rlEnableColorBlend();
        rlSetBlendFactors(RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD); // cores pré-multiplicadas
        rlSetBlendMode(RL_BLEND_CUSTOM);
        rlEnableShader(shader_.id);
        const float projection[16] = {2.0f / static_cast<float>(width), 0.0f, 0.0f, 0.0f,
                                      0.0f, -2.0f / static_cast<float>(height), 0.0f, 0.0f,
                                      0.0f, 0.0f, -1.0f, 0.0f,
                                      -1.0f, 1.0f, 0.0f, 1.0f};
        rlSetUniformMatrix(projectionLocation_, toRaylib(projection));
        SetTransform(nullptr);
        const int unit = 0;
        rlSetUniform(textureLocation_, &unit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
        rlActiveTextureSlot(0);
    }

    void endFrame() override {
        rlDisableScissorTest();
        rlDisableVertexArray();
        rlDisableTexture();
        rlDisableShader();
        rlSetBlendMode(RL_BLEND_ALPHA);
        rlEnableBackfaceCulling();
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto geometry = std::make_unique<Geometry>();
        geometry->vao = rlLoadVertexArray();
        rlEnableVertexArray(geometry->vao);
        const bool indexed = vertices.size() <= 65535; // o rlgl desenha índices de 16 bits
        if (indexed) {
            geometry->vbo = rlLoadVertexBuffer(vertices.data(), static_cast<int>(vertices.size() * sizeof(Rml::Vertex)), false);
            std::vector<unsigned short> shortIndices(indices.begin(), indices.end());
            geometry->ebo = rlLoadVertexBufferElement(shortIndices.data(),
                                                      static_cast<int>(shortIndices.size() * sizeof(unsigned short)), false);
        } else {
            std::vector<Rml::Vertex> expanded;
            expanded.reserve(indices.size());
            for (const int index : indices) {
                expanded.push_back(vertices[static_cast<std::size_t>(index)]);
            }
            geometry->vbo = rlLoadVertexBuffer(expanded.data(), static_cast<int>(expanded.size() * sizeof(Rml::Vertex)), false);
        }
        geometry->count = static_cast<int>(indices.size());
        geometry->indexed = indexed;
        constexpr int stride = sizeof(Rml::Vertex);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, 2, RL_FLOAT, false, stride,
                             static_cast<int>(offsetof(Rml::Vertex, position)));
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR, 4, RL_UNSIGNED_BYTE, true, stride,
                             static_cast<int>(offsetof(Rml::Vertex, colour)));
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, 2, RL_FLOAT, false, stride,
                             static_cast<int>(offsetof(Rml::Vertex, tex_coord)));
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD);
        rlDisableVertexArray();
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        const auto* geometry = reinterpret_cast<const Geometry*>(handle);
        const float offset[2] = {translation.x, translation.y};
        rlSetUniform(translationLocation_, offset, RL_SHADER_UNIFORM_VEC2, 1);
        rlEnableTexture(texture != 0 ? static_cast<unsigned int>(texture) : rlGetTextureIdDefault());
        rlEnableVertexArray(geometry->vao);
        if (geometry->indexed) {
            rlDrawVertexArrayElements(0, geometry->count, nullptr);
        } else {
            rlDrawVertexArray(0, geometry->count);
        }
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        std::unique_ptr<Geometry> geometry{reinterpret_cast<Geometry*>(handle)};
        rlUnloadVertexArray(geometry->vao);
        rlUnloadVertexBuffer(geometry->vbo);
        if (geometry->ebo != 0) {
            rlUnloadVertexBuffer(geometry->ebo);
        }
    }

    /// Imagem de arquivo (PNG, JPG...), com alfa pré-multiplicado como a RmlUi espera.
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        Image image = LoadImage(source.c_str());
        if (!IsImageValid(image)) {
            spdlog::warn("Interface: não foi possível carregar a imagem '{}'", source);
            return 0;
        }
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        auto* pixels = static_cast<std::uint8_t*>(image.data);
        const auto count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned alpha = pixels[i * 4 + 3];
            for (std::size_t c = 0; c < 3; ++c) {
                pixels[i * 4 + c] = static_cast<std::uint8_t>(pixels[i * 4 + c] * alpha / 255u);
            }
        }
        Texture2D texture = LoadTextureFromImage(image);
        UnloadImage(image);
        GenTextureMipmaps(&texture);
        SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
        dimensions = {texture.width, texture.height};
        return texture.id;
    }

    /// Textura gerada pela RmlUi (ex.: o atlas das letras), já pré-multiplicada.
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        const unsigned int id = rlLoadTexture(source.data(), dimensions.x, dimensions.y,
                                              PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
        rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
        return id;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override { rlUnloadTexture(static_cast<unsigned int>(texture)); }

    void EnableScissorRegion(bool enable) override {
        if (enable) {
            rlEnableScissorTest();
        } else {
            rlDisableScissorTest();
        }
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        // OpenGL conta o y de baixo para cima.
        rlScissor(region.Left(), height_ - (region.Top() + region.Height()), region.Width(), region.Height());
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        const Rml::Matrix4f matrix = transform != nullptr ? *transform : Rml::Matrix4f::Identity();
        rlSetUniformMatrix(transformLocation_, toRaylib(matrix.data()));
    }

private:
    struct Geometry {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        unsigned int ebo = 0;
        int count = 0;
        bool indexed = true;
    };

    Shader shader_{};
    int projectionLocation_ = -1;
    int transformLocation_ = -1;
    int translationLocation_ = -1;
    int textureLocation_ = -1;
    int height_ = 0;
};

RlglUiRenderer::RlglUiRenderer()
    : renderer_{std::make_unique<Renderer>()} {}

RlglUiRenderer::~RlglUiRenderer() = default;

ui_rmlui::UiRenderer& RlglUiRenderer::get() {
    return *renderer_;
}

} // namespace lkz::raylib_detail
