#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <lkz/Asset/ModelData.hpp>

namespace painter {

/// Uma imagem da biblioteca: material repetível (paleta) ou decalque.
struct PaletteMaterial {
    std::string id;              // nome do arquivo sem extensão: a chave gravada no .glb
    std::string name;            // para mostrar ("nome" no .json; senão o id)
    std::filesystem::path image; // arquivo da imagem (absoluto)
    float metersPerRepeat = 1.0f; // paleta: metros cobertos por uma repetição
    float widthMeters = 1.0f;     // decalque: largura ao carimbar com um clique
    bool transparent = false;     // algum pixel não é opaco
    std::vector<lkz::asset::ImageData> mips; // nível 0 = a imagem
};

/// As imagens de uma pasta do jogo, cada uma com um .json opcional de mesmo nome ao lado:
///   paleta:     {"nome": "Tijolo", "metrosPorRepeticao": 1.0}
///   decalques:  {"nome": "Janela", "larguraMetros": 1.2}
class Palette {
public:
    enum class Kind : std::uint8_t { Materials, Decals };

    /// Lê a pasta (sem as subpastas). `report` diz quantos entraram e o que ficou de fora.
    static Palette load(const std::filesystem::path& folder, std::string& report, Kind kind = Kind::Materials);

    /// Acrescenta uma imagem no fim (os índices das que já estavam não mudam).
    void add(PaletteMaterial material);
    [[nodiscard]] int size() const { return static_cast<int>(materials_.size()); }
    [[nodiscard]] const PaletteMaterial& at(int index) const { return materials_[static_cast<std::size_t>(index)]; }
    /// Índice pelo id; -1 se não tem.
    [[nodiscard]] int find(std::string_view id) const;
    [[nodiscard]] Kind kind() const { return kind_; }
    [[nodiscard]] const std::filesystem::path& folder() const { return folder_; }

    /// Muda a escala e grava no .json ao lado da imagem (as outras chaves ficam).
    bool setMetersPerRepeat(int index, float meters, std::string& error);

    /// Nome que um arquivo importado ganha na pasta: minúsculas, sem acento, palavras
    /// separadas por hífen, extensão em minúsculas.
    [[nodiscard]] static std::string importName(const std::filesystem::path& source);
    /// Copia `source` para a pasta como `fileName` (substitui um arquivo que já tenha esse
    /// nome), cria o .json se não houver, e devolve o índice: o mesmo de antes se o id já
    /// existia (a imagem é trocada), ou um novo no fim. -1 se falhar (`error` diz por quê).
    int importFile(const std::filesystem::path& source, const std::string& fileName, std::string& error);

    /// Cor do material em `uv` (em repetições: 1 = uma imagem inteira), para quem pinta
    /// com `texelsPerMeter` texels por metro: mistura os dois mipmaps desse tamanho.
    [[nodiscard]] glm::vec4 sample(int index, const glm::vec2& uv, float texelsPerMeter) const;

private:
    std::vector<PaletteMaterial> materials_;
    std::filesystem::path folder_;
    Kind kind_ = Kind::Materials;
};

/// Eixos de uma face para os materiais e decalques: `u` na horizontal, para a direita de
/// quem olha a face, e `v` descendo pela face (numa parede, v = -y). Faces quase
/// horizontais: u = +x, v = +z. Com a normal, formam uma base ortonormal.
struct SurfaceAxes {
    glm::vec3 u{1.0f, 0.0f, 0.0f};
    glm::vec3 v{0.0f, 0.0f, 1.0f};
};
[[nodiscard]] SurfaceAxes surfaceAxes(const glm::vec3& faceNormal);

/// Posição na superfície, em metros, nos eixos de surfaceAxes: (dot(p, u), dot(p, v)).
/// Numa parede, as fiadas de tijolo ficam horizontais e partem do chão.
[[nodiscard]] glm::vec2 surfaceMeters(const glm::vec3& position, const glm::vec3& faceNormal);

} // namespace painter
