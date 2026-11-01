#include "lkz/Asset/Bake.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <spdlog/spdlog.h>
#include <lkz/Asset/Image.hpp>
#include <spdlog/fmt/fmt.h>

namespace lkz::asset {

namespace {

float cross2(const glm::vec2& a, const glm::vec2& b) {
    return a.x * b.y - a.y * b.x;
}

} // namespace

ImageData transferTexture(const ModelData& source, const UnwrappedModel& unwrapped, int material, int padding) {
    ImageData image;
    if (material < 0 || material >= static_cast<int>(unwrapped.imageSize.size())) {
        return image;
    }
    const glm::ivec2 size = unwrapped.imageSize[static_cast<std::size_t>(material)];
    image.width = size.x;
    image.height = size.y;
    image.name = unwrapped.model.materials[static_cast<std::size_t>(material)].name;
    image.rgba.assign(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4, 0);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y), 0);
    std::vector<std::optional<std::vector<ImageData>>> mips(source.images.size());
    const glm::vec2 sizeF{static_cast<float>(size.x), static_cast<float>(size.y)};
    constexpr glm::vec2 offsets[4] = {{-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f}};

    // Primeiro, o que cada triângulo precisa; depois, os texels, em faixas de linhas (uma
    // por thread). Cada faixa vê os triângulos na mesma ordem: o resultado é o de uma thread só.
    struct Raster {
        glm::vec2 q0{0.0f};
        glm::vec2 e1{0.0f};
        glm::vec2 e2{0.0f};
        float inverse = 0.0f;
        glm::vec3 dbdx{0.0f};
        glm::vec3 dbdy{0.0f};
        float heights[3] = {0.0f, 0.0f, 0.0f};
        glm::vec2 s[3] = {glm::vec2{0.0f}, glm::vec2{0.0f}, glm::vec2{0.0f}};
        const ImageData* level = nullptr;
        glm::vec2 levelScale{1.0f};
        glm::vec4 tint{1.0f};
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::vector<Raster> rasters;

    for (std::size_t p = 0; p < unwrapped.model.parts.size(); ++p) {
        const ModelPart& part = unwrapped.model.parts[p];
        if (part.material != material) {
            continue;
        }
        const std::size_t triangles = triangleCount(part.mesh);
        for (std::size_t t = 0; t < triangles; ++t) {
            const TriangleRef ref = unwrapped.source[p][t];
            const ModelPart& from = source.parts[static_cast<std::size_t>(ref.part)];
            const MaterialData* fromMaterial = from.material >= 0 && from.material < static_cast<int>(source.materials.size())
                                                   ? &source.materials[static_cast<std::size_t>(from.material)]
                                                   : nullptr;
            const glm::vec4 tint = fromMaterial != nullptr ? fromMaterial->baseColor : glm::vec4{1.0f};
            const std::vector<ImageData>* chain = nullptr;
            if (fromMaterial != nullptr && fromMaterial->image >= 0 &&
                fromMaterial->image < static_cast<int>(source.images.size()) &&
                !source.images[static_cast<std::size_t>(fromMaterial->image)].empty() &&
                from.mesh.uvs.size() == from.mesh.positions.size()) {
                auto& slot = mips[static_cast<std::size_t>(fromMaterial->image)];
                if (!slot) {
                    slot = mipChain(source.images[static_cast<std::size_t>(fromMaterial->image)]);
                }
                chain = &*slot;
            }

            const auto to = triangleAt(part.mesh, t);
            const glm::vec2 q[3] = {part.mesh.uvs[to[0]] * sizeF, part.mesh.uvs[to[1]] * sizeF, part.mesh.uvs[to[2]] * sizeF};
            const glm::vec2 e1 = q[1] - q[0];
            const glm::vec2 e2 = q[2] - q[0];
            const float area = cross2(e1, e2);
            if (std::abs(area) < 1e-8f) {
                continue;
            }
            const float inverse = 1.0f / area;
            // As baricêntricas variam linearmente com o texel: derivadas por texel.
            const glm::vec3 dbdx{(-e2.y + e1.y) * inverse, e2.y * inverse, -e1.y * inverse};
            const glm::vec3 dbdy{(e2.x - e1.x) * inverse, -e2.x * inverse, e1.x * inverse};

            glm::vec2 s[3] = {glm::vec2{0.0f}, glm::vec2{0.0f}, glm::vec2{0.0f}};
            const ImageData* level = nullptr;
            glm::vec2 levelScale{1.0f};
            if (chain != nullptr) {
                const auto from3 = triangleAt(from.mesh, static_cast<std::size_t>(ref.triangle));
                const glm::vec2 sourceSize{static_cast<float>(chain->front().width), static_cast<float>(chain->front().height)};
                for (int k = 0; k < 3; ++k) {
                    s[k] = from.mesh.uvs[from3[static_cast<std::size_t>(k)]] * sourceSize;
                }
                // Quantos pixels da imagem original cabem num texel novo (meio texel por
                // amostra): escolhe o mipmap desse tamanho.
                const glm::vec2 dsdx = s[0] * dbdx.x + s[1] * dbdx.y + s[2] * dbdx.z;
                const glm::vec2 dsdy = s[0] * dbdy.x + s[1] * dbdy.y + s[2] * dbdy.z;
                const float footprint = std::max(glm::length(dsdx), glm::length(dsdy)) * 0.5f;
                const int index = std::clamp(static_cast<int>(std::floor(std::log2(std::max(footprint, 1.0f)))), 0,
                                             static_cast<int>(chain->size()) - 1);
                level = &(*chain)[static_cast<std::size_t>(index)];
                levelScale = glm::vec2{static_cast<float>(level->width), static_cast<float>(level->height)} / sourceSize;
            }

            // Texels de centro dentro do triângulo ou a até meio texel das bordas.
            float heights[3];
            for (int i = 0; i < 3; ++i) {
                const float edge = glm::length(q[(i + 2) % 3] - q[(i + 1) % 3]);
                heights[i] = edge > 0.0f ? std::abs(area) / edge : 0.0f;
            }
            const glm::vec2 lo = glm::min(q[0], glm::min(q[1], q[2])) - 0.5f;
            const glm::vec2 hi = glm::max(q[0], glm::max(q[1], q[2])) + 0.5f;
            Raster raster;
            raster.q0 = q[0];
            raster.e1 = e1;
            raster.e2 = e2;
            raster.inverse = inverse;
            raster.dbdx = dbdx;
            raster.dbdy = dbdy;
            std::copy_n(heights, 3, raster.heights);
            std::copy_n(s, 3, raster.s);
            raster.level = level;
            raster.levelScale = levelScale;
            raster.tint = tint;
            raster.x0 = std::max(static_cast<int>(std::floor(lo.x - 0.5f)), 0);
            raster.y0 = std::max(static_cast<int>(std::floor(lo.y - 0.5f)), 0);
            raster.x1 = std::min(static_cast<int>(std::ceil(hi.x - 0.5f)) + 1, size.x);
            raster.y1 = std::min(static_cast<int>(std::ceil(hi.y - 0.5f)) + 1, size.y);
            rasters.push_back(raster);
        }
    }

    const auto band = [&](int rowBegin, int rowEnd) {
        for (const Raster& r : rasters) {
            const int y0 = std::max(r.y0, rowBegin);
            const int y1 = std::min(r.y1, rowEnd);
            for (int y = y0; y < y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const glm::vec2 d = glm::vec2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f} - r.q0;
                    const float b1 = cross2(d, r.e2) * r.inverse;
                    const float b2 = cross2(r.e1, d) * r.inverse;
                    const glm::vec3 b{1.0f - b1 - b2, b1, b2};
                    if (b.x * r.heights[0] < -0.5f || b.y * r.heights[1] < -0.5f || b.z * r.heights[2] < -0.5f) {
                        continue;
                    }
                    glm::vec4 sum{0.0f};
                    for (const glm::vec2& o : offsets) {
                        glm::vec3 w = glm::max(b + r.dbdx * o.x + r.dbdy * o.y, glm::vec3{0.0f});
                        w /= std::max(w.x + w.y + w.z, 1e-8f);
                        sum += r.level != nullptr
                                   ? sampleBilinear(*r.level, (r.s[0] * w.x + r.s[1] * w.y + r.s[2] * w.z) * r.levelScale)
                                   : glm::vec4{1.0f};
                    }
                    const glm::vec4 color = glm::clamp(sum * 0.25f * r.tint, glm::vec4{0.0f}, glm::vec4{1.0f});
                    std::uint8_t* pixel = &image.rgba[(static_cast<std::size_t>(y) * size.x + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        pixel[c] = static_cast<std::uint8_t>(std::lround(color[c] * 255.0f));
                    }
                    coverage[static_cast<std::size_t>(y) * size.x + x] = 1;
                }
            }
        }
    };
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    const int rows = (size.y + threads - 1) / threads;
    std::vector<std::thread> workers;
    for (int i = 1; i < threads; ++i) {
        workers.emplace_back(band, i * rows, std::min((i + 1) * rows, size.y));
    }
    band(0, std::min(rows, size.y));
    for (std::thread& worker : workers) {
        worker.join();
    }

    // Bordas das cartas para fora (mipmaps), e o resto com a cor média.
    dilate(image, coverage, std::max(padding, 1) * 2);
    glm::dvec4 average{0.0};
    std::size_t covered = 0;
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        if (coverage[i] != 0) {
            average += glm::dvec4{image.rgba[i * 4], image.rgba[i * 4 + 1], image.rgba[i * 4 + 2], image.rgba[i * 4 + 3]};
            ++covered;
        }
    }
    if (covered > 0) {
        average /= static_cast<double>(covered);
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            if (coverage[i] == 0) {
                for (int c = 0; c < 4; ++c) {
                    image.rgba[i * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::lround(average[c]));
                }
            }
        }
    }
    return image;
}

