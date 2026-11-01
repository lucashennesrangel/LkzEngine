#pragma once

#include <optional>
#include <string>

#include <lkz/Asset/ModelData.hpp>
#include <lkz/Asset/Unwrap.hpp>

namespace lkz::asset {

/// A imagem do material `material` do modelo desembrulhado, com as cores do original
/// (imagem x cor base, lidas pelo UV antigo, com repetição). Cada texel é a média de
/// 2 x 2 amostras, num nível de mipmap da imagem original próximo do tamanho do texel
/// (sem serrilhado quando a densidade cai). No fim, as bordas das cartas se estendem
/// `padding` texels, e o fundo que sobra recebe a cor média.
[[nodiscard]] ImageData transferTexture(const ModelData& source, const UnwrappedModel& unwrapped, int material,
                                        int padding);

/// unwrapUnique + transferTexture: o modelo com UV único e uma imagem por grupo de
/// materiais. `report` (opcional) resume o que saiu.
[[nodiscard]] std::optional<ModelData> bakeUnique(const ModelData& source, const UnwrapOptions& options = {},
                                                  std::string* report = nullptr);

} // namespace lkz::asset
