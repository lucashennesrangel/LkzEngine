# physics_jolt (a fazer)

Física rígida multicore com [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.5.0 ou mais recente. Entra quando o loop e o ECS estiverem estáveis.

Padrão de integração com o ECS, a cada passo fixo:

1. Ler `PositionComponent`/`RotationComponent` das entidades com corpo físico e enviar ao Jolt (só as cinemáticas ou teleportadas).
2. `PhysicsSystem::Update(fixedDt, ...)` do Jolt.
3. Escrever posição/rotação de volta. Guardar `previous` antes de escrever, para a interpolação visual continuar funcionando. Entidades ligadas a um `SceneNode` precisam ser escritas com `registry.patch()`, para avisar o observer da cena.

Estrutura sugerida: `lkz_physics_jolt` (alias `lkz::physics_jolt`) com um componente `RigidBodyComponent { JPH::BodyID id; }` e um sistema que substitui o `lkz::PhysicsSystem` para essas entidades.
