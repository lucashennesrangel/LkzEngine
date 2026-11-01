#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <lkz/Input/Input.hpp>
#include <lkz/Scene/Camera.hpp>

namespace painter {

/// Câmera que gira em volta de um ponto: botão direito gira, o do meio (ou Shift + direito)
/// arrasta, a roda aproxima (Ctrl + roda fica livre para quem usa a câmera).
class OrbitCamera {
public:
    explicit OrbitCamera(lkz::Camera& camera);

    /// Mira a caixa inteira, de um ângulo de três quartos.
    void frame(const glm::vec3& min, const glm::vec3& max);
    void update(const lkz::InputState& input);

    [[nodiscard]] glm::vec3 eye() const;
    /// Direção do raio do olho pelo pixel (origem no canto superior esquerdo).
    [[nodiscard]] glm::vec3 rayDirection(const glm::vec2& pixel, const glm::vec2& viewport) const;
    /// Pixels na tela por metro, a `distance` metros do olho.
    [[nodiscard]] float pixelsPerMeter(float distance, const glm::vec2& viewport) const;

private:
    void apply();
    [[nodiscard]] glm::quat rotation() const;

    lkz::Camera& camera_;
    glm::vec3 target_{0.0f};
    float yaw_ = 0.7f;
    float pitch_ = 0.4f;
    float distance_ = 10.0f;
    float radius_ = 1.0f; // do que está enquadrado: limita o zoom e os planos de corte
};

} // namespace painter
