#pragma once

#include <spdlog/spdlog.h>

namespace lkz {

/// Configura o logger padrão do spdlog (console colorido, com thread id).
/// Pode ser chamado mais de uma vez; só a primeira chamada cria o logger.
void initLogging(spdlog::level::level_enum level = spdlog::level::info);

} // namespace lkz
