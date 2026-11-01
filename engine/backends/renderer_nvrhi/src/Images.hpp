#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lkz::nvrhi_detail {

/// Imagem RGBA de 8 bits por canal, linha a linha a partir do topo.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

/// PNG, JPEG, BMP, TGA... (stb_image). Nada se o arquivo não abre (motivo no log).
[[nodiscard]] std::optional<Image> loadImage(const std::string& path);

/// Imagem já na memória (ex.: embutida num modelo); `name` só aparece no log.
[[nodiscard]] std::optional<Image> decodeImage(const void* data, std::size_t size, const std::string& name);

/// A imagem e as reduções pela metade até 1 x 1 (média de 2 x 2, como o glGenerateMipmap).
[[nodiscard]] std::vector<Image> mipChain(Image image);

} // namespace lkz::nvrhi_detail
