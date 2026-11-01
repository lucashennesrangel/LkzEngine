#include "lkz/Terrain/Heightmap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <spdlog/spdlog.h>

namespace lkz {

namespace {

constexpr char magic[4] = {'L', 'K', 'Z', 'H'};
constexpr std::uint32_t fileVersion = 1;

struct FileHeader {
    char magic[4];
    std::uint32_t version;
    std::int32_t columns;
    std::int32_t rows;
    float spacing;
    float originX;
    float originZ;
    float minHeight;
    float maxHeight;
};

} // namespace

Heightmap::Heightmap(int columns, int rows, float spacing, glm::vec2 origin, float height)
    : columns_{std::max(columns, 2)}
    , rows_{std::max(rows, 2)}
    , spacing_{std::max(spacing, 1e-3f)}
    , origin_{origin}
    , samples_(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), height) {}

Heightmap Heightmap::covering(glm::vec2 size, float spacing) {
    const int columns = static_cast<int>(std::ceil(size.x / spacing)) + 1;
    const int rows = static_cast<int>(std::ceil(size.y / spacing)) + 1;
    const glm::vec2 covered{static_cast<float>(columns - 1) * spacing, static_cast<float>(rows - 1) * spacing};
    return Heightmap{columns, rows, spacing, -covered * 0.5f};
}

glm::vec2 Heightmap::size() const {
    return {static_cast<float>(columns_ - 1) * spacing_, static_cast<float>(rows_ - 1) * spacing_};
}

float Heightmap::at(int column, int row) const {
    if (samples_.empty()) {
        return 0.0f;
    }
    column = std::clamp(column, 0, columns_ - 1);
    row = std::clamp(row, 0, rows_ - 1);
    return samples_[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_) + static_cast<std::size_t>(column)];
}

void Heightmap::set(int column, int row, float height) {
    if (column >= 0 && row >= 0 && column < columns_ && row < rows_) {
        samples_[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_) + static_cast<std::size_t>(column)] = height;
    }
}

float Heightmap::heightAt(glm::vec2 point) const {
    if (samples_.empty()) {
        return 0.0f;
    }
    const glm::vec2 grid = glm::clamp((point - origin_) / spacing_, glm::vec2{0.0f},
                                      glm::vec2{static_cast<float>(columns_ - 1), static_cast<float>(rows_ - 1)});
    const int c = std::min(static_cast<int>(grid.x), columns_ - 2);
    const int r = std::min(static_cast<int>(grid.y), rows_ - 2);
    const float fx = grid.x - static_cast<float>(c);
    const float fz = grid.y - static_cast<float>(r);
    const float top = glm::mix(at(c, r), at(c + 1, r), fx);
    const float bottom = glm::mix(at(c, r + 1), at(c + 1, r + 1), fx);
    return glm::mix(top, bottom, fz);
}

glm::vec3 Heightmap::normalAt(glm::vec2 point) const {
    const float left = heightAt(point - glm::vec2{spacing_, 0.0f});
    const float right = heightAt(point + glm::vec2{spacing_, 0.0f});
    const float back = heightAt(point - glm::vec2{0.0f, spacing_});
    const float front = heightAt(point + glm::vec2{0.0f, spacing_});
    return glm::normalize(glm::vec3{left - right, 2.0f * spacing_, back - front});
}

std::pair<float, float> Heightmap::range() const {
    if (samples_.empty()) {
        return {0.0f, 0.0f};
    }
    const auto [low, high] = std::minmax_element(samples_.begin(), samples_.end());
    return {*low, *high};
}

std::optional<glm::vec3> Heightmap::raycast(glm::vec3 origin, glm::vec3 direction, float maxDistance) const {
    if (samples_.empty() || glm::length(direction) < 1e-6f) {
        return std::nullopt;
    }
    direction = glm::normalize(direction);
    // Recorta o raio na caixa da grade (XZ do retângulo, Y entre a menor e a maior altura).
    const auto [lowY, highY] = range();
    const glm::vec3 boxMin{origin_.x, lowY - 0.01f, origin_.y};
    const glm::vec3 boxMax = glm::vec3{origin_.x, highY + 0.01f, origin_.y} + glm::vec3{size().x, 0.0f, size().y};
    float enter = 0.0f;
    float exit = maxDistance;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-9f) {
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float t0 = (boxMin[axis] - origin[axis]) / direction[axis];
        float t1 = (boxMax[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        exit = std::min(exit, t1);
    }
    if (enter > exit) {
        return std::nullopt;
    }

    const auto above = [&](float t) {
        const glm::vec3 p = origin + direction * t;
        return p.y - heightAt({p.x, p.z});
    };
    const float step = spacing_ * 0.5f;
    float previous = enter;
    if (above(previous) <= 0.0f) {
        return origin + direction * previous; // começa embaixo da superfície
    }
    for (float t = enter + step; t <= exit + step; t += step) {
        const float current = std::min(t, exit);
        const float gap = above(current);
        if (gap <= 0.0f) {
            float a = previous;
            float b = current;
            for (int i = 0; i < 16; ++i) {
                const float middle = 0.5f * (a + b);
                if (above(middle) > 0.0f) {
                    a = middle;
                } else {
                    b = middle;
                }
            }
            return origin + direction * b;
        }
        previous = current;
        if (current >= exit) {
            break;
        }
    }
    return std::nullopt;
}

bool Heightmap::save(const std::filesystem::path& path) const {
    const auto [low, high] = range();
    FileHeader header{};
    std::memcpy(header.magic, magic, sizeof magic);
    header.version = fileVersion;
    header.columns = columns_;
    header.rows = rows_;
    header.spacing = spacing_;
    header.originX = origin_.x;
    header.originZ = origin_.y;
    header.minHeight = low;
    header.maxHeight = high;
    const float scale = high > low ? 65535.0f / (high - low) : 0.0f;
    std::vector<std::uint16_t> quantized(samples_.size());
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        quantized[i] = static_cast<std::uint16_t>(std::lround((samples_[i] - low) * scale));
    }

    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path{path}.concat(".tmp");
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(&header), sizeof header);
        file.write(reinterpret_cast<const char*>(quantized.data()),
                   static_cast<std::streamsize>(quantized.size() * sizeof(std::uint16_t)));
        if (!file) {
            spdlog::error("Heightmap: não foi possível gravar '{}'", temporary.string());
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        spdlog::error("Heightmap: não foi possível substituir '{}': {}", path.string(), error.message());
        return false;
    }
    return true;
}

std::optional<Heightmap> Heightmap::load(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    FileHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof header);
    if (!file || std::memcmp(header.magic, magic, sizeof magic) != 0 || header.version != fileVersion ||
        header.columns < 2 || header.rows < 2) {
        spdlog::warn("Heightmap: '{}' não é um arquivo de relevo válido", path.string());
        return std::nullopt;
    }
    Heightmap heightmap{header.columns, header.rows, header.spacing, {header.originX, header.originZ}};
    std::vector<std::uint16_t> quantized(heightmap.samples_.size());
    file.read(reinterpret_cast<char*>(quantized.data()), static_cast<std::streamsize>(quantized.size() * sizeof(std::uint16_t)));
    if (!file) {
        spdlog::warn("Heightmap: '{}' terminou antes do esperado", path.string());
        return std::nullopt;
    }
    const float step = (header.maxHeight - header.minHeight) / 65535.0f;
    for (std::size_t i = 0; i < quantized.size(); ++i) {
        heightmap.samples_[i] = header.minHeight + static_cast<float>(quantized[i]) * step;
    }
    return heightmap;
}

} // namespace lkz
