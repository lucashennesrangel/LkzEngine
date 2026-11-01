#include "lkz/Asset/Image.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include <glm/common.hpp>
#include <spdlog/spdlog.h>

// stb com escopo de arquivo: a raylib (e o backend NVRHI) trazem outras cópias.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace lkz::asset {

namespace {

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

std::size_t pixel(const ImageData& image, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
}

} // namespace

std::optional<ImageData> loadImage(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        spdlog::error("Imagem: não abriu '{}'", path.string());
        return std::nullopt;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto image = decodeImage(bytes.data(), bytes.size(), path.string());
    if (image) {
        image->name = path.filename().string();
    }
    return image;
}

std::optional<ImageData> decodeImage(const void* data, std::size_t size, const std::string& name) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &width, &height,
                                            &channels, 4);
    if (pixels == nullptr) {
        spdlog::error("Imagem: formato não reconhecido em '{}' ({})", name, stbi_failure_reason());
        return std::nullopt;
    }
    ImageData image;
    image.width = width;
    image.height = height;
    image.name = name;
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(pixels);
    return image;
}

std::vector<std::uint8_t> encodePng(const ImageData& image) {
    std::vector<std::uint8_t> out;
    stbi_write_png_to_func(appendBytes, &out, image.width, image.height, 4, image.rgba.data(), image.width * 4);
    return out;
}

std::vector<std::uint8_t> encodeJpeg(const ImageData& image, int quality) {
    // JPEG não tem alfa: grava só RGB.
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    for (std::size_t i = 0, j = 0; i < image.rgba.size(); i += 4, j += 3) {
        rgb[j] = image.rgba[i];
        rgb[j + 1] = image.rgba[i + 1];
        rgb[j + 2] = image.rgba[i + 2];
    }
    std::vector<std::uint8_t> out;
    stbi_write_jpg_to_func(appendBytes, &out, image.width, image.height, 3, rgb.data(), std::clamp(quality, 1, 100));
    return out;
}

bool saveImage(const std::filesystem::path& path, const ImageData& image, int jpegQuality) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<std::uint8_t> bytes =
        extension == ".jpg" || extension == ".jpeg" ? encodeJpeg(image, jpegQuality) : encodePng(image);
    if (bytes.empty()) {
        spdlog::error("Imagem: não consegui codificar '{}'", path.string());
        return false;
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

bool hasTransparency(const ImageData& image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] < 255) {
            return true;
        }
    }
    return false;
}

ImageData resize(const ImageData& source, int width, int height) {
    ImageData current = source;
    // Reduções grandes: médias de 2 x 2 até ficar a menos do dobro do alvo.
    while (current.width >= width * 2 && current.height >= height * 2) {
        ImageData half;
        half.width = current.width / 2;
        half.height = current.height / 2;
        half.rgba.resize(static_cast<std::size_t>(half.width) * static_cast<std::size_t>(half.height) * 4);
        for (int y = 0; y < half.height; ++y) {
            for (int x = 0; x < half.width; ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    const int sum = current.rgba[pixel(current, x * 2, y * 2) + c] + current.rgba[pixel(current, x * 2 + 1, y * 2) + c] +
                                    current.rgba[pixel(current, x * 2, y * 2 + 1) + c] + current.rgba[pixel(current, x * 2 + 1, y * 2 + 1) + c];
                    half.rgba[pixel(half, x, y) + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        current = std::move(half);
    }
    ImageData out;
    out.width = width;
    out.height = height;
    out.name = source.name;
    out.uri = source.uri;
    out.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const float sy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(current.height) / static_cast<float>(height) - 0.5f,
                                    0.0f, static_cast<float>(current.height - 1));
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, current.height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < width; ++x) {
            const float sx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(current.width) / static_cast<float>(width) - 0.5f,
                                        0.0f, static_cast<float>(current.width - 1));
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, current.width - 1);
            const float fx = sx - static_cast<float>(x0);
            for (std::size_t c = 0; c < 4; ++c) {
                const float top = current.rgba[pixel(current, x0, y0) + c] * (1.0f - fx) + current.rgba[pixel(current, x1, y0) + c] * fx;
                const float bottom = current.rgba[pixel(current, x0, y1) + c] * (1.0f - fx) + current.rgba[pixel(current, x1, y1) + c] * fx;
                out.rgba[pixel(out, x, y) + c] = static_cast<std::uint8_t>(std::lround(top * (1.0f - fy) + bottom * fy));
            }
        }
    }
    return out;
}

void dilate(ImageData& image, std::vector<std::uint8_t>& coverage, int pixels) {
    const std::size_t count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    if (coverage.size() != count) {
        return;
    }
    std::vector<std::size_t> grow;
    for (int pass = 0; pass < pixels; ++pass) {
        grow.clear();
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x);
                if (coverage[i] != 0) {
                    continue;
                }
                // Média dos vizinhos (8) já cobertos.
                int sum[4] = {0, 0, 0, 0};
                int found = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) {
                            continue;
                        }
                        const std::size_t n = static_cast<std::size_t>(ny) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(nx);
                        if (coverage[n] != 0) {
                            for (std::size_t c = 0; c < 4; ++c) {
                                sum[c] += image.rgba[n * 4 + c];
                            }
                            ++found;
                        }
                    }
                }
                if (found > 0) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        image.rgba[i * 4 + c] = static_cast<std::uint8_t>(sum[c] / found);
                    }
                    grow.push_back(i);
                }
            }
        }
        if (grow.empty()) {
            break;
        }
        for (const std::size_t i : grow) {
            coverage[i] = 1;
        }
    }
}

std::vector<ImageData> mipChain(const ImageData& image) {
    std::vector<ImageData> chain{image};
    while (chain.back().width > 1 || chain.back().height > 1) {
        const ImageData& from = chain.back();
        ImageData next;
        next.width = std::max(1, from.width / 2);
        next.height = std::max(1, from.height / 2);
        next.rgba.resize(static_cast<std::size_t>(next.width) * static_cast<std::size_t>(next.height) * 4);
        for (int y = 0; y < next.height; ++y) {
            for (int x = 0; x < next.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int sx = std::min(x * 2 + (k & 1), from.width - 1);
                        const int sy = std::min(y * 2 + (k >> 1), from.height - 1);
                        sum += from.rgba[(static_cast<std::size_t>(sy) * from.width + sx) * 4 + c];
                    }
                    next.rgba[(static_cast<std::size_t>(y) * next.width + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        chain.push_back(std::move(next));
    }
    return chain;
}

glm::vec4 sampleBilinear(const ImageData& image, const glm::vec2& pixel) {
    const auto wrap = [](int value, int size) {
        const int r = value % size;
        return r < 0 ? r + size : r;
    };
    const glm::vec2 p = pixel - 0.5f;
    const glm::vec2 base = glm::floor(p);
    const glm::vec2 f = p - base;
    const int x0 = static_cast<int>(base.x);
    const int y0 = static_cast<int>(base.y);
    const auto fetch = [&](int x, int y) {
        const std::uint8_t* c =
            &image.rgba[(static_cast<std::size_t>(wrap(y, image.height)) * image.width + wrap(x, image.width)) * 4];
        return glm::vec4{c[0], c[1], c[2], c[3]};
    };
    const glm::vec4 top = glm::mix(fetch(x0, y0), fetch(x0 + 1, y0), f.x);
    const glm::vec4 bottom = glm::mix(fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1), f.x);
    return glm::mix(top, bottom, f.y) / 255.0f;
}

} // namespace lkz::asset
