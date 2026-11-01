// Impostores octaédricos (Ryan Brucks, "Octahedral Impostors", 2018; hoje no Unreal).
//
// O hemisfério de cima (de onde a câmera de um jogo no chão vê os objetos) é mapeado
// num quadrado pelo octaedro: a direção d vira p = d.xz / (|d.x| + |d.y| + |d.z|),
// um losango, girado 45 graus para encher o quadrado [-1, 1]². O quadrado é dividido
// em N x N vistas; cada vista é o objeto fotografado (projeção ortográfica da esfera
// envolvente) da direção do seu centro. Duas fotos por vista: a cor e a normal no
// espaço do objeto, para a luz ser aplicada na hora.
//
// Na hora de desenhar, cada cópia é um quadrado virado para a câmera. O vertex shader
// leva a direção da câmera ao espaço do objeto (o giro da cópia conta), acha o lugar
// dela no quadrado das vistas, e o fragment shader mistura as 4 vistas vizinhas
// (bilinear), o que evita "pulos" quando a câmera gira. Pixels sem objeto são
// descartados (alfa < 0.5).

#include "Impostor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <rlgl.h>
#include <spdlog/spdlog.h>

namespace lkz::raylib_detail {

namespace {

constexpr int dilatePasses = 16; // pixels de cor estendida em volta do objeto (para os mipmaps)

// Fotografia de uma vista: modo 0 = cor (sem luz), 1 = normal do objeto em 0..1.
constexpr const char* bakeVertexShader = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;

uniform mat4 mvp;

out vec2 uv;
out vec3 normal;

void main() {
    uv = vertexTexCoord;
    normal = vertexNormal;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

constexpr const char* bakeFragmentShader = R"(#version 330
in vec2 uv;
in vec3 normal;

uniform sampler2D texture0;
uniform vec4 materialColor;
uniform int mode;

out vec4 color;

void main() {
    vec4 albedo = texture(texture0, uv) * materialColor;
    if (albedo.a < 0.5) {
        discard;
    }
    if (mode == 0) {
        color = vec4(albedo.rgb, 1.0);
    } else {
        vec3 n = normalize(normal) * (gl_FrontFacing ? 1.0 : -1.0);
        color = vec4(n * 0.5 + 0.5, 1.0);
    }
}
)";

/// Quadrado [-1, 1]² -> direção no hemisfério de cima (Y para cima).
glm::vec3 hemiOctDecode(glm::vec2 uv) {
    const glm::vec2 p{(uv.x + uv.y) * 0.5f, (uv.x - uv.y) * 0.5f};
    return glm::normalize(glm::vec3{p.x, 1.0f - std::abs(p.x) - std::abs(p.y), p.y});
}

/// Eixos do quadrado virado para `toCamera` (a mesma conta do vertex shader).
void billboardBasis(glm::vec3 toCamera, glm::vec3& right, glm::vec3& up) {
    right = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, toCamera);
    right = glm::length(right) < 1e-4f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::normalize(right);
    up = glm::cross(toCamera, right);
}

/// Estende a cor das bordas do objeto para os pixels vazios (alfa continua 0): sem
/// isso, os mipmaps misturam o objeto com o fundo preto e ele ganha contorno escuro.
void dilate(Image& image, int passes) {
    const int width = image.width;
    const int height = image.height;
    auto* pixels = static_cast<std::uint8_t*>(image.data);
    std::vector<std::uint8_t> filled(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < filled.size(); ++i) {
        filled[i] = pixels[i * 4 + 3] > 0 ? 1 : 0;
    }
    std::vector<std::size_t> grown;
    for (int pass = 0; pass < passes; ++pass) {
        grown.clear();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                if (filled[index] != 0) {
                    continue;
                }
                int sum[3] = {0, 0, 0};
                int neighbours = 0;
                const std::array<std::array<int, 2>, 4> offsets{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
                for (const auto& [dx, dy] : offsets) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const auto neighbour = static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) +
                                           static_cast<std::size_t>(nx);
                    if (filled[neighbour] == 1) {
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += pixels[neighbour * 4 + static_cast<std::size_t>(c)];
                        }
                        ++neighbours;
                    }
                }
                if (neighbours > 0) {
                    for (int c = 0; c < 3; ++c) {
                        pixels[index * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(sum[c] / neighbours);
                    }
                    grown.push_back(index);
                }
            }
        }
        if (grown.empty()) {
            break;
        }
        for (const auto index : grown) {
            filled[index] = 1; // só no fim da passada: cresce um pixel por vez
        }
    }
}

} // namespace

const char* const impostorVertexShader = R"(#version 330
in vec3 vertexPosition;
in mat4 instanceTransform;
in vec4 instanceColor;

uniform mat4 viewProjection;
uniform vec3 cameraPosition;
uniform vec4 impostorBounds; // xyz = centro, w = raio (espaço do objeto)
uniform float impostorFrames;

