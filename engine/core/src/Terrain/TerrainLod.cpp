#include "lkz/Terrain/TerrainLod.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace lkz {

namespace {

/// Os 6 planos do volume visível (Gribb e Hartmann), normalizados, apontando para dentro.
std::array<glm::vec4, 6> frustumPlanes(const glm::mat4& m) {
    const auto row = [&m](int i) { return glm::vec4{m[0][i], m[1][i], m[2][i], m[3][i]}; };
    std::array<glm::vec4, 6> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1),
                                    row(3) - row(1), row(3) + row(2), row(3) - row(2)};
    for (auto& plane : planes) {
        plane /= glm::length(glm::vec3{plane});
    }
    return planes;
}

bool boxVisible(const glm::vec4* planes, glm::vec3 low, glm::vec3 high) {
    for (int i = 0; i < 6; ++i) {
        const glm::vec3 normal{planes[i]};
        // O canto da caixa mais à frente do plano.
        const glm::vec3 corner{normal.x >= 0.0f ? high.x : low.x, normal.y >= 0.0f ? high.y : low.y,
                               normal.z >= 0.0f ? high.z : low.z};
        if (glm::dot(normal, corner) + planes[i].w < 0.0f) {
            return false;
        }
    }
    return true;
}

bool boxTouchesSphere(glm::vec3 low, glm::vec3 high, glm::vec3 center, float radius) {
    const glm::vec3 nearest = glm::clamp(center, low, high);
    const glm::vec3 offset = nearest - center;
    return glm::dot(offset, offset) <= radius * radius;
}

} // namespace

TerrainLod::TerrainLod(const Heightmap& heightmap, TerrainLodSettings settings)
    : settings_{settings} {
    settings_.gridSize = std::max(2, settings_.gridSize / 2 * 2); // par: os quartos usam metade
    origin_ = heightmap.origin();
    extent_ = heightmap.size();
    leafSize_ = static_cast<float>(settings_.gridSize) * heightmap.spacing();

    // Níveis até um nó cobrir o lado maior do mapa.
    float size = leafSize_;
    do {
        Level level;
        level.size = size;
        level.columns = std::max(1, static_cast<int>(std::ceil(extent_.x / size)));
        level.rows = std::max(1, static_cast<int>(std::ceil(extent_.y / size)));
        level.heights.assign(static_cast<std::size_t>(level.columns) * static_cast<std::size_t>(level.rows), glm::vec2{0.0f});
        levels_.push_back(std::move(level));
        size *= 2.0f;
    } while (levels_.back().size < std::max(extent_.x, extent_.y));

    float range = settings_.firstRange > 0.0f ? settings_.firstRange : leafSize_ * 2.5f;
    for (std::size_t i = 0; i < levels_.size(); ++i) {
        ranges_.push_back(range);
        range *= 2.0f;
    }
    refresh(heightmap, 0, 0, heightmap.columns(), heightmap.rows());
}

glm::vec2 TerrainLod::nodeHeights(int level, int x, int z) const {
    const Level& l = levels_[static_cast<std::size_t>(level)];
    if (x < 0 || z < 0 || x >= l.columns || z >= l.rows) {
        return {1e30f, -1e30f};
    }
    return l.heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(l.columns) + static_cast<std::size_t>(x)];
}

void TerrainLod::refresh(const Heightmap& heightmap, int column, int row, int columns, int rows) {
    if (levels_.empty()) {
        return;
    }
    // Nós do nível 0 tocados pela região (cada nó inclui as amostras das suas bordas).
    Level& leaves = levels_.front();
    const int perNode = settings_.gridSize;
    const int x0 = std::clamp((column - 1) / perNode, 0, leaves.columns - 1);
    const int z0 = std::clamp((row - 1) / perNode, 0, leaves.rows - 1);
    const int x1 = std::clamp((column + columns + 1) / perNode, 0, leaves.columns - 1);
    const int z1 = std::clamp((row + rows + 1) / perNode, 0, leaves.rows - 1);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            glm::vec2 heights{1e30f, -1e30f};
            for (int r = z * perNode; r <= (z + 1) * perNode; ++r) {
                for (int c = x * perNode; c <= (x + 1) * perNode; ++c) {
                    const float h = heightmap.at(c, r);
                    heights = {std::min(heights.x, h), std::max(heights.y, h)};
                }
            }
            leaves.heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(leaves.columns) + static_cast<std::size_t>(x)] = heights;
        }
    }
    // Sobe: cada nó junta os quatro filhos.
    int ax0 = x0;
    int az0 = z0;
    int ax1 = x1;
    int az1 = z1;
    for (std::size_t level = 1; level < levels_.size(); ++level) {
        ax0 /= 2;
        az0 /= 2;
        ax1 /= 2;
        az1 /= 2;
        Level& current = levels_[level];
        for (int z = az0; z <= std::min(az1, current.rows - 1); ++z) {
            for (int x = ax0; x <= std::min(ax1, current.columns - 1); ++x) {
                glm::vec2 heights{1e30f, -1e30f};
                for (int j = 0; j < 2; ++j) {
                    for (int i = 0; i < 2; ++i) {
                        const glm::vec2 child = nodeHeights(static_cast<int>(level) - 1, x * 2 + i, z * 2 + j);
                        heights = {std::min(heights.x, child.x), std::max(heights.y, child.y)};
                    }
                }
                current.heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(current.columns) + static_cast<std::size_t>(x)] = heights;
            }
        }
    }
}

