#include "TerrainRenderer.hpp"

#include <algorithm>

#include <rlgl.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "GpuTypes.hpp"

namespace lkz::raylib_detail {

namespace {

// Pedaço do CDLOD: a grade [0,1]² vira o quadrado do nó; o vértice se transforma no da
// grade do nível seguinte (vértices ímpares escorregam para o par) conforme a distância.
constexpr const char* terrainVertexShader = R"(#version 330
in vec3 vertexPosition;
in vec4 nodeArea;  // origem x, origem z, lado, resolução
in vec4 nodeMorph; // início do morph, fim do morph, nível

uniform mat4 viewProjection;
uniform vec3 cameraPosition;
uniform sampler2D heights;
uniform vec4 terrainArea; // origem x, origem z, largura, profundidade
uniform vec3 terrainGrid; // colunas, linhas, espaçamento

out vec2 fragTexCoord;
out vec3 fragNormal;
flat out float fragLevel;

float sampleAt(ivec2 c) {
    c = clamp(c, ivec2(0), ivec2(terrainGrid.xy) - 1);
    return texelFetch(heights, c, 0).r;
}

float heightAt(vec2 p) {
    vec2 g = clamp((p - terrainArea.xy) / terrainGrid.z, vec2(0.0), terrainGrid.xy - 1.0);
    ivec2 c = ivec2(min(floor(g), terrainGrid.xy - 2.0));
    vec2 f = g - vec2(c);
    float top = mix(sampleAt(c), sampleAt(c + ivec2(1, 0)), f.x);
    float bottom = mix(sampleAt(c + ivec2(0, 1)), sampleAt(c + ivec2(1, 1)), f.x);
    return mix(top, bottom, f.y);
}

void main() {
    float resolution = nodeArea.w;
    vec2 grid = floor(vertexPosition.xz * resolution + 0.5);
    float quad = nodeArea.z / resolution;
    vec2 p = nodeArea.xy + grid * quad;
    float distanceToCamera = distance(cameraPosition, vec3(p.x, heightAt(p), p.y));
    float k = clamp((distanceToCamera - nodeMorph.x) / max(nodeMorph.y - nodeMorph.x, 1e-3), 0.0, 1.0);
    p -= fract(grid * 0.5) * 2.0 * quad * k;
    p = clamp(p, terrainArea.xy, terrainArea.xy + terrainArea.zw);

    float h = heightAt(p);
    float e = max(quad, terrainGrid.z);
    fragNormal = normalize(vec3(heightAt(p - vec2(e, 0.0)) - heightAt(p + vec2(e, 0.0)), 2.0 * e,
                                heightAt(p - vec2(0.0, e)) - heightAt(p + vec2(0.0, e))));
    fragTexCoord = (p - terrainArea.xy) / terrainArea.zw;
    fragLevel = nodeMorph.z;
    gl_Position = viewProjection * vec4(p.x, h, p.y, 1.0);
}
)";

// A mesma luz do shader principal (o terreno plano sai igual a um plano com a imagem).
constexpr const char* terrainFragmentShader = R"(#version 330
in vec2 fragTexCoord;
in vec3 fragNormal;
flat in float fragLevel;

uniform sampler2D texture0;
uniform vec4 terrainColor;
uniform vec3 lightDirection;
uniform int lodColors;

out vec4 finalColor;

vec3 levelTint(float level) {
    int l = int(level + 0.5);
    if (l == 0) return vec3(1.0);
    if (l == 1) return vec3(0.4, 0.9, 3.0);
    if (l == 2) return vec3(3.0, 2.4, 0.3);
    if (l == 3) return vec3(3.0, 0.8, 0.3);
    return vec3(2.5, 0.4, 2.5);
}

void main() {
    vec4 base = texture(texture0, fragTexCoord) * terrainColor;
    float diffuse = max(dot(normalize(fragNormal), -lightDirection), 0.0);
    vec3 rgb = base.rgb * (0.35 + 0.65 * diffuse);
    if (lodColors == 1) {
        rgb *= levelTint(fragLevel);
    }
    finalColor = vec4(rgb, base.a);
}
)";

