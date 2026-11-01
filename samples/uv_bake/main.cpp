// uv_bake: UV único para um glTF, com as imagens dele assadas numa só por grupo de
// materiais (lkz::asset::bakeUnique). Sem janela.
//
// Uso: uv_bake <entrada.glb> <saida.glb> [px/m] [--png]
//   px/m   densidade de texels (padrão 32)
//   --png  grava também cada imagem nova ao lado da saída (<saida>_atlas<N>.png)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <lkz/Asset/Bake.hpp>
#include <lkz/Asset/Gltf.hpp>
#include <lkz/Asset/Image.hpp>
#include <lkz/Asset/Unwrap.hpp>
#include <lkz/Core/Log.hpp>

namespace {

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

void describe(const char* label, const lkz::asset::ModelData& model) {
    std::printf("%s: %zu triângulos, %zu materiais, %zu imagens, ~%.1f MiB de vídeo\n", label, model.triangleCount(),
                model.materials.size(), model.images.size(),
                static_cast<double>(model.estimatedTextureMemory()) / (1024.0 * 1024.0));
}

} // namespace

int main(int argc, char** argv) {
    lkz::initLogging();
    if (argc < 3) {
        std::printf("Uso: uv_bake <entrada.glb> <saida.glb> [px/m] [--png]\n");
        return EXIT_FAILURE;
    }
    const auto input = fromUtf8(argv[1]);
    const auto output = fromUtf8(argv[2]);
    lkz::asset::UnwrapOptions options;
    bool png = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--png") {
            png = true;
        } else {
            options.texelsPerMeter = static_cast<float>(std::atof(argv[i]));
        }
    }

    const auto model = lkz::asset::loadGltf(input);
    if (!model) {
        return EXIT_FAILURE;
    }
    describe("entrada", *model);
    for (std::size_t m = 0; m < model->materials.size(); ++m) {
        const auto overlap = lkz::asset::measureUvOverlap(*model, static_cast<int>(m));
        if (overlap.covered > 0) {
            std::printf("  material %-24s UV sobreposto em %5.1f%%\n", model->materials[m].name.c_str(),
                        overlap.ratio() * 100.0f);
        }
    }

    std::string report;
    const auto baked = lkz::asset::bakeUnique(*model, options, &report);
    if (!baked) {
        std::printf("falhou (detalhes no log)\n");
        return EXIT_FAILURE;
    }
    std::printf("UV único: %s\n", report.c_str());
    for (std::size_t m = 0; m < baked->materials.size(); ++m) {
        std::printf("  %s: UV sobreposto em %.1f%%\n", baked->materials[m].name.c_str(),
                    lkz::asset::measureUvOverlap(*baked, static_cast<int>(m)).ratio() * 100.0f);
    }
    if (!lkz::asset::saveGlb(output, *baked)) {
        return EXIT_FAILURE;
    }
    if (png) {
        for (std::size_t i = 0; i < baked->images.size(); ++i) {
            auto file = output;
            file.replace_extension();
            file += "_atlas" + std::to_string(i + 1) + ".png";
            lkz::asset::saveImage(file, baked->images[i]);
        }
    }
    const auto back = lkz::asset::loadGltf(output);
    if (!back) {
        return EXIT_FAILURE;
    }
    describe("saída", *back);
    std::printf("%s: %.1f KB -> %.1f KB\n", output.filename().string().c_str(),
                static_cast<double>(std::filesystem::file_size(input)) / 1024.0,
                static_cast<double>(std::filesystem::file_size(output)) / 1024.0);
    return EXIT_SUCCESS;
}
