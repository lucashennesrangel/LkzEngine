#pragma once

#include <entt/entity/registry.hpp>

namespace lkz {

/// Integração de movimento em lote (Euler semi-implícito, sem colisão).
///
/// O hot path itera o grupo owning <Position, Velocity> (memória contígua, sem
/// alocação, sem sinais). Entidades ligadas ao Scene Graph ficam fora do grupo e
/// são movidas via patch(), para avisar o observer da cena.
/// Será substituído/complementado pelo backend Jolt.
class PhysicsSystem {
public:
    explicit PhysicsSystem(entt::registry& registry);

    void update(float dt);

private:
    entt::registry& registry_;
};

} // namespace lkz