/// Grade de resolution x resolution quadrados em [0,1]² (XZ), virada para +Y.
Mesh makeGrid(int resolution) {
    Mesh mesh{};
    const int side = resolution + 1;
    mesh.vertexCount = side * side;
    mesh.triangleCount = resolution * resolution * 2;
    mesh.vertices = static_cast<float*>(RL_MALLOC(static_cast<std::size_t>(mesh.vertexCount) * 3 * sizeof(float)));
    mesh.indices = static_cast<unsigned short*>(RL_MALLOC(static_cast<std::size_t>(mesh.triangleCount) * 3 * sizeof(unsigned short)));
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            float* v = mesh.vertices + (z * side + x) * 3;
            v[0] = static_cast<float>(x) / static_cast<float>(resolution);
            v[1] = 0.0f;
            v[2] = static_cast<float>(z) / static_cast<float>(resolution);
        }
    }
    int i = 0;
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const auto a = static_cast<unsigned short>(z * side + x);
            const auto b = static_cast<unsigned short>(a + 1);
            const auto c = static_cast<unsigned short>(a + side);
            const auto d = static_cast<unsigned short>(c + 1);
            // Anti-horário visto de cima.
            const unsigned short corners[6] = {a, c, b, b, c, d};
            std::copy(std::begin(corners), std::end(corners), mesh.indices + i);
            i += 6;
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

} // namespace

bool TerrainRenderer::initialize(glm::vec3 lightDirection) {
    shader_ = LoadShaderFromMemory(terrainVertexShader, terrainFragmentShader);
    if (!IsShaderValid(shader_)) {
        spdlog::error("RaylibRenderBackend: falha ao compilar o shader do terreno");
        return false;
    }
    nodeAreaLocation_ = GetShaderLocationAttrib(shader_, "nodeArea");
    nodeMorphLocation_ = GetShaderLocationAttrib(shader_, "nodeMorph");
    viewProjectionLocation_ = GetShaderLocation(shader_, "viewProjection");
    cameraLocation_ = GetShaderLocation(shader_, "cameraPosition");
    heightsLocation_ = GetShaderLocation(shader_, "heights");
    textureLocation_ = GetShaderLocation(shader_, "texture0");
    areaLocation_ = GetShaderLocation(shader_, "terrainArea");
    gridLocation_ = GetShaderLocation(shader_, "terrainGrid");
    colorLocation_ = GetShaderLocation(shader_, "terrainColor");
    lodColorsLocation_ = GetShaderLocation(shader_, "lodColors");
    const glm::vec3 light = glm::normalize(lightDirection);
    SetShaderValue(shader_, GetShaderLocation(shader_, "lightDirection"), &light, SHADER_UNIFORM_VEC3);
    return true;
}

void TerrainRenderer::clear() {
    if (heightTexture_ != 0) {
        rlUnloadTexture(heightTexture_);
        heightTexture_ = 0;
    }
    if (fullGrid_.vaoId != 0) {
        UnloadMesh(fullGrid_);
        UnloadMesh(halfGrid_);
    }
    fullGrid_ = {};
    halfGrid_ = {};
    texture_ = {};
}

void TerrainRenderer::release() {
    clear();
    if (instanceBuffer_ != 0) {
        rlUnloadVertexBuffer(instanceBuffer_);
        instanceBuffer_ = 0;
        instanceCapacity_ = 0;
    }
    if (IsShaderValid(shader_)) {
        UnloadShader(shader_);
    }
    shader_ = {};
}