out vec2 localUv;
flat out vec2 frameBase;
flat out vec2 frameBlend;
flat out mat3 objectRotation;
out vec4 fragColor;

vec2 hemiOctEncode(vec3 d) {
    vec2 p = d.xz / (abs(d.x) + abs(d.y) + abs(d.z));
    return vec2(p.x + p.y, p.x - p.y);
}

void main() {
    mat3 m = mat3(instanceTransform);
    float scale = max(length(m[0]), max(length(m[1]), length(m[2])));
    mat3 rotation = mat3(normalize(m[0]), normalize(m[1]), normalize(m[2]));
    vec3 center = (instanceTransform * vec4(impostorBounds.xyz, 1.0)).xyz;

    // Câmera no espaço do objeto, limitada ao hemisfério fotografado.
    vec3 toCamera = transpose(rotation) * normalize(cameraPosition - center);
    toCamera = normalize(vec3(toCamera.x, max(toCamera.y, 0.0), toCamera.z) + vec3(0.0, 1e-4, 0.0));
    vec3 right = cross(vec3(0.0, 1.0, 0.0), toCamera);
    right = length(right) < 1e-4 ? vec3(1.0, 0.0, 0.0) : normalize(right);
    vec3 up = cross(toCamera, right);

    vec2 grid = (hemiOctEncode(toCamera) * 0.5 + 0.5) * impostorFrames - 0.5;
    frameBase = clamp(floor(grid), vec2(0.0), vec2(impostorFrames - 2.0));
    frameBlend = clamp(grid - frameBase, 0.0, 1.0);
    localUv = vertexPosition.xy * 0.5 + 0.5;
    objectRotation = rotation;
    fragColor = instanceColor;

    vec3 offset = rotation * (right * vertexPosition.x + up * vertexPosition.y) * impostorBounds.w * scale;
    gl_Position = viewProjection * vec4(center + offset, 1.0);
}
)";

const char* const impostorFragmentShader = R"(#version 330
in vec2 localUv;
flat in vec2 frameBase;
flat in vec2 frameBlend;
flat in mat3 objectRotation;
in vec4 fragColor;

uniform sampler2D texture0;    // atlas de cor
uniform sampler2D normalAtlas; // atlas de normais
uniform float impostorFrames;
uniform vec3 lightDirection;
uniform vec4 materialColor; // cor de depuração do nível (branco = normal)

out vec4 finalColor;

vec4 frames(sampler2D atlas) {
    vec2 f = frameBlend;
    return texture(atlas, (frameBase + localUv) / impostorFrames) * (1.0 - f.x) * (1.0 - f.y)
         + texture(atlas, (frameBase + vec2(1.0, 0.0) + localUv) / impostorFrames) * f.x * (1.0 - f.y)
         + texture(atlas, (frameBase + vec2(0.0, 1.0) + localUv) / impostorFrames) * (1.0 - f.x) * f.y
         + texture(atlas, (frameBase + vec2(1.0, 1.0) + localUv) / impostorFrames) * f.x * f.y;
}

void main() {
    vec4 albedo = frames(texture0);
    if (albedo.a < 0.5) {
        discard;
    }
    vec3 normal = normalize(objectRotation * (frames(normalAtlas).xyz * 2.0 - 1.0));
    float diffuse = max(dot(normal, -lightDirection), 0.0);
    finalColor = vec4(albedo.rgb * fragColor.rgb * materialColor.rgb * (0.35 + 0.65 * diffuse), fragColor.a);
}
)";

