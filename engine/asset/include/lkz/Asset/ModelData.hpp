#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/vec4.hpp>
#include <lkz/Render/MeshData.hpp>

namespace lkz::asset {

/// Imagem RGBA de 8 bits por canal, linha a linha a partir do topo.
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::string name; // nome no arquivo (só informativo)
    /// Imagem externa: caminho relativo ao .glb/.gltf de onde veio. Vazio = embutida.
    /// saveGlb grava a referência (e não os pixels) quando GlbOptions::keepExternal.
    std::string uri;
    /// Imagem externa lida de um arquivo: o caminho absoluto dela (a chave de um cache).
    std::filesystem::path file;
    /// Embutir sem perda (PNG) mesmo sem transparência: blocos de cor exata, por exemplo.
    bool lossless = false;

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
};

/// Para onde a pintura de uma face vai na exportação (Pintor de modelos).
enum class SurfaceTarget : std::uint8_t {
    Shared, // material da paleta, repetido em metros (imagem externa compartilhada)
    Unique, // atlas único do modelo
    Color,  // só cor (amostra pequena dentro do atlas único)
    Decal,  // uma imagem posta uma vez, sem repetir (janela, porta, letreiro)
};

enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };

/// Material no estilo glTF (metallic-roughness), com o que a engine usa.
struct MaterialData {
    std::string name;
    glm::vec4 baseColor{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    int image = -1; // índice em ModelData::images (-1 = só cor)
    bool doubleSided = false;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    /// Destino das faces deste material (extras.lkz.target no arquivo).
    SurfaceTarget target = SurfaceTarget::Unique;
    std::string shared;           // nome do material da paleta (Shared) ou do decalque (Decal)
    float metersPerRepeat = 0.0f; // destino Shared: metros cobertos por uma repetição da imagem
    std::string extras;           // JSON de "extras" do material, como veio do arquivo
};

/// Uma parte: a malha (no espaço do modelo, com as transformações dos nós já aplicadas)
/// e o material dela. `faceTarget` é opcional (um por triângulo).
struct ModelPart {
    std::string name;
    MeshData mesh;
    int material = -1; // índice em ModelData::materials (-1 = material padrão, branco)
    std::vector<SurfaceTarget> faceTarget;
};

/// Triângulos de uma malha (sem índices: cada três posições formam um).
[[nodiscard]] inline std::size_t triangleCount(const MeshData& mesh) {
    return (mesh.indices.empty() ? mesh.positions.size() : mesh.indices.size()) / 3;
}
/// Os três vértices do triângulo `triangle`.
[[nodiscard]] inline std::array<std::uint32_t, 3> triangleAt(const MeshData& mesh, std::size_t triangle) {
    if (mesh.indices.empty()) {
        const auto first = static_cast<std::uint32_t>(triangle * 3);
        return {first, first + 1, first + 2};
    }
    return {mesh.indices[triangle * 3], mesh.indices[triangle * 3 + 1], mesh.indices[triangle * 3 + 2]};
}

struct ModelData {
    std::vector<ModelPart> parts;
    std::vector<MaterialData> materials;
    std::vector<ImageData> images;
    std::string extras; // JSON de "asset.extras" (ex.: {"lkz": {...}}), como veio do arquivo

    [[nodiscard]] std::size_t triangleCount() const;
    [[nodiscard]] std::size_t vertexCount() const;
    /// Memória de vídeo das imagens embutidas, em bytes (RGBA8 com mipmaps: lado² × 4 × 4/3).
    [[nodiscard]] std::size_t estimatedTextureMemory() const;
};

} // namespace lkz::asset