void TerrainRenderer::set(const TerrainDesc& desc, Texture2D texture) {
    clear();
    if (desc.heightmap.empty() || !IsShaderValid(shader_)) {
        return;
    }
    const Heightmap& map = desc.heightmap;
    heightTexture_ = rlLoadTexture(map.samples().data(), map.columns(), map.rows(), PIXELFORMAT_UNCOMPRESSED_R32, 1);
    rlTextureParameters(heightTexture_, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(heightTexture_, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
    lod_ = TerrainLod{map, desc.lod};
    heightmap_ = map;
    fullGrid_ = makeGrid(lod_.gridSize());
    halfGrid_ = makeGrid(lod_.gridSize() / 2);
    texture_ = texture;
    color_ = desc.color;
    area_ = {map.origin().x, map.origin().y, map.size().x, map.size().y};
    grid_ = {static_cast<float>(map.columns()), static_cast<float>(map.rows()), map.spacing()};
    spdlog::info("RaylibRenderBackend: terreno {}x{} amostras de {} m, {} níveis, pedaço de {} m", map.columns(), map.rows(),
                 map.spacing(), lod_.levels(), lod_.leafSize());
}

void TerrainRenderer::update(const TerrainPatch& patch) {
    if (!active() || patch.columns <= 0 || patch.rows <= 0 ||
        patch.samples.size() != static_cast<std::size_t>(patch.columns) * static_cast<std::size_t>(patch.rows) ||
        patch.column < 0 || patch.row < 0 || patch.column + patch.columns > heightmap_.columns() ||
        patch.row + patch.rows > heightmap_.rows()) {
        spdlog::warn("RaylibRenderBackend: mudança de relevo fora da grade; ignorada");
        return;
    }
    for (int r = 0; r < patch.rows; ++r) {
        for (int c = 0; c < patch.columns; ++c) {
            heightmap_.set(patch.column + c, patch.row + r,
                           patch.samples[static_cast<std::size_t>(r) * static_cast<std::size_t>(patch.columns) + static_cast<std::size_t>(c)]);
        }
    }
    rlUpdateTexture(heightTexture_, patch.column, patch.row, patch.columns, patch.rows, PIXELFORMAT_UNCOMPRESSED_R32,
                    patch.samples.data());
    lod_.refresh(heightmap_, patch.column, patch.row, patch.columns, patch.rows);
}

void TerrainRenderer::draw(const glm::mat4& viewProjection, glm::vec3 camera, bool lodColors, std::size_t& drawCalls,
                           std::size_t& triangles) {
    if (!active()) {
        return;
    }
    ZoneScopedN("TerrainRenderer::draw");
    lod_.select(camera, viewProjection, nodes_);
    if (nodes_.empty()) {
        return;
    }
    // Pedaços inteiros primeiro, quartos depois: um draw call por grade.
    std::stable_partition(nodes_.begin(), nodes_.end(),
                          [this](const TerrainNode& node) { return node.resolution == lod_.gridSize(); });
    instances_.clear();
    std::size_t fullCount = 0;
    for (const auto& node : nodes_) {
        instances_.push_back({{node.origin.x, node.origin.y, node.size, static_cast<float>(node.resolution)},
                              {node.morphStart, node.morphEnd, static_cast<float>(node.level), 0.0f}});
        fullCount += node.resolution == lod_.gridSize() ? 1 : 0;
    }
    if (instances_.size() > instanceCapacity_) {
        if (instanceBuffer_ != 0) {
            rlUnloadVertexBuffer(instanceBuffer_);
        }
        instanceCapacity_ = std::max(instances_.size(), instanceCapacity_ * 2);
        instanceBuffer_ = rlLoadVertexBuffer(nullptr, static_cast<int>(instanceCapacity_ * sizeof(NodeInstance)), true);
    }
    rlUpdateVertexBuffer(instanceBuffer_, instances_.data(), static_cast<int>(instances_.size() * sizeof(NodeInstance)), 0);

    rlEnableShader(shader_.id);
    rlSetUniformMatrix(viewProjectionLocation_, toRaylib(viewProjection));
    rlSetUniform(cameraLocation_, &camera, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(areaLocation_, &area_, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(gridLocation_, &grid_, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(colorLocation_, &color_, RL_SHADER_UNIFORM_VEC4, 1);
    const int lodColorsValue = lodColors ? 1 : 0;
    rlSetUniform(lodColorsLocation_, &lodColorsValue, RL_SHADER_UNIFORM_INT, 1);
    const int albedoUnit = 0;
    const int heightsUnit = 1;
    rlSetUniform(textureLocation_, &albedoUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    rlSetUniform(heightsLocation_, &heightsUnit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
    rlActiveTextureSlot(1);
    rlEnableTexture(heightTexture_);
    rlActiveTextureSlot(0);
    rlEnableTexture(texture_.id != 0 ? texture_.id : rlGetTextureIdDefault());

    const auto drawGrid = [&](const Mesh& grid, std::size_t first, std::size_t count) {
        if (count == 0) {
            return;
        }
        rlEnableVertexArray(grid.vaoId);
        rlEnableVertexBuffer(instanceBuffer_);
        constexpr int stride = sizeof(NodeInstance);
        const auto offset = static_cast<int>(first * sizeof(NodeInstance));
        for (const auto& [location, field] : {std::pair{nodeAreaLocation_, 0}, std::pair{nodeMorphLocation_, 1}}) {
            const auto index = static_cast<unsigned int>(location);
            rlSetVertexAttribute(index, 4, RL_FLOAT, false, stride, offset + field * static_cast<int>(sizeof(glm::vec4)));
            rlEnableVertexAttribute(index);
            rlSetVertexAttributeDivisor(index, 1);
        }
        rlDrawVertexArrayElementsInstanced(0, grid.triangleCount * 3, nullptr, static_cast<int>(count));
        ++drawCalls;
        triangles += static_cast<std::size_t>(grid.triangleCount) * count;
    };
    drawGrid(fullGrid_, 0, fullCount);
    drawGrid(halfGrid_, fullCount, instances_.size() - fullCount);

    rlActiveTextureSlot(1);
    rlDisableTexture();
    rlActiveTextureSlot(0);
}

} // namespace lkz::raylib_detail
