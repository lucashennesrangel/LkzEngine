#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/RenderInterface.h>
#include <glm/vec2.hpp>
#include <lkz/UI/Ui.hpp>

namespace lkz::ui_rmlui {

/// Números que o próprio backend põe no modelo "ui", para a interface mostrar:
/// lkzFps, lkzDrawCalls, lkzTriangles (milhões), lkzObjects e lkzLodColors.
struct UiStats {
    int fps = 0;
    std::size_t drawCalls = 0;
    std::size_t triangles = 0;
    std::size_t objects = 0;
    bool lodColors = false;
};

/// Botões do mouse num frame (esquerdo, direito, meio).
struct UiMouse {
    glm::vec2 position{0.0f};
    std::array<bool, 3> pressed{};
    std::array<bool, 3> released{};
    float wheel = 0.0f;
};

/// O desenho da interface numa API gráfica: a RenderInterface da RmlUi (geometria com
/// cores de alfa pré-multiplicado, texturas, recorte, transformações) mais o começo e o
/// fim do desenho de cada frame, por cima da cena.
class UiRenderer : public Rml::RenderInterface {
public:
    /// Antes do primeiro documento. false = sem interface (motivo no log).
    virtual bool initialize() = 0;
    /// Depois do Rml::Shutdown (texturas e geometria já devolvidas).
    virtual void release() = 0;
    virtual void beginFrame(int width, int height) = 0;
    virtual void endFrame() = 0;
};

/// A interface do World::showUi sobre a RmlUi, igual em qualquer backend: documento
/// RML/RCSS, modelo de dados "ui" com os valores do jogo, evento action(nome, argumento)
/// e o mouse (o fundo do documento não segura o mouse, só painéis e botões). Tudo na
/// thread de render (a RmlUi não é thread-safe). Ações com nome começando em "lkz." são
/// do próprio backend (ex.: "lkz.lodColors") e não chegam ao jogo.
class UiLayer {
public:
    explicit UiLayer(UiRenderer& renderer);
    ~UiLayer();

    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;

    /// Carrega fontes e documento (troca o anterior). false = nada carregado (veja o log).
    bool load(const UiDocument& document);
    [[nodiscard]] bool active() const;

    void setValues(std::span<const std::pair<std::string, UiValue>> values);
    void setStats(const UiStats& stats);

    /// Passa o mouse para a RmlUi. true = o mouse está sobre um painel ou botão.
    bool processMouse(const UiMouse& mouse);
    std::vector<UiEvent> takeEvents();

    void toggleDebugger();
    void render(int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lkz::ui_rmlui
