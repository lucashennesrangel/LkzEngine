#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lkz {

/// Um valor que a interface mostra: sim/não, número ou texto.
using UiValue = std::variant<bool, int, double, std::string>;

/// Tela do jogo desenhada por cima da cena (World::showUi): um documento RML com estilo
/// RCSS, a "quase-HTML/CSS" da RmlUi (flexbox, sem grid; ver o README do backend).
///
/// O documento lê os valores pelo nome no modelo de dados "ui" (`<body data-model="ui">`):
/// `{{pontos}}`, `data-if="ajuda"`, `data-class-ativo="aba == 'mapa'"`,
/// `data-style-left="cursorX + 'px'"`. E manda ações com
/// `data-event-click="action('salvar')"` ou `action('aba', 'mapa')`, que chegam à
/// lógica em InputState::uiEvents.
///
/// O fundo do documento é transparente para o mouse: clique em área vazia vai para o
/// jogo. O que estiver sob um painel ou botão fica com a interface
/// (InputState::pointerOverUi), e esse clique não chega ao jogo.
struct UiDocument {
    std::string path;               // arquivo .rml (imagens e .rcss relativos a ele)
    std::vector<std::string> fonts; // .ttf/.otf, carregados antes do documento
    float designHeight = 720.0f;    // altura de referência: 1dp = altura da janela / designHeight
    /// Todos os nomes que o documento usa, com o valor inicial. Nome fora desta lista
    /// não existe para o documento.
    std::vector<std::pair<std::string, UiValue>> values;
};

/// Ação vinda da interface (ex.: clique em `action('aba', 'mapa')`).
struct UiEvent {
    std::string action;
    std::string argument; // vazio se a ação não tem argumento
};

} // namespace lkz
