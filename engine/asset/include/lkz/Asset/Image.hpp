#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <lkz/Asset/ModelData.hpp>

namespace lkz::asset {

/// PNG, JPEG, BMP, TGA... (stb_image), sempre em RGBA. Nada se não abre (motivo no log).
[[nodiscard]] std::optional<ImageData> loadImage(const std::filesystem::path& path);
/// Imagem já na memória; `name` só aparece no log.
[[nodiscard]] std::optional<ImageData> decodeImage(const void* data, std::size_t size, const std::string& name);

[[nodiscard]] std::vector<std::uint8_t> encodePng(const ImageData& image);
[[nodiscard]] std::vector<std::uint8_t> encodeJpeg(const ImageData& image, int quality = 90);
/// Grava pela extensão do arquivo (.png, .jpg/.jpeg).
bool saveImage(const std::filesystem::path& path, const ImageData& image, int jpegQuality = 90);

/// Algum pixel não é totalmente opaco?
[[nodiscard]] bool hasTransparency(const ImageData& image);
/// Redimensiona (bilinear; reduções grandes fazem médias de 2 x 2 antes).
[[nodiscard]] ImageData resize(const ImageData& image, int width, int height);
/// Estende as ilhas pintadas para fora (padding): cada pixel com `coverage` 0 recebe a cor
/// do vizinho coberto mais próximo, até `pixels` de distância. Assim os mipmaps não puxam
/// o fundo para dentro das bordas. `coverage` tem um byte por pixel e é atualizado.
void dilate(ImageData& image, std::vector<std::uint8_t>& coverage, int pixels);

/// Mipmaps por média de 2 x 2, até 1 x 1; o nível 0 é a própria imagem.
[[nodiscard]] std::vector<ImageData> mipChain(const ImageData& image);
/// Cor bilinear (0..1) com repetição nos dois eixos; `pixel` em pixels da imagem, com os
/// centros em +0,5.
[[nodiscard]] glm::vec4 sampleBilinear(const ImageData& image, const glm::vec2& pixel);

} // namespace lkz::asset
