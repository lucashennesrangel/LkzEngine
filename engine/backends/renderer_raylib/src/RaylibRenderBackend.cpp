#include "lkz/backends/RaylibRenderBackend.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <lkz/Asset/Gltf.hpp>
#include <raylib.h>
#include <rlgl.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "GpuTypes.hpp"
#include "Impostor.hpp"
#include "MeshSimplify.hpp"
#include "TerrainRenderer.hpp"
#include "RlglUiRenderer.hpp"

namespace lkz {

namespace {

using raylib_detail::Bounds;
using raylib_detail::Frustum;
using raylib_detail::GpuImpostor;
using raylib_detail::GpuPart;
using raylib_detail::toRaylib;

// GLSL 330: matriz de modelo e cor chegam por instância; textura da mesh (branca
// quando não há imagem); luz direcional + ambiente.
// "instanceTransform" é o nome que a raylib já liga a um local fixo (ocupa 4 locais).
constexpr const char* vertexShader = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in mat4 instanceTransform;
in vec4 instanceColor;
in vec4 instanceUv;

uniform mat4 viewProjection;

out vec2 fragTexCoord;
out vec3 fragNormal;
out vec4 fragColor;

void main() {
    fragTexCoord = vertexTexCoord * instanceUv.xy + instanceUv.zw;
    fragNormal = mat3(instanceTransform) * vertexNormal;
    fragColor = instanceColor;
    gl_Position = viewProjection * instanceTransform * vec4(vertexPosition, 1.0);
}
)";

// A imagem é lida em grade comum ou em hex-tiling (uniforms por mesh, TextureTiling) e
// multiplicada pela cor da instância e pela do material (partes de um modelo). Em malha
// de dois lados, o lado de trás vira a normal (a luz vale para o lado visto).
//
// Hex-tiling: Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(3), 2022, versão para
// cor (hex2colTex do demo oficial, MIT: github.com/mmikk/hextile-demo). As coordenadas
// de textura caem numa grade de triângulos cujos vértices são centros de hexágonos; cada
// centro lê a imagem com deslocamento e rotação aleatórios, e o ponto mistura os três
// centros do seu triângulo. Os pesos são as baricêntricas elevadas a 7 (transição curta)
// vezes a luminância de cada leitura (o mais claro prevalece, o que preserva o contraste
// melhor que uma média). O sorteio usa o hash inteiro pcg2d (Jarzynski e Olano, "Hash
// Functions for GPU Rendering", JCGT 2020), estável em qualquer coordenada, em vez do
// hash com sin() do original.
constexpr const char* fragmentShader = R"(#version 330
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec3 lightDirection;
uniform vec4 materialColor;
uniform int tiling;     // 0 = grade comum, 1 = hex-tiling
uniform vec2 hexTiling; // x = rotação (0..1), y = contraste da transição (0.5 = neutro)

out vec4 finalColor;

const float pi = 3.14159265;
const float hexScale = 3.46410162; // 2 * sqrt(3): hexágono ~0,29 repetição da imagem

vec2 hash2(ivec2 p) {
    uvec2 v = uvec2(p) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return vec2(v) * (1.0 / 4294967296.0);
}

// Curva de ganho: r > 0.5 aproxima os pesos de 0 ou 1 (transição mais nítida).
vec3 gain(vec3 x, float r) {
    float k = log(1.0 - r) / log(0.5);
    vec3 s = 2.0 * step(0.5, x);
    vec3 m = 2.0 * (1.0 - s);
    vec3 result = 0.5 * s + 0.25 * m * pow(max(vec3(0.0), s + x * m), vec3(k));
    return result / (result.x + result.y + result.z);
}

// A grade e as rotações ficam num espaço com a proporção da imagem (unidade = altura
// dela): numa imagem não quadrada, girar direto em UV esticaria o desenho.
vec4 hexTiled(vec2 uvIn, vec2 uvDx, vec2 uvDy) {
    vec2 size = vec2(textureSize(texture0, 0));
    vec2 aspect = vec2(size.x / size.y, 1.0);
    vec2 st = uvIn * aspect;
    vec2 dx = uvDx * aspect;
    vec2 dy = uvDy * aspect;
    vec2 grid = st * hexScale;
    vec2 skewed = vec2(grid.x - 0.57735027 * grid.y, 1.15470054 * grid.y);
    ivec2 base = ivec2(floor(skewed));
    vec3 t = vec3(fract(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    vec3 barycentric = max(vec3(-t.z * s2, s - t.y * s2, s - t.x * s2), vec3(0.0));
    int si = int(s);
    ivec2 corners[3] = ivec2[3](base + ivec2(si, si), base + ivec2(si, 1 - si), base + ivec2(1 - si, si));

    vec4 samples[3];
    vec3 luminance;
    for (int i = 0; i < 3; ++i) {
        vec2 corner = vec2(corners[i]);
        vec2 center = vec2(corner.x + 0.5 * corner.y, corner.y * 0.8660254) / hexScale;
        float angle = (hash2(corners[i] + ivec2(7919, 104729)).x * 2.0 - 1.0) * pi * hexTiling.x;
        float c = cos(angle);
        float n = sin(angle);
        mat2 rotation = mat2(c, -n, n, c);
        vec2 uv = (rotation * (st - center) + center + hash2(corners[i])) / aspect;
        samples[i] = textureGrad(texture0, uv, rotation * dx / aspect, rotation * dy / aspect);
        luminance[i] = dot(samples[i].rgb, vec3(0.299, 0.587, 0.114));
    }
    vec3 weight = mix(vec3(1.0), luminance, 0.6) * pow(barycentric, vec3(7.0));
    weight /= weight.x + weight.y + weight.z;
    if (hexTiling.y != 0.5) {
        weight = gain(weight, hexTiling.y);
    }
    return weight.x * samples[0] + weight.y * samples[1] + weight.z * samples[2];
}

void main() {
    vec2 dx = dFdx(fragTexCoord);
    vec2 dy = dFdy(fragTexCoord);
    vec4 texel = tiling == 1 ? hexTiled(fragTexCoord, dx, dy) : texture(texture0, fragTexCoord);
    vec4 base = texel * fragColor * materialColor;
    vec3 normal = normalize(fragNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    float diffuse = max(dot(normal, -lightDirection), 0.0);
    finalColor = vec4(base.rgb * (0.35 + 0.65 * diffuse), base.a);
}
)";

/// Uma instância no buffer da GPU: 4 colunas da matriz de modelo + cor + transformação
/// das coordenadas de textura.
struct Instance {
    glm::mat4 model;
    glm::vec4 color;
    glm::vec4 uv;
};
static_assert(sizeof(Instance) == 24 * sizeof(float));

/// Um nível de detalhe: as partes e a partir de que tamanho na tela ele entra.
struct GpuLevel {
    std::vector<GpuPart> parts{};
    float screenSize = 0.0f;  // entra abaixo disto (o nível 0 vale sempre)
    bool ownsMeshes = true;   // false: as malhas são de um modelo em Impl::models
    int triangles = 0;

    [[nodiscard]] bool hasGeometry() const {
        return std::any_of(parts.begin(), parts.end(), [](const GpuPart& part) { return part.mesh.vertexCount > 0; });
    }
};

/// O que um id de mesh desenha: níveis de detalhe (0 = original), impostor opcional e
/// as cópias do frame separadas por nível.
struct GpuMesh {
    std::vector<GpuLevel> levels{};
    GpuImpostor impostor{};
    float impostorScreenSize = 0.0f;
    float cullScreenSize = 0.0f;
    Bounds bounds{}; // esfera do nível 0, no espaço do objeto
    std::vector<std::vector<Instance>> instances{}; // por nível; a última lista é a dos impostores
    int tiling = 0;            // uniform "tiling": TextureTiling
    glm::vec2 hex{1.0f, 0.5f}; // uniform "hexTiling": rotação, contraste
    bool doubleSided = false;

    [[nodiscard]] int impostorLevel() const { return static_cast<int>(levels.size()); }

    /// Nível para um objeto que ocupa `screen` da altura da tela; -1 = não desenha.
    [[nodiscard]] int levelFor(float screen) const {
        if (screen < cullScreenSize) {
            return -1;
        }
        if (impostor.valid() && screen < impostorScreenSize) {
            return impostorLevel();
        }
        int level = 0;
        for (std::size_t i = 1; i < levels.size(); ++i) {
            if (screen < levels[i].screenSize) {
                level = static_cast<int>(i);
            }
        }
        return level;
    }
};

GpuMesh makeGpuMesh(std::vector<GpuPart> parts, const PrimitiveMesh& primitive, bool ownsMeshes) {
    GpuMesh gpuMesh;
    gpuMesh.bounds = raylib_detail::boundsOf(parts);
    GpuLevel level{std::move(parts)};
    level.ownsMeshes = ownsMeshes;
    for (const auto& part : level.parts) {
        level.triangles += part.mesh.triangleCount;
    }
    gpuMesh.levels.push_back(std::move(level));
    gpuMesh.instances.resize(gpuMesh.levels.size() + 1);
    gpuMesh.tiling = primitive.tiling == TextureTiling::Hex ? 1 : 0;
    // O contraste vira expoente log(1 - r): fora de (0, 1) a curva não existe.
    gpuMesh.hex = {glm::clamp(primitive.hex.rotation, 0.0f, 1.0f), glm::clamp(primitive.hex.contrast, 0.01f, 0.99f)};
    gpuMesh.doubleSided = primitive.doubleSided;
    gpuMesh.cullScreenSize = std::max(primitive.cullScreenSize, 0.0f);
    return gpuMesh;
}

/// Lote de cópias paradas (World::setInstanceBatch): tudo num buffer da GPU, ordenado
/// por célula; cada célula é um trecho contínuo com a sua esfera envolvente.
struct StaticCell {
    Bounds bounds{};             // esfera que contém as cópias da célula
    float instanceRadius = 0.0f; // maior esfera de uma cópia (para o tamanho na tela)
    std::size_t first = 0;
    std::size_t count = 0;
};

struct GpuBatch {
    std::size_t slot = 0;
    std::vector<StaticCell> cells{};
    unsigned int buffer = 0;
    std::size_t count = 0;
};

/// Um draw call instanciado: `count` cópias a partir de `first` no `buffer`, num nível
/// da mesh (o último nível é o impostor).
struct DrawItem {
    const GpuMesh* mesh = nullptr;
    int level = 0;
    unsigned int buffer = 0;
    std::size_t first = 0;
    std::size_t count = 0;
};

/// Aponta os atributos de instância do shader ativo para `buffer`, a partir de `first`.
void bindInstances(unsigned int buffer, std::size_t first, int transformLocation, int colorLocation, int uvLocation) {
    constexpr int stride = sizeof(Instance);
    const auto offset = static_cast<int>(first * sizeof(Instance));
    rlEnableVertexBuffer(buffer);
    for (int column = 0; column < 4; ++column) {
        const auto location = static_cast<unsigned int>(transformLocation + column);
        rlSetVertexAttribute(location, 4, RL_FLOAT, false, stride, offset + column * static_cast<int>(sizeof(glm::vec4)));
        rlEnableVertexAttribute(location);
        rlSetVertexAttributeDivisor(location, 1);
    }
    if (colorLocation >= 0) {
        const auto location = static_cast<unsigned int>(colorLocation);
        rlSetVertexAttribute(location, 4, RL_FLOAT, false, stride, offset + static_cast<int>(sizeof(glm::mat4)));
        rlEnableVertexAttribute(location);
        rlSetVertexAttributeDivisor(location, 1);
    }
    if (uvLocation >= 0) {
        const auto location = static_cast<unsigned int>(uvLocation);
        rlSetVertexAttribute(location, 4, RL_FLOAT, false, stride,
                             offset + static_cast<int>(sizeof(glm::mat4) + sizeof(glm::vec4)));
        rlEnableVertexAttribute(location);
        rlSetVertexAttributeDivisor(location, 1);
    }
}

/// Cor de depuração de cada nível (RaylibConfig::lodDebugKey). Acima de 1 para aparecer
/// também em texturas escuras (multiplica a cor).
glm::vec4 lodDebugColor(int level, bool impostor) {
    if (impostor) {
        return {2.5f, 0.4f, 2.5f, 1.0f}; // magenta
    }
    switch (level) {
    case 0: return glm::vec4{1.0f};
    case 1: return {0.4f, 0.9f, 3.0f, 1.0f}; // azul
    case 2: return {3.0f, 2.4f, 0.3f, 1.0f}; // amarelo
    default: return {3.0f, 0.8f, 0.3f, 1.0f}; // laranja
    }
}

void drawInstanced(const Mesh& mesh, std::size_t count) {
    if (mesh.indices != nullptr) {
        rlDrawVertexArrayElementsInstanced(0, mesh.triangleCount * 3, nullptr, static_cast<int>(count));
    } else {
        rlDrawVertexArrayInstanced(0, mesh.vertexCount, static_cast<int>(count));
    }
}

Color toRaylib(const glm::vec4& color) {
    const glm::vec4 bytes = glm::clamp(color, 0.0f, 1.0f) * 255.0f;
    return Color{static_cast<unsigned char>(bytes.r), static_cast<unsigned char>(bytes.g),
                 static_cast<unsigned char>(bytes.b), static_cast<unsigned char>(bytes.a)};
}

void forwardLog(int level, const char* text, va_list args) {
    std::array<char, 1024> message{};
    std::vsnprintf(message.data(), message.size(), text, args);
    switch (level) {
    case LOG_FATAL:
    case LOG_ERROR: spdlog::error("raylib: {}", message.data()); break;
    case LOG_WARNING: spdlog::warn("raylib: {}", message.data()); break;
    default: spdlog::debug("raylib: {}", message.data()); break;
    }
}

/// Reposiciona os vértices na CPU, uma vez só (vértice = (v - center) * scale + offset):
/// o tamanho e o deslocamento do PrimitiveMesh não custam nada por instância.
void placeVertices(Mesh& mesh, glm::vec3 center, glm::vec3 scale, glm::vec3 offset) {
    const auto floats = static_cast<std::size_t>(mesh.vertexCount) * 3;
    auto vertexAt = [&mesh](std::size_t i) {
        return glm::vec3{mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]};
    };
    for (std::size_t i = 0; i < floats; i += 3) {
        const glm::vec3 vertex = (vertexAt(i) - center) * scale + offset;
        mesh.vertices[i] = vertex.x;
        mesh.vertices[i + 1] = vertex.y;
        mesh.vertices[i + 2] = vertex.z;

        if (mesh.normals != nullptr) {
            // Escala não uniforme: a normal é transformada pela inversa da escala.
            const glm::vec3 normal = glm::normalize(
                glm::vec3{mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2]} / scale);
            mesh.normals[i] = normal.x;
            mesh.normals[i + 1] = normal.y;
            mesh.normals[i + 2] = normal.z;
        }
    }

    const auto bytes = static_cast<int>(floats * sizeof(float));
    UpdateMeshBuffer(mesh, RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, mesh.vertices, bytes, 0);
    if (mesh.normals != nullptr) {
        UpdateMeshBuffer(mesh, RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL, mesh.normals, bytes, 0);
    }
}

/// Gera a primitiva já no tamanho e na posição pedidos.
Mesh generateMesh(const PrimitiveMesh& primitive) {
    Mesh mesh{};
    switch (primitive.shape) {
    case PrimitiveShape::Box: mesh = GenMeshCube(1.0f, 1.0f, 1.0f); break;
    case PrimitiveShape::Sphere: mesh = GenMeshSphere(0.5f, 12, 16); break;
    case PrimitiveShape::Cylinder: mesh = GenMeshCylinder(0.5f, 1.0f, 16); break;
    case PrimitiveShape::Plane: mesh = GenMeshPlane(1.0f, 1.0f, 1, 1); break;
    case PrimitiveShape::Custom: // vazia até World::uploadMesh
    case PrimitiveShape::Model:  // vem do arquivo (Impl::loadModel)
        return mesh;
    }

    // Cada gerador da raylib tem sua origem (o cilindro nasce apoiado em y = 0): encaixa
    // a caixa envolvente em `size`, centrada em `offset`.
    glm::vec3 low{FLT_MAX};
    glm::vec3 high{-FLT_MAX};
    for (int i = 0; i < mesh.vertexCount; ++i) {
        const glm::vec3 vertex{mesh.vertices[i * 3], mesh.vertices[i * 3 + 1], mesh.vertices[i * 3 + 2]};
        low = glm::min(low, vertex);
        high = glm::max(high, vertex);
    }
    const glm::vec3 scale = glm::max(primitive.size, glm::vec3{1e-4f}) / glm::max(high - low, glm::vec3{1e-6f});
    placeVertices(mesh, (low + high) * 0.5f, scale, primitive.offset);
    return mesh;
}

/// Malha do jogo -> Mesh da raylib, sem índices (lista de triângulos expandida): os
/// índices da raylib são de 16 bits, e uma malha procedural passa fácil de 65k vértices.
Mesh buildMesh(const MeshData& data) {
    Mesh mesh{};
    std::vector<std::uint32_t> corners = data.indices;
    if (corners.empty()) {
        corners.resize(data.positions.size() - data.positions.size() % 3);
        for (std::uint32_t i = 0; i < corners.size(); ++i) {
            corners[i] = i;
        }
    }
    corners.resize(corners.size() - corners.size() % 3);
    const bool valid = std::all_of(corners.begin(), corners.end(),
                                   [&](std::uint32_t index) { return index < data.positions.size(); });
    if (!valid) {
        spdlog::warn("RaylibRenderBackend: malha com índice fora das posições; ignorada");
        return mesh;
    }
    if (corners.empty()) {
        return mesh;
    }

    const auto count = corners.size();
    mesh.vertexCount = static_cast<int>(count);
    mesh.triangleCount = static_cast<int>(count / 3);
    mesh.vertices = static_cast<float*>(RL_MALLOC(count * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(RL_MALLOC(count * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(RL_MALLOC(count * 2 * sizeof(float)));
    const bool hasNormals = data.normals.size() == data.positions.size();
    const bool hasUvs = data.uvs.size() == data.positions.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = corners[i];
        const glm::vec3 position = data.positions[index];
        const glm::vec3 normal = hasNormals ? data.normals[index] : glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::vec2 uv = hasUvs ? data.uvs[index] : glm::vec2{0.0f};
        std::copy_n(&position.x, 3, mesh.vertices + i * 3);
        std::copy_n(&normal.x, 3, mesh.normals + i * 3);
        std::copy_n(&uv.x, 2, mesh.texcoords + i * 2);
    }
    UploadMesh(&mesh, false);
    return mesh;
}

/// Mipmaps e filtro anisotrópico: a imagem fica nítida vista de longe e de lado.
void sharpen(Texture2D& texture) {
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_16X);
}

/// Carrega a imagem (com sharpen). Falha = textura vazia e um aviso; a mesh continua
/// sendo desenhada.
Texture2D loadTexture(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    Texture2D texture = LoadTexture(path.c_str());
    if (!IsTextureValid(texture)) {
        spdlog::warn("RaylibRenderBackend: não foi possível carregar a imagem '{}'", path);
        return {};
    }
    sharpen(texture);
    spdlog::info("RaylibRenderBackend: imagem '{}' carregada ({}x{})", path, texture.width, texture.height);
    return texture;
}

} // namespace

struct RaylibRenderBackend::Impl {
    RaylibConfig config;
    MouseCursor cursor = MouseCursor::Default;

    // Shader principal (malhas) e o dos impostores; cada um tem os seus locais de atributo.
    Shader shader{};
    int transformLocation = -1;
    int colorLocation = -1;
    int uvLocation = -1;
    int viewProjectionLocation = -1;
    int textureLocation = -1;
    int tilingLocation = -1;
    int hexTilingLocation = -1;
    int materialColorLocation = -1;

    Shader impostorShader{};
    int impostorTransformLocation = -1;
    int impostorColorLocation = -1;
    int impostorViewProjectionLocation = -1;
    int impostorCameraLocation = -1;
    int impostorBoundsLocation = -1;
    int impostorFramesLocation = -1;
    int impostorTextureLocation = -1;
    int impostorNormalLocation = -1;
    int impostorTintLocation = -1;
    Mesh impostorQuad{};

    // Uma entrada por id configurado; a última é o cubo usado para ids desconhecidos.
    std::vector<GpuMesh> meshes;
    std::unordered_map<std::uint32_t, std::size_t> slots;
    std::unordered_map<std::uint32_t, Texture2D> runtimeTextures; // World::uploadTexture, por id
    std::unordered_set<std::uint32_t> staleMipmaps;               // updateTexture: refazer antes de desenhar

    /// As partes (todos os níveis) das meshes com PrimitiveMesh::runtimeTexture == id
    /// passam a usar `texture`.
    void bindRuntimeTexture(std::uint32_t id, Texture2D texture) {
        for (const auto& [meshId, primitive] : config.meshes) {
            if (primitive.runtimeTexture != id) {
                continue;
            }
            const auto slot = slots.find(meshId);
            if (slot == slots.end()) {
                continue;
            }
            for (auto& level : meshes[slot->second].levels) {
                for (auto& part : level.parts) {
                    part.texture = texture;
                }
            }
        }
    }
    std::size_t fallbackSlot = 0; // cubo para ids sem geometria
    std::unordered_map<std::string, Texture2D> textures; // por caminho; as meshes só apontam
    std::vector<Model> models; // donos das malhas e imagens das meshes PrimitiveShape::Model
    std::unordered_map<std::uint32_t, GpuBatch> batches; // lotes parados, por id

    unsigned int instanceBuffer = 0; // cópias enviadas pelo World neste frame
    std::size_t instanceCapacity = 0;
    std::vector<DrawItem> drawItems; // do frame; clear() mantém a capacidade

    glm::mat4 viewProjection{1.0f};
    Frustum frustum;
    glm::vec3 cameraPosition{0.0f};
    float tanHalfFov = 1.0f;

    std::string overlay; // do frame atual; vazio = sem texto
    std::size_t objectCount = 0;  // cópias enviadas pelo World neste frame
    std::size_t batchedCount = 0; // cópias nos lotes parados
    std::size_t drawCalls = 0;    // deste frame
    std::size_t triangles = 0;    // deste frame
    bool lodColors = false;       // cor de depuração por nível (RaylibConfig::lodDebugKey)
    std::unique_ptr<raylib_detail::RlglUiRenderer> uiRenderer; // desenho da interface (rlgl)
    std::unique_ptr<ui_rmlui::UiLayer> ui; // World::showUi; criada no primeiro documento
    raylib_detail::TerrainRenderer terrain;     // World::setTerrain
    std::array<bool, 3> uiHeld{};               // botão apertado sobre a interface, até soltar
    std::uint64_t frames = 0;
    std::uint64_t draws = 0;
    bool windowOpen = false;
    bool closeRequested = false;

    std::size_t slotFor(std::uint32_t meshId);
    std::vector<GpuPart> loadModel(const PrimitiveMesh& primitive, Texture2D replacement);
    void addDetail(GpuMesh& mesh, const PrimitiveMesh& primitive, Texture2D replacement);

    /// Quanto da altura da tela uma esfera de `radius` a `distance` da câmera ocupa.
    [[nodiscard]] float screenSize(float radius, float distance) const {
        return radius / (std::max(distance, 1e-3f) * tanHalfFov);
    }

    void collectBatch(const GpuBatch& batch);
    void drawScene();
    void drawMeshes();
    void drawImpostors();
    void drawOverlay() const;
    void release();
};

std::size_t RaylibRenderBackend::Impl::slotFor(std::uint32_t meshId) {
    if (const auto it = slots.find(meshId); it != slots.end()) {
        return it->second;
    }
    // Avisa uma vez e passa a tratar o id como cubo.
    spdlog::warn("RaylibRenderBackend: mesh {} sem geometria configurada; desenhando como cubo", meshId);
    slots.emplace(meshId, fallbackSlot);
    return fallbackSlot;
}

/// Cada malha do arquivo vira uma parte com a imagem e a cor base do seu material (ou
/// `replacement`, se tiver imagem). O Model fica em `models` até o release.
std::vector<GpuPart> RaylibRenderBackend::Impl::loadModel(const PrimitiveMesh& primitive, Texture2D replacement) {
    Model model = LoadModel(primitive.model.c_str());
    if (model.meshCount == 0 || model.meshes == nullptr) {
        spdlog::warn("RaylibRenderBackend: não foi possível carregar o modelo '{}'", primitive.model);
        UnloadModel(model);
        return {};
    }
    // Imagem externa (ex.: material de uma paleta usado por vários prédios): uma textura só
    // por arquivo, em `textures`. O modelo deixa de ser dono dela (UnloadModel não a libera).
    // A raylib numera os materiais do arquivo a partir de 1 (o 0 é o padrão dela).
    const std::vector<std::filesystem::path> files =
        primitive.model.ends_with(".glb") || primitive.model.ends_with(".gltf") ? asset::materialImageFiles(primitive.model)
                                                                                : std::vector<std::filesystem::path>{};
    std::vector<Texture2D> albedo(static_cast<std::size_t>(model.materialCount));
    for (int i = 0; i < model.materialCount; ++i) {
        Texture2D& texture = model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture;
        const bool loaded = texture.id != 0 && texture.id != rlGetTextureIdDefault();
        const std::size_t source = static_cast<std::size_t>(i) - 1;
        if (i > 0 && source < files.size() && !files[source].empty()) {
            auto [it, inserted] = textures.try_emplace(files[source].generic_string());
            if (inserted) {
                if (loaded) {
                    sharpen(texture);
                }
                it->second = loaded ? texture : loadTexture(it->first);
            } else if (loaded) {
                UnloadTexture(texture); // já carregada por outro modelo
            }
            albedo[static_cast<std::size_t>(i)] = it->second;
            texture = Texture2D{};
            texture.id = rlGetTextureIdDefault();
            continue;
        }
        if (loaded) {
            sharpen(texture);
        }
        albedo[static_cast<std::size_t>(i)] = texture;
    }

    std::vector<GpuPart> parts;
    int triangles = 0;
    for (int i = 0; i < model.meshCount; ++i) {
        Mesh& mesh = model.meshes[i];
        placeVertices(mesh, glm::vec3{0.0f}, glm::max(primitive.size, glm::vec3{1e-4f}), primitive.offset);
        const int material = model.meshMaterial != nullptr ? model.meshMaterial[i] : 0;
        const bool known = material >= 0 && material < model.materialCount;
        const MaterialMap map = known ? model.materials[material].maps[MATERIAL_MAP_ALBEDO] : MaterialMap{};
        const Texture2D texture = known ? albedo[static_cast<std::size_t>(material)] : Texture2D{};
        GpuPart part{mesh, replacement.id != 0 ? replacement : texture};
        part.color = glm::vec4{map.color.r, map.color.g, map.color.b, map.color.a} / 255.0f;
        parts.push_back(part);
        triangles += mesh.triangleCount;
    }
    spdlog::info("RaylibRenderBackend: modelo '{}' carregado ({} partes, {} triângulos)", primitive.model,
                 parts.size(), triangles);
    models.push_back(model);
    return parts;
}

/// Níveis de detalhe e impostor pedidos no PrimitiveMesh, depois do nível 0 carregado.
void RaylibRenderBackend::Impl::addDetail(GpuMesh& mesh, const PrimitiveMesh& primitive, Texture2D replacement) {
    if (primitive.lods.empty() && !primitive.impostor.enabled) {
        return;
    }
    if (primitive.shape == PrimitiveShape::Custom) {
        spdlog::warn("RaylibRenderBackend: lods e impostor não valem para PrimitiveShape::Custom; ignorados");
        return;
    }
    const std::string name = primitive.model.empty() ? std::string{"primitiva"} : primitive.model;

    auto lods = primitive.lods;
    std::sort(lods.begin(), lods.end(), [](const MeshLod& a, const MeshLod& b) { return a.screenSize > b.screenSize; });
    for (const auto& lod : lods) {
        GpuLevel level;
        level.screenSize = lod.screenSize;
        if (!lod.model.empty()) {
            PrimitiveMesh file = primitive;
            file.model = lod.model;
            level.parts = loadModel(file, replacement);
            level.ownsMeshes = false;
        } else {
            for (const auto& part : mesh.levels.front().parts) {
                GpuPart simple = part;
                simple.mesh = raylib_detail::simplifyMesh(part.mesh, std::clamp(lod.triangleRatio, 0.001f, 1.0f));
                level.parts.push_back(simple);
            }
        }
        for (const auto& part : level.parts) {
            level.triangles += part.mesh.triangleCount;
        }
        spdlog::info("RaylibRenderBackend: '{}' LOD {}: {} triângulos (abaixo de {:.3f} da tela)", name,
                     mesh.levels.size(), level.triangles, level.screenSize);
        mesh.levels.push_back(std::move(level));
    }

    if (primitive.impostor.enabled) {
        mesh.impostor = raylib_detail::bakeImpostor(mesh.levels.front().parts, mesh.bounds, primitive.impostor.frames,
                                                    primitive.impostor.frameSize);
        mesh.impostorScreenSize = primitive.impostor.screenSize;
        if (mesh.impostor.valid()) {
            spdlog::info("RaylibRenderBackend: '{}' impostor: {}x{} vistas de {} px (abaixo de {:.3f} da tela)", name,
                         mesh.impostor.frames, mesh.impostor.frames, mesh.impostor.albedo.width / mesh.impostor.frames,
                         mesh.impostorScreenSize);
        } else {
            spdlog::warn("RaylibRenderBackend: '{}' sem impostor (falha na fotografia)", name);
        }
    }
    mesh.instances.resize(mesh.levels.size() + 1);
}

/// Células visíveis do lote viram draw calls, com o nível escolhido pela distância até a
/// célula. Células vizinhas no buffer com o mesmo nível saem num draw call só.
void RaylibRenderBackend::Impl::collectBatch(const GpuBatch& batch) {
    const GpuMesh& mesh = meshes[batch.slot];
    std::optional<std::size_t> open; // draw call que ainda pode crescer
    for (const auto& cell : batch.cells) {
        int level = -1;
        if (frustum.sees(cell.bounds)) {
            const float nearest = glm::distance(cameraPosition, cell.bounds.center) - cell.bounds.radius;
            level = mesh.levelFor(screenSize(cell.instanceRadius, nearest));
        }
        const bool drawable = level >= 0 && (level == mesh.impostorLevel() ||
                                             mesh.levels[static_cast<std::size_t>(level)].hasGeometry());
        if (!drawable) {
            open.reset();
            continue;
        }
        if (open && drawItems[*open].level == level && drawItems[*open].first + drawItems[*open].count == cell.first) {
            drawItems[*open].count += cell.count;
        } else {
            drawItems.push_back({&mesh, level, batch.buffer, cell.first, cell.count});
            open = drawItems.size() - 1;
        }
    }
}

void RaylibRenderBackend::Impl::drawScene() {
    ZoneScoped;
    drawItems.clear();
    for (const std::uint32_t id : staleMipmaps) {
        if (const auto it = runtimeTextures.find(id); it != runtimeTextures.end()) {
            sharpen(it->second);
        }
    }
    staleMipmaps.clear();

    // Cópias do World: todas num buffer só, uma faixa por (mesh, nível).
    std::size_t total = 0;
    for (const auto& mesh : meshes) {
        for (const auto& list : mesh.instances) {
            total += list.size();
        }
    }
    if (total > instanceCapacity) {
        if (instanceBuffer != 0) {
            rlUnloadVertexBuffer(instanceBuffer);
        }
        instanceCapacity = std::max(total, instanceCapacity + instanceCapacity / 2);
        instanceBuffer = rlLoadVertexBuffer(nullptr, static_cast<int>(instanceCapacity * sizeof(Instance)), true);
    }
    std::size_t first = 0;
    for (const auto& mesh : meshes) {
        for (std::size_t level = 0; level < mesh.instances.size(); ++level) {
            const auto& list = mesh.instances[level];
            const bool drawable = !list.empty() && (static_cast<int>(level) == mesh.impostorLevel() ||
                                                    mesh.levels[level].hasGeometry());
            if (!drawable) {
                continue; // nada a desenhar, ou malha do jogo que ainda não chegou
            }
            rlUpdateVertexBuffer(instanceBuffer, list.data(), static_cast<int>(list.size() * sizeof(Instance)),
                                 static_cast<int>(first * sizeof(Instance)));
            drawItems.push_back({&mesh, static_cast<int>(level), instanceBuffer, first, list.size()});
            first += list.size();
        }
    }
    // Lotes parados: já estão na GPU; só se escolhe o que desenhar.
    for (const auto& [id, batch] : batches) {
        collectBatch(batch);
    }
    if (drawItems.empty() && !terrain.active()) {
        return;
    }

    rlDrawRenderBatchActive(); // esvazia o lote 2D da raylib antes de mudar o estado do GL
    rlEnableDepthTest();
    terrain.draw(viewProjection, cameraPosition, lodColors, drawCalls, triangles);
    if (!drawItems.empty()) {
        drawMeshes();
        drawImpostors();
    }
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableTexture();
    rlDisableShader();
    rlDisableDepthTest();
}

void RaylibRenderBackend::Impl::drawMeshes() {
    rlEnableShader(shader.id);
    rlSetUniformMatrix(viewProjectionLocation, toRaylib(viewProjection));
    const int textureUnit = 0;
    rlSetUniform(textureLocation, &textureUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    rlActiveTextureSlot(0);

    for (const auto& item : drawItems) {
        const GpuMesh& mesh = *item.mesh;
        if (item.level == mesh.impostorLevel()) {
            continue;
        }
        rlSetUniform(tilingLocation, &mesh.tiling, RL_SHADER_UNIFORM_INT, 1);
        rlSetUniform(hexTilingLocation, &mesh.hex, RL_SHADER_UNIFORM_VEC2, 1);
        if (mesh.doubleSided) {
            rlDisableBackfaceCulling();
        }
        // Cada parte tem a sua malha (VAO); as instâncias são as mesmas.
        for (const auto& part : mesh.levels[static_cast<std::size_t>(item.level)].parts) {
            if (part.mesh.vertexCount == 0) {
                continue;
            }
            rlEnableTexture(part.texture.id != 0 ? part.texture.id : rlGetTextureIdDefault());
            const glm::vec4 color = lodColors ? part.color * lodDebugColor(item.level, false) : part.color;
            rlSetUniform(materialColorLocation, &color, RL_SHADER_UNIFORM_VEC4, 1);
            rlEnableVertexArray(part.mesh.vaoId);
            bindInstances(item.buffer, item.first, transformLocation, colorLocation, uvLocation);
            drawInstanced(part.mesh, item.count);
            ++drawCalls;
            triangles += static_cast<std::size_t>(part.mesh.triangleCount) * item.count;
        }
        if (mesh.doubleSided) {
            rlEnableBackfaceCulling();
        }
    }
}

void RaylibRenderBackend::Impl::drawImpostors() {
    const bool any = std::any_of(drawItems.begin(), drawItems.end(),
                                 [](const DrawItem& item) { return item.level == item.mesh->impostorLevel(); });
    if (!any) {
        return;
    }
    rlEnableShader(impostorShader.id);
    rlSetUniformMatrix(impostorViewProjectionLocation, toRaylib(viewProjection));
    rlSetUniform(impostorCameraLocation, &cameraPosition, RL_SHADER_UNIFORM_VEC3, 1);
    const int albedoUnit = 0;
    const int normalUnit = 1;
    rlSetUniform(impostorTextureLocation, &albedoUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    rlSetUniform(impostorNormalLocation, &normalUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    const glm::vec4 tint = lodColors ? lodDebugColor(0, true) : glm::vec4{1.0f};
    rlSetUniform(impostorTintLocation, &tint, RL_SHADER_UNIFORM_VEC4, 1);
    rlDisableBackfaceCulling();
    rlEnableVertexArray(impostorQuad.vaoId);

    for (const auto& item : drawItems) {
        if (item.level != item.mesh->impostorLevel()) {
            continue;
        }
        const GpuImpostor& impostor = item.mesh->impostor;
        const glm::vec4 bounds{impostor.bounds.center, impostor.bounds.radius};
        const auto frameCount = static_cast<float>(impostor.frames);
        rlSetUniform(impostorBoundsLocation, &bounds, RL_SHADER_UNIFORM_VEC4, 1);
        rlSetUniform(impostorFramesLocation, &frameCount, RL_SHADER_UNIFORM_FLOAT, 1);
        rlActiveTextureSlot(1);
        rlEnableTexture(impostor.normal.id);
        rlActiveTextureSlot(0);
        rlEnableTexture(impostor.albedo.id);
        bindInstances(item.buffer, item.first, impostorTransformLocation, impostorColorLocation, -1);
        drawInstanced(impostorQuad, item.count);
        ++drawCalls;
        triangles += 2 * item.count;
    }

    rlActiveTextureSlot(1);
    rlDisableTexture();
    rlActiveTextureSlot(0);
    rlEnableBackfaceCulling();
}

void RaylibRenderBackend::Impl::drawOverlay() const {
    if (overlay.empty()) {
        return;
    }
    constexpr int fontSize = 20;
    constexpr int lineHeight = 24;
    constexpr int margin = 10;
    const int top = config.showStats ? 72 : margin;

    // Uma faixa escura atrás do texto para ler sobre qualquer fundo.
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= overlay.size()) {
        const auto end = std::min(overlay.find('\n', start), overlay.size());
        lines.emplace_back(overlay.substr(start, end - start));
        start = end + 1;
    }
    int width = 0;
    for (const auto& line : lines) {
        width = std::max(width, MeasureText(line.c_str(), fontSize));
    }
    const int height = static_cast<int>(lines.size()) * lineHeight;
    DrawRectangle(margin - 6, top - 6, width + 12, height + 8, Color{0, 0, 0, 160});
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawText(lines[i].c_str(), margin, top + static_cast<int>(i) * lineHeight, fontSize, RAYWHITE);
    }
}

void RaylibRenderBackend::Impl::release() {
    if (!windowOpen) {
        return;
    }
    ui.reset(); // a RmlUi devolve texturas e geometria enquanto o contexto OpenGL existe
    uiRenderer.reset();
    terrain.release();
    if (instanceBuffer != 0) {
        rlUnloadVertexBuffer(instanceBuffer);
        instanceBuffer = 0;
        instanceCapacity = 0;
    }
    for (auto& [id, batch] : batches) {
        rlUnloadVertexBuffer(batch.buffer);
    }
    batches.clear();
    for (auto& gpuMesh : meshes) {
        for (auto& level : gpuMesh.levels) {
            if (level.ownsMeshes) {
                for (auto& part : level.parts) {
                    UnloadMesh(part.mesh);
                }
            }
        }
        raylib_detail::unloadImpostor(gpuMesh.impostor);
    }
    for (auto& model : models) {
        UnloadModel(model); // malhas e imagens do arquivo
    }
    models.clear();
    for (auto& [path, texture] : textures) {
        if (texture.id != 0) {
            UnloadTexture(texture);
        }
    }
    for (auto& [id, texture] : runtimeTextures) {
        UnloadTexture(texture);
    }
    runtimeTextures.clear();
    meshes.clear();
    slots.clear();
    textures.clear();
    if (impostorQuad.vaoId != 0) {
        UnloadMesh(impostorQuad);
    }
    impostorQuad = {};
    for (Shader* program : {&shader, &impostorShader}) {
        if (IsShaderValid(*program)) {
            UnloadShader(*program);
        }
        *program = {};
    }
    CloseWindow(); // por último: os recursos acima dependem do contexto OpenGL
    windowOpen = false;
}

RaylibRenderBackend::RaylibRenderBackend(RaylibConfig config)
    : impl_{std::make_unique<Impl>()} {
    impl_->config = std::move(config);
}

RaylibRenderBackend::~RaylibRenderBackend() = default;

bool RaylibRenderBackend::initialize() {
    auto& state = *impl_;
    const auto& config = state.config;

    SetTraceLogCallback(forwardLog);
    unsigned int flags = FLAG_WINDOW_RESIZABLE;
    if (config.vsync) {
        flags |= FLAG_VSYNC_HINT;
    }
    if (config.msaa) {
        flags |= FLAG_MSAA_4X_HINT;
    }
    SetConfigFlags(flags);
    InitWindow(config.width, config.height, config.title.c_str());
    if (!IsWindowReady()) {
        spdlog::error("RaylibRenderBackend: não foi possível criar a janela");
        return false;
    }
    state.windowOpen = true;
    if (!config.escapeClosesWindow) {
        SetExitKey(KEY_NULL);
    }

    state.shader = LoadShaderFromMemory(vertexShader, fragmentShader);
    state.transformLocation = GetShaderLocationAttrib(state.shader, "instanceTransform");
    state.colorLocation = GetShaderLocationAttrib(state.shader, "instanceColor");
    state.uvLocation = GetShaderLocationAttrib(state.shader, "instanceUv");
    state.viewProjectionLocation = GetShaderLocation(state.shader, "viewProjection");
    state.textureLocation = GetShaderLocation(state.shader, "texture0");
    state.tilingLocation = GetShaderLocation(state.shader, "tiling");
    state.hexTilingLocation = GetShaderLocation(state.shader, "hexTiling");
    state.materialColorLocation = GetShaderLocation(state.shader, "materialColor");

    state.impostorShader = LoadShaderFromMemory(raylib_detail::impostorVertexShader,
                                                raylib_detail::impostorFragmentShader);
    state.impostorTransformLocation = GetShaderLocationAttrib(state.impostorShader, "instanceTransform");
    state.impostorColorLocation = GetShaderLocationAttrib(state.impostorShader, "instanceColor");
    state.impostorViewProjectionLocation = GetShaderLocation(state.impostorShader, "viewProjection");
    state.impostorCameraLocation = GetShaderLocation(state.impostorShader, "cameraPosition");
    state.impostorBoundsLocation = GetShaderLocation(state.impostorShader, "impostorBounds");
    state.impostorFramesLocation = GetShaderLocation(state.impostorShader, "impostorFrames");
    state.impostorTextureLocation = GetShaderLocation(state.impostorShader, "texture0");
    state.impostorNormalLocation = GetShaderLocation(state.impostorShader, "normalAtlas");
    state.impostorTintLocation = GetShaderLocation(state.impostorShader, "materialColor");

    if (!IsShaderValid(state.shader) || state.transformLocation < 0 || state.colorLocation < 0 ||
        state.uvLocation < 0 || !IsShaderValid(state.impostorShader) || state.impostorTransformLocation < 0) {
        spdlog::error("RaylibRenderBackend: falha ao compilar os shaders de instâncias");
        state.release();
        return false;
    }
    const glm::vec3 light = glm::normalize(config.lightDirection);
    SetShaderValue(state.shader, GetShaderLocation(state.shader, "lightDirection"), &light, SHADER_UNIFORM_VEC3);
    SetShaderValue(state.impostorShader, GetShaderLocation(state.impostorShader, "lightDirection"), &light,
                   SHADER_UNIFORM_VEC3);
    state.impostorQuad = raylib_detail::makeImpostorQuad();
    state.terrain.initialize(config.lightDirection); // falhou = sem terreno (avisa no log)

    state.meshes.reserve(config.meshes.size() + 1);
    for (const auto& [id, primitive] : config.meshes) {
        // Meshes com a mesma imagem compartilham a textura (carregada uma vez só).
        Texture2D texture{};
        if (!primitive.texture.empty()) {
            auto [it, inserted] = state.textures.try_emplace(primitive.texture);
            if (inserted) {
                it->second = loadTexture(primitive.texture);
            }
            texture = it->second;
        }
        state.slots.emplace(id, state.meshes.size());
        if (primitive.shape == PrimitiveShape::Model) {
            state.meshes.push_back(makeGpuMesh(state.loadModel(primitive, texture), primitive, false));
        } else {
            state.meshes.push_back(makeGpuMesh({{generateMesh(primitive), texture}}, primitive, true));
        }
        state.addDetail(state.meshes.back(), primitive, texture);
    }
    state.fallbackSlot = state.meshes.size();
    state.meshes.push_back(makeGpuMesh({{generateMesh(PrimitiveMesh{})}}, PrimitiveMesh{}, true));

    spdlog::info("RaylibRenderBackend inicializado: janela {}x{}, {} meshes configuradas", GetRenderWidth(),
                 GetRenderHeight(), config.meshes.size());
    return true;
}

void RaylibRenderBackend::shutdown() {
    auto& state = *impl_;
    const double seconds = state.windowOpen ? GetTime() : 0.0; // tempo desde InitWindow
    state.release();

    const auto frames = static_cast<double>(state.frames);
    spdlog::info("RaylibRenderBackend encerrado: {} frames, {:.0f} FPS médio, {:.0f} objetos/frame", state.frames,
                 seconds > 0.0 ? frames / seconds : 0.0, frames > 0.0 ? static_cast<double>(state.draws) / frames : 0.0);
}

void RaylibRenderBackend::updateMesh(std::uint32_t id, const MeshData& mesh) {
    auto& state = *impl_;
    if (!state.windowOpen) {
        return;
    }
    // Id sem entrada no config (ou que caiu no cubo): ganha um slot próprio, sem imagem.
    auto it = state.slots.find(id);
    if (it == state.slots.end() || it->second == state.fallbackSlot) {
        const std::size_t slot = state.meshes.size();
        state.meshes.push_back(makeGpuMesh({GpuPart{}}, PrimitiveMesh{PrimitiveShape::Custom}, true));
        it = state.slots.insert_or_assign(id, slot).first;
    }
    auto& gpuMesh = state.meshes[it->second];
    auto& level = gpuMesh.levels.front();
    if (!level.ownsMeshes) {
        spdlog::warn("RaylibRenderBackend: mesh {} é um modelo de arquivo; uploadMesh ignorado", id);
        return;
    }
    if (level.parts.empty()) {
        level.parts.emplace_back();
        // Textura feita em tempo de execução que já chegou antes da malha.
        if (const auto config = state.config.meshes.find(id); config != state.config.meshes.end()) {
            if (const auto texture = state.runtimeTextures.find(config->second.runtimeTexture);
                config->second.runtimeTexture != 0 && texture != state.runtimeTextures.end()) {
                level.parts.front().texture = texture->second;
            }
        }
    }
    UnloadMesh(level.parts.front().mesh);
    level.parts.front().mesh = buildMesh(mesh);
    level.triangles = level.parts.front().mesh.triangleCount;
    gpuMesh.bounds = raylib_detail::boundsOf(level.parts);
}

void RaylibRenderBackend::updateInstanceBatch(std::uint32_t id, const InstanceBatch& batch) {
    auto& state = *impl_;
    if (!state.windowOpen) {
        return;
    }
    if (const auto it = state.batches.find(id); it != state.batches.end()) {
        rlUnloadVertexBuffer(it->second.buffer);
        state.batches.erase(it);
    }
    if (!batch.transforms.empty()) {
        const std::size_t slot = state.slotFor(batch.mesh);
        const Bounds local = state.meshes[slot].bounds;
        const float cellSize = std::max(batch.cellSize, 1.0f);
        const bool colored = batch.colors.size() == batch.transforms.size();

        // Ordena as cópias pela célula (XZ da posição): cada célula vira um trecho contínuo.
        std::vector<std::pair<std::int64_t, std::size_t>> order(batch.transforms.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            const glm::vec4& position = batch.transforms[i][3];
            const auto x = static_cast<std::int64_t>(std::floor(position.x / cellSize));
            const auto z = static_cast<std::int64_t>(std::floor(position.z / cellSize));
            order[i] = {(x << 32) ^ (z & 0xffffffff), i};
        }
        std::sort(order.begin(), order.end());

        GpuBatch gpu;
        gpu.slot = slot;
        std::vector<Instance> instances;
        instances.reserve(order.size());
        for (std::size_t k = 0; k < order.size();) {
            StaticCell cell;
            cell.first = instances.size();
            glm::vec3 low{FLT_MAX};
            glm::vec3 high{-FLT_MAX};
            const std::int64_t key = order[k].first;
            for (; k < order.size() && order[k].first == key; ++k) {
                const std::size_t i = order[k].second;
                instances.push_back({batch.transforms[i], colored ? batch.colors[i] : glm::vec4{1.0f},
                                     glm::vec4{1.0f, 1.0f, 0.0f, 0.0f}});
                const Bounds sphere = raylib_detail::transformBounds(local, batch.transforms[i]);
                low = glm::min(low, sphere.center - sphere.radius);
                high = glm::max(high, sphere.center + sphere.radius);
                cell.instanceRadius = std::max(cell.instanceRadius, sphere.radius);
            }
            cell.count = instances.size() - cell.first;
            cell.bounds = {(low + high) * 0.5f, glm::length(high - low) * 0.5f};
            gpu.cells.push_back(cell);
        }
        gpu.count = instances.size();
        gpu.buffer = rlLoadVertexBuffer(instances.data(), static_cast<int>(instances.size() * sizeof(Instance)), false);
        state.batches[id] = std::move(gpu);
    }
    state.batchedCount = 0;
    for (const auto& [batchId, gpu] : state.batches) {
        state.batchedCount += gpu.count;
    }
}

void RaylibRenderBackend::beginFrame(const CameraView& camera) {
    auto& state = *impl_;

    const int width = GetRenderWidth();
    const int height = GetRenderHeight();
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    state.viewProjection = glm::perspective(camera.fovY, aspect, camera.nearPlane, camera.farPlane) * camera.view;
    state.frustum = Frustum{state.viewProjection};
    state.cameraPosition = camera.position;
    state.tanHalfFov = std::tan(camera.fovY * 0.5f);

    for (auto& gpuMesh : state.meshes) {
        for (auto& list : gpuMesh.instances) {
            list.clear();
        }
    }
    state.objectCount = 0;
    state.drawCalls = 0;
    state.triangles = 0;
    state.overlay.clear();

    BeginDrawing();
    ClearBackground(toRaylib(state.config.clearColor));
}

void RaylibRenderBackend::submit(std::span<const DrawCommand> commands) {
    ZoneScoped;

    // Cada cópia: fora da câmera, descartada; senão, vai para a lista do nível que o
    // tamanho dela na tela pede.
    auto& state = *impl_;
    for (const auto& command : commands) {
        GpuMesh& mesh = state.meshes[state.slotFor(command.render.mesh)];
        const Bounds sphere = raylib_detail::transformBounds(mesh.bounds, command.model);
        if (!state.frustum.sees(sphere)) {
            continue;
        }
        const int level = mesh.levelFor(state.screenSize(sphere.radius, glm::distance(state.cameraPosition, sphere.center)));
        if (level < 0) {
            continue;
        }
        mesh.instances[static_cast<std::size_t>(level)].push_back(
            {command.model, command.render.color, command.render.uvTransform});
    }
    state.objectCount += commands.size();
}

void RaylibRenderBackend::drawOverlay(std::string_view text) {
    impl_->overlay.assign(text);
}

void RaylibRenderBackend::setTerrain(const TerrainDesc& terrain) {
    auto& state = *impl_;
    if (!state.windowOpen) {
        return;
    }
    Texture2D texture{};
    if (!terrain.texture.empty()) {
        auto [it, inserted] = state.textures.try_emplace(terrain.texture);
        if (inserted) {
            it->second = loadTexture(terrain.texture);
        }
        texture = it->second;
    }
    state.terrain.set(terrain, texture);
}

void RaylibRenderBackend::uploadTexture(std::uint32_t id, const TextureImage& image) {
    auto& state = *impl_;
    if (!state.windowOpen || image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return;
    }
    if (const auto it = state.runtimeTextures.find(id); it != state.runtimeTextures.end()) {
        UnloadTexture(it->second);
    }
    Image source{};
    source.data = const_cast<std::uint8_t*>(image.rgba.data()); // LoadTextureFromImage só lê
    source.width = image.width;
    source.height = image.height;
    source.mipmaps = 1;
    source.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D texture = LoadTextureFromImage(source);
    sharpen(texture);
    state.runtimeTextures[id] = texture;
    state.bindRuntimeTexture(id, texture);
}

void RaylibRenderBackend::updateTexture(std::uint32_t id, const TexturePatch& patch) {
    auto& state = *impl_;
    const auto it = state.runtimeTextures.find(id);
    if (it == state.runtimeTextures.end() || patch.width <= 0 || patch.height <= 0 || patch.x < 0 || patch.y < 0 ||
        patch.x + patch.width > it->second.width || patch.y + patch.height > it->second.height ||
        patch.rgba.size() != static_cast<std::size_t>(patch.width) * static_cast<std::size_t>(patch.height) * 4) {
        spdlog::warn("RaylibRenderBackend: mudança fora da textura {}; ignorada", id);
        return;
    }
    Texture2D& texture = it->second;
    UpdateTextureRec(texture, Rectangle{static_cast<float>(patch.x), static_cast<float>(patch.y), static_cast<float>(patch.width),
                                        static_cast<float>(patch.height)},
                     patch.rgba.data());
    // Mipmaps refeitos na GPU (glGenerateMipmap) uma vez antes do próximo desenho, por
    // mais pedaços que cheguem: a tinta aparece na hora em qualquer distância.
    state.staleMipmaps.insert(id);
}

void RaylibRenderBackend::updateTerrain(const TerrainPatch& patch) {
    if (impl_->windowOpen) {
        impl_->terrain.update(patch);
    }
}

void RaylibRenderBackend::loadUi(const UiDocument& document) {
    auto& state = *impl_;
    if (!state.windowOpen) {
        return;
    }
    if (!state.ui) {
        state.uiRenderer = std::make_unique<raylib_detail::RlglUiRenderer>();
        state.ui = std::make_unique<ui_rmlui::UiLayer>(state.uiRenderer->get());
    }
    state.ui->load(document);
}

void RaylibRenderBackend::updateUi(std::span<const std::pair<std::string, UiValue>> values) {
    if (impl_->ui) {
        impl_->ui->setValues(values);
    }
}

void RaylibRenderBackend::setMouseCursor(MouseCursor cursor) {
    if (!impl_->windowOpen || cursor == impl_->cursor) {
        return;
    }
    impl_->cursor = cursor;
    switch (cursor) {
    case MouseCursor::Hand: SetMouseCursor(MOUSE_CURSOR_POINTING_HAND); break;
    case MouseCursor::Move: SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL); break;
    case MouseCursor::ResizeNwse: SetMouseCursor(MOUSE_CURSOR_RESIZE_NWSE); break;
    case MouseCursor::ResizeNesw: SetMouseCursor(MOUSE_CURSOR_RESIZE_NESW); break;
    case MouseCursor::Crosshair: SetMouseCursor(MOUSE_CURSOR_CROSSHAIR); break;
    case MouseCursor::Default: SetMouseCursor(MOUSE_CURSOR_DEFAULT); break;
    }
}

void RaylibRenderBackend::endFrame() {
    auto& state = *impl_;

    if (GetRenderHeight() > 0) { // janela minimizada: nada a desenhar
        state.drawScene();
    }
    const auto functionKey = [](Key key) {
        const int index = static_cast<int>(key) - static_cast<int>(Key::F1);
        return index >= 0 && index < 12 && IsKeyPressed(KEY_F1 + index);
    };
    if (functionKey(state.config.lodDebugKey)) {
        state.lodColors = !state.lodColors;
    }
    if (state.ui && state.ui->active()) {
        if (functionKey(state.config.uiDebuggerKey)) {
            state.ui->toggleDebugger();
        }
        state.ui->setStats({GetFPS(), state.drawCalls, state.triangles, state.objectCount + state.batchedCount,
                            state.lodColors});
        state.ui->render(GetRenderWidth(), GetRenderHeight());
    }
    if (state.config.showStats) {
        DrawFPS(10, 10);
        DrawText(TextFormat("%zu objetos   %zu draw calls   %.2f mi triângulos%s", state.objectCount + state.batchedCount,
                            state.drawCalls, static_cast<double>(state.triangles) / 1e6,
                            state.lodColors ? "   [LOD colorido]" : ""),
                 10, 34, 20, RAYWHITE);
    }
    state.drawOverlay();
    EndDrawing(); // troca os buffers e atende os eventos da janela
    state.closeRequested = WindowShouldClose();
    ++state.frames;
    state.draws += state.objectCount + state.batchedCount;
}

bool RaylibRenderBackend::closeRequested() const {
    return impl_->closeRequested;
}

void RaylibRenderBackend::pollInput(InputState& input) {
    if (!impl_->windowOpen) {
        return;
    }
    auto readKey = [&input](Key key, int code) {
        const auto i = InputState::index(key);
        input.keys.set(i, IsKeyDown(code));
        if (IsKeyPressed(code)) {
            input.keysPressed.set(i);
        }
        if (IsKeyReleased(code)) {
            input.keysReleased.set(i);
        }
    };
    // Faixas contíguas nos dois enums: letras, dígitos e F1..F12.
    for (int i = 0; i < 26; ++i) {
        readKey(static_cast<Key>(static_cast<int>(Key::A) + i), KEY_A + i);
    }
    for (int i = 0; i < 10; ++i) {
        readKey(static_cast<Key>(static_cast<int>(Key::Num0) + i), KEY_ZERO + i);
    }
    for (int i = 0; i < 12; ++i) {
        readKey(static_cast<Key>(static_cast<int>(Key::F1) + i), KEY_F1 + i);
    }
    static constexpr std::array<std::pair<Key, int>, 30> others{{
        {Key::Up, KEY_UP}, {Key::Down, KEY_DOWN}, {Key::Left, KEY_LEFT}, {Key::Right, KEY_RIGHT},
        {Key::Space, KEY_SPACE}, {Key::Enter, KEY_ENTER}, {Key::Escape, KEY_ESCAPE}, {Key::Tab, KEY_TAB},
        {Key::Backspace, KEY_BACKSPACE}, {Key::Delete, KEY_DELETE}, {Key::Insert, KEY_INSERT},
        {Key::Home, KEY_HOME}, {Key::End, KEY_END}, {Key::PageUp, KEY_PAGE_UP}, {Key::PageDown, KEY_PAGE_DOWN},
        {Key::LeftShift, KEY_LEFT_SHIFT}, {Key::RightShift, KEY_RIGHT_SHIFT},
        {Key::LeftControl, KEY_LEFT_CONTROL}, {Key::RightControl, KEY_RIGHT_CONTROL},
        {Key::LeftAlt, KEY_LEFT_ALT}, {Key::RightAlt, KEY_RIGHT_ALT},
        {Key::Minus, KEY_MINUS}, {Key::Equal, KEY_EQUAL}, {Key::LeftBracket, KEY_LEFT_BRACKET},
        {Key::RightBracket, KEY_RIGHT_BRACKET}, {Key::Comma, KEY_COMMA}, {Key::Period, KEY_PERIOD},
        {Key::Slash, KEY_SLASH}, {Key::KeypadAdd, KEY_KP_ADD}, {Key::KeypadSubtract, KEY_KP_SUBTRACT},
    }};
    for (const auto& [key, code] : others) {
        readKey(key, code);
    }

    // A ordem é a da RmlUi: 0 esquerdo, 1 direito, 2 meio.
    static constexpr std::array<std::pair<MouseButton, int>, 3> buttons{{
        {MouseButton::Left, MOUSE_BUTTON_LEFT},
        {MouseButton::Right, MOUSE_BUTTON_RIGHT},
        {MouseButton::Middle, MOUSE_BUTTON_MIDDLE},
    }};
    const Vector2 position = GetMousePosition();
    const Vector2 delta = GetMouseDelta();
    ui_rmlui::UiMouse mouse;
    mouse.position = {position.x, position.y};
    mouse.wheel = GetMouseWheelMove();
    for (std::size_t b = 0; b < buttons.size(); ++b) {
        mouse.pressed[b] = IsMouseButtonPressed(buttons[b].second);
        mouse.released[b] = IsMouseButtonReleased(buttons[b].second);
    }

    // A interface vê o mouse primeiro: o que começa sobre um painel ou botão (clique,
    // arrasto, roda) fica com ela e não chega ao jogo.
    auto& state = *impl_;
    const bool overUi = state.ui && state.ui->active() && state.ui->processMouse(mouse);
    input.pointerOverUi = overUi;
    for (std::size_t b = 0; b < buttons.size(); ++b) {
        const auto i = InputState::index(buttons[b].first);
        if (mouse.pressed[b] && overUi) {
            state.uiHeld[b] = true;
        }
        input.buttons.set(i, IsMouseButtonDown(buttons[b].second) && !state.uiHeld[b]);
        if (mouse.pressed[b] && !state.uiHeld[b]) {
            input.buttonsPressed.set(i);
        }
        if (mouse.released[b]) {
            if (!state.uiHeld[b]) {
                input.buttonsReleased.set(i);
            }
            state.uiHeld[b] = false;
        }
    }
    input.mousePosition = {position.x, position.y};
    input.mouseDelta += glm::vec2{delta.x, delta.y};
    if (!overUi) {
        input.wheel += mouse.wheel;
    }
    if (state.ui) {
        for (auto& event : state.ui->takeEvents()) {
            if (event.action == "lkz.lodColors") {
                state.lodColors = !state.lodColors;
            } else if (!event.action.starts_with("lkz.")) {
                input.uiEvents.push_back(std::move(event));
            }
        }
    }
    input.viewport = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
}

} // namespace lkz