void TerrainLod::select(glm::vec3 cameraPosition, const glm::mat4& viewProjection, std::vector<TerrainNode>& out) const {
    out.clear();
    if (levels_.empty()) {
        return;
    }
    const auto planes = frustumPlanes(viewProjection);
    const int top = static_cast<int>(levels_.size()) - 1;
    const Level& roots = levels_.back();
    for (int z = 0; z < roots.rows; ++z) {
        for (int x = 0; x < roots.columns; ++x) {
            selectNode(top, x, z, cameraPosition, planes.data(), out);
        }
    }
}

void TerrainLod::selectNode(int level, int x, int z, glm::vec3 camera, const glm::vec4* planes,
                            std::vector<TerrainNode>& out) const {
    const Level& l = levels_[static_cast<std::size_t>(level)];
    const glm::vec2 corner = origin_ + glm::vec2{static_cast<float>(x), static_cast<float>(z)} * l.size;
    const glm::vec2 heights = nodeHeights(level, x, z);
    if (heights.x > heights.y) {
        return; // fora do mapa
    }
    const glm::vec2 farCorner = glm::min(corner + l.size, origin_ + extent_);
    const glm::vec3 low{corner.x, heights.x, corner.y};
    const glm::vec3 high{farCorner.x, heights.y, farCorner.y};
    if (!boxVisible(planes, low, high)) {
        return;
    }

    const auto emit = [&](glm::vec2 nodeOrigin, float size, int resolution) {
        TerrainNode node;
        node.origin = nodeOrigin;
        node.size = size;
        node.resolution = resolution;
        node.level = level;
        node.morphEnd = ranges_[static_cast<std::size_t>(level)];
        const float previous = level > 0 ? ranges_[static_cast<std::size_t>(level) - 1] : 0.0f;
        node.morphStart = previous + (node.morphEnd - previous) * settings_.morphStartRatio;
        out.push_back(node);
    };

    // Nível 0, ou nenhum filho perto o bastante para o nível de baixo: o nó inteiro aqui.
    if (level == 0 || !boxTouchesSphere(low, high, camera, ranges_[static_cast<std::size_t>(level) - 1])) {
        emit(corner, l.size, settings_.gridSize);
        return;
    }
    // Filhos perto o bastante descem de nível; os outros quartos ficam aqui, com metade da
    // grade (mesmo espaçamento de vértices do nível).
    const float half = l.size * 0.5f;
    const float childRange = ranges_[static_cast<std::size_t>(level) - 1];
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int cx = x * 2 + i;
            const int cz = z * 2 + j;
            const glm::vec2 childHeights = nodeHeights(level - 1, cx, cz);
            if (childHeights.x > childHeights.y) {
                continue; // quarto fora do mapa
            }
            const glm::vec2 childCorner = corner + glm::vec2{static_cast<float>(i), static_cast<float>(j)} * half;
            const glm::vec2 childFar = glm::min(childCorner + half, origin_ + extent_);
            const glm::vec3 childLow{childCorner.x, childHeights.x, childCorner.y};
            const glm::vec3 childHigh{childFar.x, childHeights.y, childFar.y};
            if (boxTouchesSphere(childLow, childHigh, camera, childRange)) {
                selectNode(level - 1, cx, cz, camera, planes, out);
            } else if (boxVisible(planes, childLow, childHigh)) {
                emit(childCorner, half, settings_.gridSize / 2);
            }
        }
    }
}

} // namespace lkz
