#pragma once

#include <cstdint>
#include <vector>

namespace lkz {

/// Imagem RGBA de 8 bits criada pelo jogo ou por uma ferramenta (World::uploadTexture),
/// linha a linha a partir do topo.
struct TextureImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width x height x 4
};

/// Mudança num retângulo de uma textura já enviada (World::updateTexture): `rgba` tem
/// width x height pixels, a partir do pixel (x, y).
///
/// Os mipmaps são refeitos só quando `strokeEnd` é true (fim de um traço de pincel):
/// refazê-los a cada pedaço travaria em texturas grandes. Durante o traço, a imagem nova
/// aparece no nível mais nítido.
struct TexturePatch {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    bool strokeEnd = false;
};

} // namespace lkz
