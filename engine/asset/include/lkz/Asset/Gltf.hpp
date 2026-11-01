#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include <lkz/Asset/ModelData.hpp>

namespace lkz::asset {

struct GltfLoadOptions {
    /// false: imagens externas ficam só com `uri` e `file` (sem pixels), para quem as
    /// carrega por conta própria, uma vez por arquivo.
    bool externalImagePixels = true;
};

/// Lê um glTF (.glb, ou .gltf com os arquivos ao lado). Cada primitiva de triângulos vira
/// uma parte, com as transformações dos nós já aplicadas (posição e normal). Imagens
/// embutidas são decodificadas; externas também, guardando a `uri` original e o arquivo.
/// Os destinos gravados em extras.lkz voltam em MaterialData (target, shared,
/// metersPerRepeat). Nada se o arquivo não abre (motivo no log).
[[nodiscard]] std::optional<ModelData> loadGltf(const std::filesystem::path& path, const GltfLoadOptions& options = {});

/// Só o JSON do arquivo: para cada material, o arquivo da imagem de cor base quando ela é
/// externa (absoluto), ou vazio. Serve para reaproveitar uma imagem já carregada.
[[nodiscard]] std::vector<std::filesystem::path> materialImageFiles(const std::filesystem::path& path);

struct GlbOptions {
    int jpegQuality = 90;     // imagens sem transparência viram JPEG
    bool forcePng = false;    // sempre PNG (sem perda)
    bool keepExternal = true; // imagem com `uri` continua externa (não é embutida)
};

/// Grava um .glb (glTF 2.0 binário): uma malha e um nó por parte, imagens embutidas (PNG
/// se houver transparência, senão JPEG), amostrador com repetição e mipmaps. Índices de
/// 16 bits quando a parte tem até 65 535 vértices (o carregador da raylib converte os de
/// 32 bits e pode perder dados). Grava num temporário e troca. false se falhar (log).
bool saveGlb(const std::filesystem::path& path, const ModelData& model, const GlbOptions& options = {});

} // namespace lkz::asset
