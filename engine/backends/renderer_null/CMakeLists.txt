add_library(lkz_renderer_null STATIC
    src/NullRenderBackend.cpp
)
add_library(lkz::renderer_null ALIAS lkz_renderer_null)

target_include_directories(lkz_renderer_null PUBLIC include)
target_link_libraries(lkz_renderer_null PUBLIC lkz::core)
lkz_target_defaults(lkz_renderer_null)
