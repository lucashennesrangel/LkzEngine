// Testes da física (lkz::physics, Jolt): relevo, corpos, raio, carro e personagem.

#include <cmath>
#include <cstdio>

#include <lkz/Physics/PhysicsWorld.hpp>
#include <lkz/Terrain/Heightmap.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FALHOU: %s\n", what);
        ++failures;
    }
}

/// Relevo de 600 x 600 m (blocos não quadrados nas bordas), plano a 2 m com um morro.
lkz::Heightmap terrain() {
    lkz::Heightmap map = lkz::Heightmap::covering({600.0f, 400.0f}, 2.0f);
    for (int r = 0; r < map.rows(); ++r) {
        for (int c = 0; c < map.columns(); ++c) {
            const glm::vec2 p = map.origin() + glm::vec2{static_cast<float>(c), static_cast<float>(r)} * map.spacing();
            const float hill = 10.0f * std::exp(-glm::dot(p - glm::vec2{150.0f, 0.0f}, p - glm::vec2{150.0f, 0.0f}) / 2000.0f);
            map.set(c, r, 2.0f + hill);
        }
    }
    return map;
}

} // namespace

int main() {
    const lkz::Heightmap map = terrain();
    lkz::PhysicsWorld physics;
    const auto tiles = physics.addHeightfield(map, 128);
    // 301 x 201 amostras, blocos de 128 amostras (127 células, vizinhos dividem a borda): 3 x 2.
    check(tiles.size() == 6, "relevo dividido em blocos de 128 amostras");

    // Raio de cima para baixo acerta o relevo na altura certa.
    const auto hit = physics.raycast({150.0f, 100.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 200.0f);
    check(hit.has_value() && std::abs(hit->point.y - map.heightAt({150.0f, 0.0f})) < 0.2f, "raio acerta o topo do morro");

    // Caixa cai no plano e para em cima dele.
    const lkz::BodyId box = physics.addBody({lkz::BoxShape{glm::vec3{0.5f}}, glm::vec3{-100.0f, 10.0f, 0.0f}});
    // Carro no plano, acelerando para +Z.
    lkz::VehicleDesc car;
    car.position = {0.0f, 3.5f, -100.0f};
    const std::uint32_t vehicle = physics.addVehicle(car);
    // Personagem cai e anda para +X.
    const std::uint32_t person = physics.addCharacter({glm::vec3{-50.0f, 6.0f, 50.0f}});

    for (int i = 0; i < 60 * 6; ++i) {
        physics.setVehicleInput(vehicle, {i > 60 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f});
        physics.moveCharacter(person, i > 60 ? glm::vec3{2.0f, 0.0f, 0.0f} : glm::vec3{0.0f}, false);
        physics.step(1.0f / 60.0f);
    }

    const lkz::BodyState boxState = physics.bodyState(box);
    check(std::abs(boxState.position.y - 2.5f) < 0.05f, "caixa parada sobre o chão (centro a 0,5 m dele)");

    const lkz::VehicleState carState = physics.vehicleState(vehicle);
    check(carState.body.position.z > -80.0f, "carro andou para a frente");
    check(carState.speed > 3.0f, "carro com velocidade");
    check(std::abs(carState.body.position.y - 2.0f) < 1.5f, "carro sobre o chão");

    const lkz::CharacterState personState = physics.characterState(person);
    check(personState.onGround, "personagem no chão");
    check(std::abs(personState.position.y - 2.0f) < 0.1f, "pés do personagem na altura do chão");
    check(personState.position.x > -45.0f, "personagem andou");

    std::printf("caixa y=%.2f | carro z=%.1f v=%.1f m/s marcha %d | personagem (%.1f, %.2f)\n", boxState.position.y,
                carState.body.position.z, carState.speed, carState.gear, personState.position.x, personState.position.y);
    if (failures == 0) {
        std::printf("physics_test: tudo certo\n");
    }
    return failures == 0 ? 0 : 1;
}
