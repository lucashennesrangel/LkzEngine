#include "Images.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC // só neste arquivo: a raylib traz outra cópia do stb_image
#define STBI_NO_STDIO // o arquivo é lido aqui (std::ifstream aceita caminhos do Windows)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace lkz::nvrhi_detail {

std::optional<Image> loadImage(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        spdlog::error("Imagem: não abriu '{}'", path);
        return std::nullopt;
    }
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return decodeImage(bytes.data(), bytes.size(), path);
}

std::optional<Image> decodeImage(const void* bytes, std::size_t size, const std::string& name) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height,
                                          &channels, 4);
    if (data == nullptr) {
        spdlog::error("Imagem: formato não reconhecido em '{}' ({})", name, stbi_failure_reason());
        return std::nullopt;
    }
    Image image;
    image.width = width;
    image.height = height;
    image.pixels.assign(data, data + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(data);
    return image;
}

std::vector<Image> mipChain(Image image) {
    std::vector<Image> chain;
    chain.push_back(std::move(image));
    while (chain.back().width > 1 || chain.back().height > 1) {
        const Image& source = chain.back();
        Image next;
        next.width = std::max(1, source.width / 2);
        next.height = std::max(1, source.height / 2);
        next.pixels.resize(static_cast<std::size_t>(next.width) * static_cast<std::size_t>(next.height) * 4);
        for (int y = 0; y < next.height; ++y) {
            for (int x = 0; x < next.width; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(x * 2 + dx, source.width - 1);
                            const int sy = std::min(y * 2 + dy, source.height - 1);
                            sum += source.pixels[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(source.width) +
                                                  static_cast<std::size_t>(sx)) * 4 + static_cast<std::size_t>(channel)];
                        }
                    }
                    next.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(next.width) +
                                 static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(channel)] =
                        static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        chain.push_back(std::move(next));
    }
    return chain;
}

} // namespace lkz::nvrhi_detail
