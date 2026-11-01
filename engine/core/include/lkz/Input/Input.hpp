#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>

#include "lkz/UI/Ui.hpp"

namespace lkz {

/// Teclas pela posição no teclado americano (como o GLFW/raylib): num ABNT2, `Minus` e
/// `Equal` são o "-" e o "=" de sempre, mas os colchetes ficam em outras teclas.
enum class Key : std::uint8_t {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Up, Down, Left, Right,
    Space, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,
    Minus, Equal, LeftBracket, RightBracket, Comma, Period, Slash,
    KeypadAdd, KeypadSubtract,
    Count,
};

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
    Count,
};

/// Forma do ponteiro do mouse sobre a cena (World::setMouseCursor).
enum class MouseCursor : std::uint8_t {
    Default,
    Hand,       // algo que dá para pegar
    Move,       // arrastando, ou dá para arrastar em qualquer direção
    ResizeNwse, // diagonal: noroeste-sudeste
    ResizeNesw, // diagonal: nordeste-sudoeste
    Crosshair,
};

/// Teclado e mouse como a lógica enxerga num passo fixo.
///
/// O backend preenche na thread de render; o World entrega uma cópia por passo fixo
/// (World::input()). `keys`/`buttons` são o estado atual. Os eventos (`*Pressed`,
/// `*Released`), `mouseDelta` e `wheel` são acumulados desde o passo anterior: nenhum
/// clique ou movimento se perde quando a render roda mais rápido que a lógica.
struct InputState {
    static constexpr std::size_t keyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t buttonCount = static_cast<std::size_t>(MouseButton::Count);

    std::bitset<keyCount> keys;
    std::bitset<keyCount> keysPressed;
    std::bitset<keyCount> keysReleased;
    std::bitset<buttonCount> buttons;
    std::bitset<buttonCount> buttonsPressed;
    std::bitset<buttonCount> buttonsReleased;
    glm::vec2 mousePosition{0.0f}; // pixels, origem no canto superior esquerdo
    glm::vec2 mouseDelta{0.0f};    // pixels
    float wheel = 0.0f;            // positivo = rolar para frente
    glm::vec2 viewport{0.0f};      // tamanho da área de desenho, em pixels
    /// O mouse está sobre um painel ou botão da interface (World::showUi): os cliques e a
    /// roda que caem nela não aparecem nos campos acima.
    bool pointerOverUi = false;
    std::vector<UiEvent> uiEvents; // ações da interface desde o último passo, em ordem

    [[nodiscard]] bool down(Key key) const { return keys.test(index(key)); }
    [[nodiscard]] bool pressed(Key key) const { return keysPressed.test(index(key)); }
    [[nodiscard]] bool released(Key key) const { return keysReleased.test(index(key)); }

    [[nodiscard]] bool down(MouseButton button) const { return buttons.test(index(button)); }
    [[nodiscard]] bool pressed(MouseButton button) const { return buttonsPressed.test(index(button)); }
    [[nodiscard]] bool released(MouseButton button) const { return buttonsReleased.test(index(button)); }

    /// Zera o que é acumulado entre passos (eventos e deltas); mantém o estado atual.
    void clearEvents() {
        keysPressed.reset();
        keysReleased.reset();
        buttonsPressed.reset();
        buttonsReleased.reset();
        mouseDelta = glm::vec2{0.0f};
        wheel = 0.0f;
        uiEvents.clear();
    }

    static constexpr std::size_t index(Key key) { return static_cast<std::size_t>(key); }
    static constexpr std::size_t index(MouseButton button) { return static_cast<std::size_t>(button); }
};

} // namespace lkz
