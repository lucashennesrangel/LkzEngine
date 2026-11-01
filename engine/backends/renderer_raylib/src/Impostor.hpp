#pragma once

#include <vector>

#include <raylib.h>

#include "GpuTypes.hpp"

namespace lkz::raylib_detail {

/// Vistas de um objeto para desenhá-lo de longe como um quadrado virado para a câmera
/// (impostor octaédrico; ver Impostor.cpp).
struct GpuImpostor {
    Texture2D albedo{}; // cor; alfa 0 fora do objeto
    Texture2D normal{}; // normal no espaço do objeto, de 0 a 1
    int frames = 0;     // N x N vistas
    Bounds bounds;      // esfera fotografada (espaço do objeto)

    [[nodiscard]] bool valid() const { return albedo.id != 0 && normal.id != 0; }
};

/// Fotografa `parts` (já na GPU) de N x N direções do hemisfério de cima.
/// Chamar na thread de render, fora de BeginDrawing.
GpuImpostor bakeImpostor(const std::vector<GpuPart>& parts, const Bounds& bounds, int frames, int frameSize);

void unloadImpostor(GpuImpostor& impostor);

/// Quadrado de -1 a 1 no plano XY: a geometria de todo impostor.
Mesh makeImpostorQuad();

/// Shader instanciado dos impostores (mesmos atributos de instância do principal).
extern const char* const impostorVertexShader;
extern const char* const impostorFragmentShader;

} // namespace lkz::raylib_detail
