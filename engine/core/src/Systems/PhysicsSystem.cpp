#include "lkz/Systems/PhysicsSystem.hpp"

#include <tracy/Tracy.hpp>

#include "lkz/ECS/Components.hpp"

namespace lkz {

namespace {

auto movementGroup(entt::registry& registry) {
    return registry.group<PositionComponent, VelocityComponent>(entt::get<>, entt::exclude<SceneLinkComponent>);
}

} // namespace

PhysicsSystem::PhysicsSystem(entt::registry& registry)
    : registry_{registry} {
    // Cria o grupo já na construção: a partir daqui o EnTT mantém Position e Velocity
    // dessas entidades empacotadas lado a lado nas storages.
    static_cast<void>(movementGroup(registry_));
}

void PhysicsSystem::update(float dt) {
    ZoneScoped;

    movementGroup(registry_).each([dt](PositionComponent& position, const VelocityComponent& velocity) {
        position.previous = position.value;
        position.value += velocity.value * dt;
    });

    // Poucas entidades: as do Scene Graph que também têm velocidade.
    registry_.view<const VelocityComponent, const SceneLinkComponent>().each(
        [this, dt](const entt::entity entity, const VelocityComponent& velocity, const SceneLinkComponent&) {
            registry_.patch<PositionComponent>(entity, [&](PositionComponent& p) { p.value += velocity.value * dt; });
        });
}

} // namespace lkz
