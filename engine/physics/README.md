# physics

Física da engine sobre o [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.5.0 (MIT). Alvo `lkz::physics`, header `<lkz/Physics/PhysicsWorld.hpp>`. Os headers do Jolt ficam no `.cpp`: quem usa só vê os tipos da engine.

```cpp
lkz::PhysicsWorld physics;                        // gravidade, limites e threads em PhysicsSettings
physics.addHeightfield(heightmap);                // relevo estático (o mesmo Heightmap do terreno)
physics.addStaticMesh(meshData);                  // malha estática (ruas, prédios)
const lkz::BodyId box = physics.addBody({lkz::BoxShape{{0.5f, 0.5f, 0.5f}}, {0.0f, 10.0f, 0.0f}});
const std::uint32_t car = physics.addVehicle({});  // carro de 4 rodas (VehicleDesc)
const std::uint32_t person = physics.addCharacter({});

world.onFixedUpdate([&](lkz::World& w, float dt) {
    physics.setVehicleInput(car, {1.0f, 0.0f, 0.0f, 0.0f}); // acelera
    physics.moveCharacter(person, {1.5f, 0.0f, 0.0f}, false);
    physics.step(dt);
    physics.syncToScene(w.registry());            // nós com PhysicsBodyComponent seguem os corpos
});
```

- **Passo:** `step(dt)` no passo fixo da lógica (60 Hz, o que o Jolt espera). Tudo na thread lógica; a interpolação para o desenho é a de sempre do World.
- **Corpos:** caixa, esfera e cápsula; `Static`, `Kinematic` ou `Dynamic`; massa, atrito e quique. `bodyState`, `setBodyTransform`, `setLinearVelocity`, `addImpulse`.
- **Relevo:** o heightfield do Jolt é quadrado; `addHeightfield` divide o Heightmap em blocos de `tileSamples` (256) amostras, um corpo por bloco, vizinhos dividindo a borda. A colisão usa sempre a resolução total (o LOD é só do desenho).
- **Veículos:** `VehicleDesc` (carroceria, rodas, suspensão, motor, tração dianteira ou integral). `setVehicleInput` (acelerar, virar, frear, freio de mão), `vehicleState` (posição, transformação de cada roda, velocidade, rotação do motor e marcha).
- **Personagens:** cápsula em pé (`CharacterDesc`) que anda, sobe degraus, escorrega em rampas íngremes e pula: `moveCharacter` + `characterState`.
- **Consultas:** `raycast` (ponto, normal, distância, corpo).
- **ECS:** `PhysicsBodyComponent{body}` numa entidade com nó da cena; `syncToScene` copia posição e rotação.
- **Opções de build:** o CMake do Jolt sobrescreve flags globais e liga LTO por padrão; aqui isso fica desligado (só a biblioteca é compilada). Usa AVX2 (padrão do Jolt).

Testes: `tests/physics_test.cpp` (caixa parando no chão, raio no relevo, carro andando, personagem). Demo: `samples/physics` (relevo de 2 × 2 km, 200 caixas, carro dirigível com WASD/setas).

Ainda não tem: ragdolls, juntas (constraints) além do veículo, gatilhos (sensores), camadas configuráveis e atualização parcial do heightfield (hoje: remover e adicionar os blocos).
