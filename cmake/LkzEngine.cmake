# Funções públicas da engine. Funções do CMake são globais: depois que a engine é
# incluída (FetchContent_MakeAvailable/add_subdirectory), o jogo pode chamá-las.

# Flags comuns aos alvos da engine e dos jogos (não se aplicam às dependências).
function(lkz_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Cria um executável que linka o núcleo e os backends escolhidos.
# Usado pelos samples da engine e pelos jogos (que ficam em repositórios próprios).
#
#   lkz_add_game(<nome>
#       SOURCES   main.cpp ...
#       LIBRARIES lkz::renderer_null ...)
function(lkz_add_game name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES")

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "lkz_add_game(${name}): informe SOURCES")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE lkz::core ${ARG_LIBRARIES})
    lkz_target_defaults(${name})
    set_target_properties(${name} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
