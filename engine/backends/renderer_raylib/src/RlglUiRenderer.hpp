#pragma once

#include <memory>

#include <lkz/ui_rmlui/UiLayer.hpp>

namespace lkz::raylib_detail {

/// O desenho da interface (RmlUi) sobre o rlgl, o OpenGL 3.3 da raylib: geometria em
/// VAOs, texturas da raylib, recorte por scissor e transformações por matriz.
class RlglUiRenderer {
public:
    RlglUiRenderer();
    ~RlglUiRenderer();

    RlglUiRenderer(const RlglUiRenderer&) = delete;
    RlglUiRenderer& operator=(const RlglUiRenderer&) = delete;

    [[nodiscard]] ui_rmlui::UiRenderer& get();

private:
    class Renderer; // guarda os tipos da raylib fora do header
    std::unique_ptr<Renderer> renderer_;
};

} // namespace lkz::raylib_detail
