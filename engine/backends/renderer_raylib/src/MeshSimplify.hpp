#pragma once

#include <raylib.h>

namespace lkz::raylib_detail {

/// Versão simplificada de `source` com cerca de `ratio` dos triângulos (meshoptimizer),
/// já enviada à placa de vídeo. Primeiro tenta preservar a forma (bordas, costuras de
/// textura); se não chegar perto da meta (folhagem solta, por exemplo), usa a
/// simplificação "sloppy", que junta pedaços próximos. Malha vazia se não der.
Mesh simplifyMesh(const Mesh& source, float ratio);

} // namespace lkz::raylib_detail