std::optional<ModelData> bakeUnique(const ModelData& source, const UnwrapOptions& options, std::string* report) {
    const auto start = std::chrono::steady_clock::now();
    auto unwrapped = unwrapUnique(source, options);
    if (!unwrapped) {
        return std::nullopt;
    }
    const auto unwrappedAt = std::chrono::steady_clock::now();
    std::vector<ImageData> images;
    std::string sizes;
    for (std::size_t m = 0; m < unwrapped->model.materials.size(); ++m) {
        images.push_back(transferTexture(source, *unwrapped, static_cast<int>(m), options.padding));
        const ImageData& image = images.back();
        sizes += fmt::format("{}{}", sizes.empty() ? "" : ", ",
                             image.width == image.height ? fmt::format("{}²", image.width)
                                                         : fmt::format("{}×{}", image.width, image.height));
    }
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    spdlog::info("bakeUnique: UV em {:.0f} ms, imagens em {:.0f} ms", ms(start, unwrappedAt),
                 ms(unwrappedAt, std::chrono::steady_clock::now()));
    ModelData out = std::move(unwrapped->model);
    for (std::size_t m = 0; m < out.materials.size(); ++m) {
        out.materials[m].image = static_cast<int>(m);
    }
    out.images = std::move(images);
    if (report != nullptr) {
        *report = fmt::format("{} {} · {} {} ({}) · {:.0f} px/m", unwrapped->charts,
                              unwrapped->charts == 1 ? "carta" : "cartas", out.images.size(),
                              out.images.size() == 1 ? "imagem" : "imagens", sizes, unwrapped->texelsPerMeter);
    }
    return out;
}

} // namespace lkz::asset
