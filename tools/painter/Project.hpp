#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace painter {

/// Arquivo de projeto de um jogo (ex.: <jogo>/painter.json): de onde abrir os modelos e
/// para onde exportar. A ferramenta não conhece jogo nenhum; tudo vem daqui. Caminhos
/// relativos ao próprio arquivo.
///
///     {
///       "nome": "MeuJogo",
///       "abrir": ["assets/models", "assets/models/_fonte"],
///       "saida": "assets/models",
///       "projetos": "assets/models/_projetos",
///       "paleta": "assets/textures/materials",
///       "decalques": "assets/textures/decals"
///     }
///
/// `saida`: a exportação vai para uma subpasta dela (escolhida na tela). `projetos`: antes
/// de substituir um modelo, o original é copiado para projetos/<nome>/ (uma vez só).
/// `paleta`: a pasta dos materiais repetíveis; `decalques`: a das imagens que o Carimbo põe
/// uma vez (janela, porta, letreiro). Ver Palette.hpp; as duas são opcionais.
struct Project {
    std::string name;
    std::filesystem::path file; // vazio = sem projeto (só o modelo da linha de comando)
    std::vector<std::filesystem::path> roots;
    std::filesystem::path output;
    std::filesystem::path work;
    std::filesystem::path palette; // vazio = sem paleta
    std::filesystem::path decals;  // vazio = sem decalques

    [[nodiscard]] static std::optional<Project> load(const std::filesystem::path& file, std::string& error);

    /// Subpastas de `output` que podem receber modelos (sem as de trabalho, que começam com _).
    [[nodiscard]] std::vector<std::string> outputFolders() const;
    /// A subpasta de `output` onde `model` já está, se estiver numa delas.
    [[nodiscard]] std::optional<std::string> folderOf(const std::filesystem::path& model) const;
};

} // namespace painter