GpuImpostor bakeImpostor(const std::vector<GpuPart>& parts, const Bounds& bounds, int frames, int frameSize) {
    GpuImpostor impostor;
    frames = std::clamp(frames, 2, 16);
    frameSize = std::clamp(frameSize, 16, 1024);
    if (bounds.radius <= 0.0f || parts.empty()) {
        return impostor;
    }
    Shader shader = LoadShaderFromMemory(bakeVertexShader, bakeFragmentShader);
    if (!IsShaderValid(shader)) {
        spdlog::warn("RaylibRenderBackend: falha ao compilar o shader de fotografia dos impostores");
        return impostor;
    }
    const int mvpLocation = GetShaderLocation(shader, "mvp");
    const int modeLocation = GetShaderLocation(shader, "mode");
    const int colorLocation = GetShaderLocation(shader, "materialColor");
    const int textureLocation = GetShaderLocation(shader, "texture0");

    const int size = frames * frameSize;
    const float radius = bounds.radius;
    const glm::mat4 projection = glm::ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 3.5f);
    std::array<RenderTexture2D, 2> targets{LoadRenderTexture(size, size), LoadRenderTexture(size, size)};

    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling(); // folhas e placas finas: os dois lados contam
    rlEnableDepthTest();
    for (int pass = 0; pass < 2; ++pass) {
        rlEnableFramebuffer(targets[static_cast<std::size_t>(pass)].id);
        rlViewport(0, 0, size, size);
        if (pass == 0) {
            rlClearColor(0, 0, 0, 0);
        } else {
            rlClearColor(128, 128, 255, 0);
        }
        rlClearScreenBuffers();
        rlEnableShader(shader.id);
        rlSetUniform(modeLocation, &pass, RL_SHADER_UNIFORM_INT, 1);
        const int textureUnit = 0;
        rlSetUniform(textureLocation, &textureUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
        rlActiveTextureSlot(0);

        for (int j = 0; j < frames; ++j) {
            for (int i = 0; i < frames; ++i) {
                const glm::vec2 uv{(static_cast<float>(i) + 0.5f) / static_cast<float>(frames) * 2.0f - 1.0f,
                                   (static_cast<float>(j) + 0.5f) / static_cast<float>(frames) * 2.0f - 1.0f};
                const glm::vec3 toCamera = hemiOctDecode(uv);
                glm::vec3 right;
                glm::vec3 up;
                billboardBasis(toCamera, right, up);
                const glm::vec3 eye = bounds.center + toCamera * radius * 2.0f;
                glm::mat4 view{1.0f};
                for (int axis = 0; axis < 3; ++axis) {
                    view[axis][0] = right[axis];
                    view[axis][1] = up[axis];
                    view[axis][2] = toCamera[axis];
                }
                view[3] = glm::vec4{-glm::dot(right, eye), -glm::dot(up, eye), -glm::dot(toCamera, eye), 1.0f};

                rlViewport(i * frameSize, j * frameSize, frameSize, frameSize);
                rlSetUniformMatrix(mvpLocation, toRaylib(projection * view));
                for (const auto& part : parts) {
                    if (part.mesh.vertexCount == 0) {
                        continue;
                    }
                    rlEnableTexture(part.texture.id != 0 ? part.texture.id : rlGetTextureIdDefault());
                    rlSetUniform(colorLocation, &part.color, RL_SHADER_UNIFORM_VEC4, 1);
                    rlEnableVertexArray(part.mesh.vaoId);
                    if (part.mesh.indices != nullptr) {
                        rlDrawVertexArrayElements(0, part.mesh.triangleCount * 3, nullptr);
                    } else {
                        rlDrawVertexArray(0, part.mesh.vertexCount);
                    }
                }
            }
        }
        rlDisableVertexArray();
        rlDisableShader();
        rlDisableFramebuffer();
    }
    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
    UnloadShader(shader);

    // As texturas ficam; o framebuffer (e a profundidade dele) vai embora.
    for (auto& target : targets) {
        Image image = LoadImageFromTexture(target.texture);
        dilate(image, dilatePasses);
        UpdateTexture(target.texture, image.data);
        UnloadImage(image);
        GenTextureMipmaps(&target.texture);
        SetTextureFilter(target.texture, TEXTURE_FILTER_TRILINEAR);
        SetTextureWrap(target.texture, TEXTURE_WRAP_CLAMP);
        rlUnloadFramebuffer(target.id);
    }
    impostor.albedo = targets[0].texture;
    impostor.normal = targets[1].texture;
    impostor.frames = frames;
    impostor.bounds = bounds;
    return impostor;
}

void unloadImpostor(GpuImpostor& impostor) {
    if (impostor.albedo.id != 0) {
        UnloadTexture(impostor.albedo);
    }
    if (impostor.normal.id != 0) {
        UnloadTexture(impostor.normal);
    }
    impostor = {};
}

Mesh makeImpostorQuad() {
    Mesh mesh{};
    mesh.vertexCount = 4;
    mesh.triangleCount = 2;
    mesh.vertices = static_cast<float*>(RL_MALLOC(4 * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(RL_MALLOC(4 * 2 * sizeof(float)));
    mesh.normals = static_cast<float*>(RL_MALLOC(4 * 3 * sizeof(float)));
    mesh.indices = static_cast<unsigned short*>(RL_MALLOC(6 * sizeof(unsigned short)));
    const std::array<float, 12> positions{-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
    const std::array<float, 8> uvs{0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const std::array<unsigned short, 6> indices{0, 1, 2, 0, 2, 3};
    std::copy(positions.begin(), positions.end(), mesh.vertices);
    std::copy(uvs.begin(), uvs.end(), mesh.texcoords);
    for (int i = 0; i < 4; ++i) {
        mesh.normals[i * 3] = 0.0f;
        mesh.normals[i * 3 + 1] = 0.0f;
        mesh.normals[i * 3 + 2] = 1.0f;
    }
    std::copy(indices.begin(), indices.end(), mesh.indices);
    UploadMesh(&mesh, false);
    return mesh;
}

} // namespace lkz::raylib_detail
