// Testes do Pintor de modelos (tools/painter/Document) sem janela: abrir um quadrado de
// 2 x 2 m só com cor, pintar no meio, desfazer, refazer, apagar e exportar.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <lkz/Asset/Gltf.hpp>
#include <lkz/Asset/Image.hpp>
#include <lkz/Asset/Raycast.hpp>
#include <lkz/Asset/TexelMap.hpp>

#include "Document.hpp"
#include "Palette.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FALHOU: %s\n", what);
        ++failures;
    }
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance = 0.01f) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}

/// Quadrado de 2 x 2 m no chão (y = 0), virado para cima, UV de 0 a 1, cor cinza.
lkz::asset::ModelData quad() {
    lkz::asset::ModelData model;
    lkz::asset::ModelPart part;
    part.name = "chao";
    part.mesh.positions = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}};
    part.mesh.normals.assign(4, glm::vec3{0.0f, 1.0f, 0.0f});
    part.mesh.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    part.mesh.indices = {0, 2, 1, 0, 3, 2};
    part.material = 0;
    model.parts.push_back(part);
    lkz::asset::MaterialData material;
    material.name = "cinza";
    material.baseColor = glm::vec4{0.5f, 0.5f, 0.5f, 1.0f};
    model.materials.push_back(material);
    return model;
}

/// Cor no ponto do quadrado sob (x, z), vista de cima.
glm::vec3 colorAt(const painter::Document& document, float x, float z) {
    const auto hit = document.raycaster().intersect({x, 5.0f, z}, {0.0f, -1.0f, 0.0f});
    if (!hit) {
        return glm::vec3{-1.0f};
    }
    return document.colorAt(*hit).value_or(glm::vec3{-1.0f});
}

/// Imagem 2 x 2: vermelho, verde / azul, branco.
lkz::asset::ImageData quadrants() {
    lkz::asset::ImageData image;
    image.width = 2;
    image.height = 2;
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    return image;
}

/// Três quadrados de 2 x 2 m do mesmo material cinza: o chão A (no meio), o chão B
/// (separado, em x = 4) e uma parede em pé encostada em A (em z = -1, virada para +Z).
lkz::asset::ModelData scene() {
    lkz::asset::ModelData model;
    const auto add = [&](const char* name, std::vector<glm::vec3> positions, glm::vec3 normal, std::vector<std::uint32_t> indices) {
        lkz::asset::ModelPart part;
        part.name = name;
        part.mesh.positions = std::move(positions);
        part.mesh.normals.assign(4, normal);
        part.mesh.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        part.mesh.indices = std::move(indices);
        part.material = 0;
        model.parts.push_back(std::move(part));
    };
    add("chao_a", {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}}, {0, 1, 0}, {0, 2, 1, 0, 3, 2});
    add("chao_b", {{3, 0, -1}, {5, 0, -1}, {5, 0, 1}, {3, 0, 1}}, {0, 1, 0}, {0, 2, 1, 0, 3, 2});
    add("parede", {{-1, 0, -1}, {1, 0, -1}, {1, 2, -1}, {-1, 2, -1}}, {0, 0, 1}, {0, 1, 2, 0, 2, 3});
    lkz::asset::MaterialData material;
    material.name = "cinza";
    material.baseColor = glm::vec4{0.5f, 0.5f, 0.5f, 1.0f};
    model.materials.push_back(material);
    return model;
}

std::optional<lkz::asset::RayHit> ray(const painter::Document& document, glm::vec3 origin, glm::vec3 direction) {
    return document.raycaster().intersect(origin, direction);
}

glm::vec3 colorOf(const painter::Document& document, const std::optional<lkz::asset::RayHit>& hit) {
    return hit ? document.colorAt(*hit).value_or(glm::vec3{-1.0f}) : glm::vec3{-1.0f};
}

lkz::asset::SurfaceTarget targetOf(const painter::Document& document, const std::optional<lkz::asset::RayHit>& hit) {
    const int face = hit ? document.faceIndex(hit->where) : -1;
    return face >= 0 ? document.face(face).target : lkz::asset::SurfaceTarget::Unique;
}

/// Fase 5: paleta, seleção do balde, destinos, exportação por referência e de volta.
void materials(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;
    check(painter::surfaceMeters({1.0f, 2.0f, -1.0f}, {0.0f, 0.0f, 1.0f}) == glm::vec2(1.0f, -2.0f),
          "surfaceMeters: parede para +Z, u = x e v = -y");
    check(painter::surfaceMeters({0.0f, 2.0f, 3.0f}, {1.0f, 0.0f, 0.0f}) == glm::vec2(-3.0f, -2.0f),
          "surfaceMeters: parede para +X, u = -z");
    check(painter::surfaceMeters({1.0f, 0.0f, 3.0f}, {0.0f, 1.0f, 0.0f}) == glm::vec2(1.0f, 3.0f),
          "surfaceMeters: chão, u = x e v = z");

    // A paleta: uma imagem num arquivo (a exportação aponta para ele), 1 m por repetição.
    fs::create_directories(folder / "textures");
    const fs::path imageFile = fs::absolute(folder / "textures" / "xadrez.png").lexically_normal();
    check(lkz::asset::saveImage(imageFile, quadrants()), "gravar a imagem da paleta");
    painter::Palette palette;
    painter::PaletteMaterial xadrez;
    xadrez.id = "xadrez";
    xadrez.name = "Xadrez";
    xadrez.image = imageFile;
    xadrez.mips.push_back(quadrants());
    palette.add(xadrez);
    check(palette.size() == 1 && palette.at(0).mips.size() == 2 && palette.find("xadrez") == 0, "paleta com mipmaps");

    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "gravar a cena");
    painter::Document document;
    document.setPalette(&palette);
    std::string error;
    check(document.open(source, error), "abrir a cena");
    const auto hitA = ray(document, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto hitB = ray(document, {4.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto hitWall = ray(document, {0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(hitA && hitB && hitWall, "raios na cena");
    if (!hitA || !hitB || !hitWall) {
        return;
    }
    check(document.select(*hitA, painter::Selection::Triangle).size() == 1, "seleção: triângulo");
    check(document.select(*hitA, painter::Selection::Wall).size() == 2, "seleção: parede = só o chão A (ligado)");
    check(document.select(*hitA, painter::Selection::Plane).size() == 4, "seleção: plano = os dois chãos");
    check(document.select(*hitA, painter::Selection::Material).size() == 6, "seleção: material = tudo");
    check(std::abs(document.area(document.select(*hitA, painter::Selection::Plane)) - 8.0f) < 1e-4f, "área do plano: 8 m²");
    document.takeGeometryChanged();

    // Balde: o chão A vira o xadrez compartilhado, em metros (1 m por repetição).
    painter::Fill shared;
    shared.target = SurfaceTarget::Shared;
    shared.material = 0;
    check(document.fill(document.select(*hitA, painter::Selection::Wall), shared) == 2, "balde em 2 faces");
    check(document.takeGeometryChanged(), "o balde muda as superfícies");
    check(targetOf(document, hitA) == SurfaceTarget::Shared && document.paletteAt(*hitA) == 0, "chão A compartilhado");
    check(near(colorAt(document, 0.25f, 0.25f), {1.0f, 0.0f, 0.0f}) && near(colorAt(document, 0.75f, 0.25f), {0.0f, 1.0f, 0.0f}) &&
              near(colorAt(document, 0.25f, 0.75f), {0.0f, 0.0f, 1.0f}),
          "xadrez no chão A na escala certa");
    int sharedSurfaces = 0;
    for (const auto& surface : document.surfaces()) {
        if (surface.kind == painter::Document::Surface::Kind::Shared) {
            ++sharedSurfaces;
            check(surface.mesh.positions.size() == 4 && surface.mesh.uvs[0] == glm::vec2(-1.0f, -1.0f),
                  "superfície compartilhada: 4 vértices, UV em metros");
        }
    }
    check(sharedSurfaces == 1, "uma superfície compartilhada");
    check(document.undo() && targetOf(document, hitA) == SurfaceTarget::Unique, "desfazer o balde");
    check(document.redo() && targetOf(document, hitA) == SurfaceTarget::Shared, "refazer o balde");

    // Chão B em cor lisa; parede com o xadrez assado na tela (Único).
    painter::Fill flat;
    flat.target = SurfaceTarget::Color;
    flat.color = {0.2f, 0.4f, 0.6f};
    document.fill(document.select(*hitB, painter::Selection::Wall), flat);
    check(targetOf(document, hitB) == SurfaceTarget::Color && near(colorOf(document, hitB), {0.2f, 0.4f, 0.6f}, 0.01f),
          "chão B em cor lisa");
    painter::Fill unique;
    unique.target = SurfaceTarget::Unique;
    unique.material = 0;
    document.fill(document.select(*hitWall, painter::Selection::Wall), unique);
    // Parede: u = x, v = -y. Em (0,25; 1,25) a imagem dá o azul (linha de baixo, coluna da esquerda).
    check(targetOf(document, hitWall) == SurfaceTarget::Unique && near(colorOf(document, hitWall), {0.0f, 0.0f, 1.0f}, 0.1f),
          "parede: xadrez assado na tela");

    // Exportar: chão A aponta para a imagem da paleta; chão B nas amostras; parede na tela.
    const fs::path exported = folder / "modelos" / "cena.glb";
    std::string report;
    check(document.exportGlb(exported, report), "exportar a cena");
    std::printf("%s\n", report.c_str());
    const auto back = lkz::asset::loadGltf(exported);
    check(back.has_value() && back->materials.size() == 3, "três materiais: tela, paleta e cores");
    if (back) {
        int found = 0;
        for (const lkz::asset::MaterialData& material : back->materials) {
            const lkz::asset::ImageData* image =
                material.image >= 0 ? &back->images[static_cast<std::size_t>(material.image)] : nullptr;
            if (material.target == SurfaceTarget::Shared) {
                found += material.shared == "xadrez" && image != nullptr && image->uri == "../textures/xadrez.png" &&
                         image->file == imageFile;
            } else if (material.target == SurfaceTarget::Color) {
                found += image != nullptr && image->width == 4;
            } else {
                found += image != nullptr && image->uri.empty() && image->width == 64;
            }
        }
        check(found == 3, "paleta por referência relativa, amostras 4 x 4 e a tela embutida");
    }

    // Reabrir com a paleta: os destinos voltam.
    painter::Document again;
    again.setPalette(&palette);
    check(again.open(exported, error), "reabrir o exportado");
    const auto againA = ray(again, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto againB = ray(again, {4.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto againWall = ray(again, {0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(targetOf(again, againA) == SurfaceTarget::Shared && near(colorOf(again, againA), {1.0f, 0.0f, 0.0f}),
          "reaberto: chão A da paleta");
    check(targetOf(again, againB) == SurfaceTarget::Color && near(colorOf(again, againB), {0.2f, 0.4f, 0.6f}, 0.01f),
          "reaberto: chão B em cor lisa exata");
    check(targetOf(again, againWall) == SurfaceTarget::Unique && near(colorOf(again, againWall), {0.0f, 0.0f, 1.0f}, 0.15f),
          "reaberto: parede pintada");

    // UV único: só a parede vai para o atlas; chão A e chão B ficam como estão.
    lkz::asset::UnwrapOptions options;
    options.texelsPerMeter = 32.0f;
    std::string uvReport;
    check(again.makeUniqueUv(options, uvReport), "UV único com destinos");
    std::printf("%s\n", uvReport.c_str());
    const auto bakedA = ray(again, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto bakedB = ray(again, {4.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto bakedWall = ray(again, {0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(targetOf(again, bakedA) == SurfaceTarget::Shared && near(colorOf(again, bakedA), {1.0f, 0.0f, 0.0f}),
          "UV único: chão A continua da paleta");
    check(targetOf(again, bakedB) == SurfaceTarget::Color && near(colorOf(again, bakedB), {0.2f, 0.4f, 0.6f}, 0.01f),
          "UV único: chão B continua em cor lisa");
    check(targetOf(again, bakedWall) == SurfaceTarget::Unique && near(colorOf(again, bakedWall), {0.0f, 0.0f, 1.0f}, 0.2f),
          "UV único: a parede assada no atlas");

    // Original: o chão A volta como abriu (único, cinza).
    painter::Fill restore;
    restore.restore = true;
    document.fill(document.select(*hitA, painter::Selection::Wall), restore);
    check(targetOf(document, hitA) == SurfaceTarget::Unique && near(colorAt(document, 0.25f, 0.25f), {0.5f, 0.5f, 0.5f}),
          "balde Original: como ao abrir");

    // Escala: gravada no .json ao lado da imagem e lida de volta.
    std::string scaleError;
    check(palette.setMetersPerRepeat(0, 2.0f, scaleError), "mudar a escala");
    document.paletteChanged();
    check(document.takeGeometryChanged(), "escala nova refaz as superfícies");
    std::string paletteReport;
    const painter::Palette loaded = painter::Palette::load(folder / "textures", paletteReport);
    check(loaded.size() == 1 && loaded.at(0).name == "Xadrez" && std::abs(loaded.at(0).metersPerRepeat - 2.0f) < 1e-4f,
          "paleta lida da pasta, com o nome e a escala do .json");
}

void texelMap() {
    // Um triângulo que cobre metade de uma imagem 8 x 8: 36 texels de centro dentro
    // (8 + 7 + ... + 1 = 36, contando a diagonal) e mais alguns com margem.
    const glm::vec2 uv[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    int inside = 0;
    lkz::asset::forEachTexel(uv, 8, 8, 0.0f, std::nullopt, [&](int, int, const glm::vec3&) { ++inside; });
    check(inside == 36, "forEachTexel: 36 texels no meio quadrado 8 x 8");
    int padded = 0;
    lkz::asset::forEachTexel(uv, 8, 8, 1.0f, std::nullopt, [&](int, int, const glm::vec3&) { ++padded; });
    check(padded > inside, "forEachTexel: a margem pega texels da borda");
    int clipped = 0;
    lkz::asset::forEachTexel(uv, 8, 8, 0.0f, lkz::asset::TexelRect{0, 0, 2, 2},
                             [&](int, int, const glm::vec3&) { ++clipped; });
    check(clipped == 4, "forEachTexel: recorte 2 x 2");

    const glm::vec3 p[3] = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 4.0f}};
    const auto toUv = lkz::asset::SurfaceToUv::from(p, uv);
    check(toUv.has_value(), "SurfaceToUv: triângulo com área");
    if (toUv) {
        const glm::vec2 middle = toUv->at({1.0f, 0.0f, 1.0f});
        check(std::abs(middle.x - 0.5f) < 1e-5f && std::abs(middle.y - 0.25f) < 1e-5f, "SurfaceToUv: ponto no meio");
    }
}

/// Fase 5b: importar imagens, carimbar decalques (opaco, girado, com transparência),
/// desfazer, exportar por referência e voltar.
void decals(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;

    const painter::SurfaceAxes wall = painter::surfaceAxes({0.0f, 0.0f, 1.0f});
    check(wall.u == glm::vec3(1.0f, 0.0f, 0.0f) && wall.v == glm::vec3(0.0f, -1.0f, 0.0f), "eixos de uma parede para +Z");
    const painter::SurfaceAxes floor = painter::surfaceAxes({0.0f, 1.0f, 0.0f});
    check(floor.u == glm::vec3(1.0f, 0.0f, 0.0f) && floor.v == glm::vec3(0.0f, 0.0f, 1.0f), "eixos do chão");
    check(painter::Palette::importName(fs::path{"Foto da Rua.JPG"}) == "foto-da-rua.jpg", "nome importado: minúsculas e hífen");
    check(painter::Palette::importName(fs::path{u8"Placa Ação!.png"}) == "placa-acao.png", "nome importado: sem acento");

    // Duas imagens fora do projeto: uma janela opaca e uma placa com um canto transparente.
    fs::create_directories(folder / "fontes");
    lkz::asset::ImageData placa = quadrants();
    placa.rgba[3] = 0; // o pixel vermelho (em cima, à esquerda) é transparente
    check(lkz::asset::saveImage(folder / "fontes" / "Janela Azul.png", quadrants()) &&
              lkz::asset::saveImage(folder / "fontes" / fs::path{u8"Placa Ação.png"}, placa),
          "gravar as imagens a importar");
    std::string report;
    painter::Palette library = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    std::string error;
    const int janela = library.importFile(folder / "fontes" / "Janela Azul.png", "janela-azul.png", error);
    const int placaIndex = library.importFile(folder / "fontes" / fs::path{u8"Placa Ação.png"}, "placa-acao.png", error);
    check(janela == 0 && placaIndex == 1 && fs::exists(folder / "decals" / "janela-azul.png") &&
              fs::exists(folder / "decals" / "janela-azul.json"),
          "importar copia a imagem e cria o .json");
    check(!library.at(janela).transparent && library.at(placaIndex).transparent, "transparência detectada");
    check(library.importFile(folder / "fontes" / "Janela Azul.png", "janela-azul.png", error) == janela,
          "importar de novo com o mesmo nome troca a imagem, no mesmo índice");
    const painter::Palette reloaded = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    check(reloaded.size() == 2 && reloaded.at(reloaded.find("janela-azul")).name == "Janela Azul" &&
              std::abs(reloaded.at(0).widthMeters - 1.0f) < 1e-4f,
          "biblioteca relida com o nome original e a largura padrão");

    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "gravar a cena dos decalques");
    painter::Document document;
    document.setDecals(&library);
    check(document.open(source, error), "abrir a cena dos decalques");
    const std::size_t triangles = document.model().triangleCount();
    const auto wallHit = ray(document, {0.0f, 1.0f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(wallHit.has_value(), "raio na parede");
    if (!wallHit) {
        return;
    }
    const auto atWall = [&](float x, float y) { return colorOf(document, ray(document, {x, y, 5.0f}, {0.0f, 0.0f, -1.0f})); };
    const auto atFloor = [&](const painter::Document& d, float x, float z) {
        return colorOf(d, ray(d, {x, 5.0f, z}, {0.0f, -1.0f, 0.0f}));
    };

    // Janela de 1 x 1 m na parede, de x = -0,5 a 0,5 e de y = 0,5 a 1,5 (u = x, v = -y).
    painter::DecalPlacement placement;
    placement.decal = janela;
    placement.face = document.faceIndex(wallHit->where);
    placement.origin = {-0.5f, -1.5f};
    placement.size = {1.0f, 1.0f};
    const int created = document.stamp(placement);
    check(created > 0 && document.model().triangleCount() > triangles, "carimbo corta a parede");
    check(near(atWall(-0.25f, 1.25f), {1.0f, 0.0f, 0.0f}) && near(atWall(0.25f, 1.25f), {0.0f, 1.0f, 0.0f}) &&
              near(atWall(-0.25f, 0.75f), {0.0f, 0.0f, 1.0f}),
          "janela na posição e na escala certas, sem distorcer");
    check(near(atWall(0.8f, 1.8f), {0.5f, 0.5f, 0.5f}) && near(atWall(-0.8f, 0.2f), {0.5f, 0.5f, 0.5f}),
          "a parede fora do retângulo continua como era");
    const auto inside = ray(document, {-0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(targetOf(document, inside) == SurfaceTarget::Decal && document.decalAt(*inside) == janela,
          "faces de decalque e conta-gotas");
    check(document.undo() && document.model().triangleCount() == triangles && near(atWall(-0.25f, 1.25f), {0.5f, 0.5f, 0.5f}),
          "desfazer o carimbo devolve a parede");
    check(document.redo() && near(atWall(-0.25f, 1.25f), {1.0f, 0.0f, 0.0f}), "refazer o carimbo");

    // Segunda janela igual, girada um quarto de volta, no chão B (u = x, v = z).
    const auto floorB = ray(document, {4.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    placement.face = document.faceIndex(floorB->where);
    placement.origin = {3.5f, -0.5f};
    placement.quarterTurns = 1; // o alto da imagem vai para a direita (+x)
    check(document.stamp(placement) > 0, "carimbo girado");
    check(near(atFloor(document, 4.25f, -0.25f), {1.0f, 0.0f, 0.0f}) && near(atFloor(document, 3.75f, -0.25f), {0.0f, 0.0f, 1.0f}),
          "giro de 90°: vermelho à direita em cima, azul à esquerda em cima");
    check(document.decalCount() == 1, "as duas janelas dividem a mesma imagem");

    // Placa com transparência sobre o chão A em cor lisa: o canto transparente mostra a cor.
    painter::Fill flat;
    flat.target = SurfaceTarget::Color;
    flat.color = {0.2f, 0.4f, 0.6f};
    const auto floorA = ray(document, {0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    document.fill(document.select(*floorA, painter::Selection::Wall), flat);
    placement.decal = placaIndex;
    placement.face = document.faceIndex(floorA->where);
    placement.origin = {-0.5f, -0.5f};
    placement.quarterTurns = 0;
    check(document.stamp(placement) > 0, "carimbo com transparência");
    check(near(atFloor(document, -0.25f, -0.25f), {0.2f, 0.4f, 0.6f}, 0.01f) &&
              near(atFloor(document, 0.25f, -0.25f), {0.0f, 1.0f, 0.0f}),
          "transparência composta sobre a cor da face");
    check(document.decalCount() == 2, "a placa composta tem a sua imagem");

    // Exportar: a janela aponta para a biblioteca; a placa composta vai embutida.
    const fs::path exported = folder / "modelos" / "cena.glb";
    check(document.exportGlb(exported, report), "exportar com decalques");
    std::printf("%s\n", report.c_str());
    if (const auto back = lkz::asset::loadGltf(exported)) {
        int external = 0;
        int embedded = 0;
        for (const lkz::asset::MaterialData& material : back->materials) {
            if (material.target != SurfaceTarget::Decal || material.image < 0) {
                continue;
            }
            const lkz::asset::ImageData& image = back->images[static_cast<std::size_t>(material.image)];
            external += material.shared == "janela-azul" && image.uri == "../decals/janela-azul.png";
            embedded += material.shared == "placa-acao" && image.uri.empty();
        }
        check(external == 1 && embedded == 1, "janela por referência, placa composta embutida");
    }
    painter::Document again;
    again.setDecals(&library);
    check(again.open(exported, error), "reabrir com decalques");
    const auto againWall = ray(again, {-0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(targetOf(again, againWall) == SurfaceTarget::Decal && near(colorOf(again, againWall), {1.0f, 0.0f, 0.0f}),
          "reaberto: a janela");
    check(near(atFloor(again, -0.25f, -0.25f), {0.2f, 0.4f, 0.6f}, 0.02f), "reaberto: a placa composta");

    // UV único: os decalques ficam como estão.
    lkz::asset::UnwrapOptions options;
    std::string uvReport;
    check(again.makeUniqueUv(options, uvReport), "UV único com decalques");
    const auto bakedWall = ray(again, {-0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(targetOf(again, bakedWall) == SurfaceTarget::Decal && near(colorOf(again, bakedWall), {1.0f, 0.0f, 0.0f}) &&
              near(atFloor(again, 4.25f, -0.25f), {1.0f, 0.0f, 0.0f}),
          "UV único: os decalques continuam");
}

/// Mão: pegar um carimbo posto, desistir, mover (um passo de desfazer), apagar, balde
/// depois do carimbo e carimbo sobre carimbo.
void hand(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;
    fs::create_directories(folder / "decals");
    check(lkz::asset::saveImage(folder / "decals" / "janela.png", quadrants()), "mão: gravar a janela");
    std::string report;
    std::string error;
    const painter::Palette library = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "mão: gravar a cena");
    painter::Document document;
    document.setDecals(&library);
    check(document.open(source, error) && library.size() == 1, "mão: abrir a cena");
    const auto wallRay = [&](float x, float y) { return ray(document, {x, y, 5.0f}, {0.0f, 0.0f, -1.0f}); };
    const auto atWall = [&](float x, float y) { return colorOf(document, wallRay(x, y)); };
    const auto faceAt = [&](float x, float y) {
        const auto hit = wallRay(x, y);
        return hit ? document.faceIndex(hit->where) : -1;
    };
    const auto decalFaces = [&]() {
        int count = 0;
        for (int f = 0; f < document.faceCount(); ++f) {
            count += document.face(f).target == SurfaceTarget::Decal ? 1 : 0;
        }
        return count;
    };
    const glm::vec3 gray{0.5f};
    const glm::vec3 red{1.0f, 0.0f, 0.0f};
    const glm::vec3 white{1.0f};

    // Janela A: x de -0,5 a 0,5, y de 0,5 a 1,5.
    painter::DecalPlacement placement;
    placement.decal = 0;
    placement.face = faceAt(0.0f, 1.0f);
    placement.origin = {-0.5f, -1.5f};
    placement.size = {1.0f, 1.0f};
    const int created = document.stamp(placement);
    const std::size_t stamped = document.model().triangleCount();
    const int stampFace = faceAt(-0.25f, 1.25f);
    const auto found = document.stampAt(stampFace);
    check(created > 0 && found && found->origin == placement.origin && found->size == placement.size &&
              static_cast<int>(document.stampFaces(stampFace).size()) == created,
          "mão: o carimbo posto é achado pela face");
    check(!document.stampAt(faceAt(0.8f, 1.8f)) && document.stampFaces(faceAt(0.8f, 1.8f)).empty(),
          "mão: a parede não é carimbo");

    // Pegar e desistir: volta exatamente como estava.
    auto lifted = document.lift(stampFace);
    check(lifted && lifted->face >= 0 && document.faceNormal(lifted->face).z > 0.99f && document.lifted() &&
              near(atWall(-0.25f, 1.25f), gray) && decalFaces() == 0,
          "mão: pegar tira o carimbo, e a parede volta embaixo");
    document.cancelLift();
    check(!document.lifted() && document.model().triangleCount() == stamped && near(atWall(-0.25f, 1.25f), red),
          "mão: desistir devolve o carimbo");

    // Mover para x de -0,9 a 0,1, y de 0,9 a 1,9: um passo de desfazer.
    lifted = document.lift(faceAt(-0.25f, 1.25f));
    painter::DecalPlacement moved = *lifted;
    moved.origin = {-0.9f, -1.9f};
    check(document.stamp(moved) > 0 && !document.lifted(), "mão: mover");
    check(near(atWall(-0.65f, 1.65f), red) && near(atWall(0.3f, 0.7f), gray) && near(atWall(-0.15f, 1.15f), white),
          "mão: o carimbo no lugar novo, o antigo virou parede");
    check(document.undo() && near(atWall(-0.25f, 1.25f), red) && near(atWall(-0.65f, 1.65f), gray) &&
              document.model().triangleCount() == stamped,
          "mão: desfazer o movimento (um passo só)");
    check(document.redo() && near(atWall(-0.65f, 1.65f), red), "mão: refazer o movimento");

    // Apagar: pegar sem carimbar de novo.
    check(document.lift(faceAt(-0.65f, 1.65f)).has_value(), "mão: pegar para apagar");
    document.endLift();
    check(near(atWall(-0.65f, 1.65f), gray) && decalFaces() == 0, "mão: apagar");
    check(document.undo() && near(atWall(-0.65f, 1.65f), red), "mão: desfazer o apagar");

    // Balde na parede depois do carimbo: o buraco de quem sai mostra a parede nova.
    std::vector<int> wall;
    for (int f = 0; f < document.faceCount(); ++f) {
        if (document.faceNormal(f).z > 0.99f && document.face(f).target != SurfaceTarget::Decal) {
            wall.push_back(f);
        }
    }
    painter::Fill paint;
    paint.target = SurfaceTarget::Color;
    paint.color = {0.2f, 0.4f, 0.6f};
    document.fill(wall, paint);
    const glm::vec3 blue{0.2f, 0.4f, 0.6f};
    check(document.lift(faceAt(-0.65f, 1.65f)).has_value() && near(atWall(-0.65f, 1.65f), blue) &&
              near(atWall(-0.1f, 1.0f), blue),
          "mão: o buraco pega o destino atual da parede");
    document.cancelLift();

    // Carimbo B sobre A (x de -0,4 a 0,6, y de 0,4 a 1,4): tirar B mostra A inteiro de novo.
    placement.face = faceAt(0.5f, 0.5f);
    placement.origin = {-0.4f, -1.4f};
    check(document.stamp(placement) > 0 && near(atWall(-0.15f, 1.15f), red), "mão: B por cima de A");
    check(document.lift(faceAt(0.5f, 0.5f)).has_value(), "mão: pegar B");
    document.endLift();
    check(near(atWall(-0.15f, 1.15f), white) && near(atWall(0.5f, 0.5f), blue), "mão: sem B, A aparece inteiro");
    check(document.lift(faceAt(-0.15f, 1.15f)).has_value(), "mão: pegar A");
    document.endLift();
    check(near(atWall(-0.15f, 1.15f), blue) && near(atWall(-0.65f, 1.65f), blue) && decalFaces() == 0,
          "mão: sem A nem B, só a parede");
    check(document.undo() && document.undo() && near(atWall(-0.15f, 1.15f), red), "mão: desfazer volta A e B");

    std::string exported;
    check(document.exportGlb(folder / "modelos" / "mao.glb", exported), "mão: exportar depois de mexer");
}

/// Material da paleta tingido: a imagem multiplicada pela cor, no Pintor, na tela (Único),
/// na exportação (baseColorFactor) e ao reabrir.
void tint(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;
    fs::create_directories(folder / "textures");
    const fs::path imageFile = fs::absolute(folder / "textures" / "xadrez.png").lexically_normal();
    check(lkz::asset::saveImage(imageFile, quadrants()), "tinta: gravar a imagem da paleta");
    painter::Palette palette;
    painter::PaletteMaterial xadrez;
    xadrez.id = "xadrez";
    xadrez.name = "Xadrez";
    xadrez.image = imageFile;
    xadrez.mips.push_back(quadrants());
    palette.add(xadrez);

    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "tinta: gravar a cena");
    painter::Document document;
    document.setPalette(&palette);
    std::string error;
    check(document.open(source, error), "tinta: abrir a cena");
    const auto hitA = ray(document, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto hitB = ray(document, {4.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto hitWall = ray(document, {0.25f, 1.25f, 5.0f}, {0.0f, 0.0f, -1.0f});
    check(hitA && hitB && hitWall, "tinta: raios na cena");
    if (!hitA || !hitB || !hitWall) {
        return;
    }
    const glm::vec3 color{0.5f, 1.0f, 0.5f};

    // Chão A tingido, chão B sem tinta: duas superfícies da mesma imagem, cores diferentes.
    painter::Fill tinted;
    tinted.target = SurfaceTarget::Shared;
    tinted.material = 0;
    tinted.color = color;
    tinted.tint = true;
    document.fill(document.select(*hitA, painter::Selection::Wall), tinted);
    painter::Fill plain = tinted;
    plain.tint = false;
    document.fill(document.select(*hitB, painter::Selection::Wall), plain);
    check(near(colorAt(document, 0.25f, 0.25f), {0.5f, 0.0f, 0.0f}) && near(colorAt(document, 0.75f, 0.25f), {0.0f, 1.0f, 0.0f}),
          "tinta: o chão A é a imagem vezes a cor");
    check(near(colorAt(document, 4.25f, 0.25f), {1.0f, 0.0f, 0.0f}), "tinta: o chão B continua sem tinta");
    check(document.paletteAt(*hitA) == 0 && document.tintAt(*hitA) && near(*document.tintAt(*hitA), color, 0.01f) &&
              !document.tintAt(*hitB),
          "tinta: conta-gotas acha o material e a cor");
    int sharedSurfaces = 0;
    int tintedSurfaces = 0;
    for (const auto& surface : document.surfaces()) {
        if (surface.kind == painter::Document::Surface::Kind::Shared) {
            ++sharedSurfaces;
            tintedSurfaces += near(glm::vec3{surface.color}, color, 0.01f) ? 1 : 0;
        }
    }
    check(sharedSurfaces == 2 && tintedSurfaces == 1, "tinta: uma superfície tingida e outra não");

    // Parede: Único com o xadrez tingido, assado na tela. Em (0,25; 1,25) a imagem dá o azul.
    painter::Fill unique = tinted;
    unique.target = SurfaceTarget::Unique;
    document.fill(document.select(*hitWall, painter::Selection::Wall), unique);
    check(near(colorOf(document, hitWall), {0.0f, 0.0f, 0.5f}, 0.1f), "tinta: parede tingida na tela");

    // Desfazer e refazer a tinta do chão A.
    document.undo();
    document.undo();
    check(document.undo() && !document.tintAt(*hitA) && targetOf(document, hitA) == SurfaceTarget::Unique,
          "tinta: desfazer");
    check(document.redo() && document.tintAt(*hitA).has_value(), "tinta: refazer");
    document.redo();
    document.redo();

    // Exportar: dois materiais da mesma imagem; o tingido leva a cor no baseColorFactor.
    const fs::path exported = folder / "modelos" / "cena.glb";
    std::string report;
    check(document.exportGlb(exported, report), "tinta: exportar");
    const auto back = lkz::asset::loadGltf(exported);
    check(back.has_value(), "tinta: ler o exportado");
    if (back) {
        int plainShared = 0;
        int tintedShared = 0;
        for (const lkz::asset::MaterialData& material : back->materials) {
            if (material.target != SurfaceTarget::Shared) {
                continue;
            }
            const lkz::asset::ImageData& image = back->images[static_cast<std::size_t>(material.image)];
            check(material.shared == "xadrez" && image.uri == "../textures/xadrez.png", "tinta: aponta para a imagem");
            if (glm::vec3{material.baseColor} == glm::vec3{1.0f}) {
                ++plainShared;
            } else if (near(glm::vec3{material.baseColor}, color, 0.01f)) {
                ++tintedShared;
            }
        }
        check(plainShared == 1 && tintedShared == 1, "tinta: um material sem tinta e um tingido");
    }

    // Reabrir: a tinta volta.
    painter::Document again;
    again.setPalette(&palette);
    check(again.open(exported, error), "tinta: reabrir");
    const auto againA = ray(again, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    const auto againB = ray(again, {4.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    check(againA && again.tintAt(*againA) && near(*again.tintAt(*againA), color, 0.01f) &&
              near(colorOf(again, againA), {0.5f, 0.0f, 0.0f}),
          "tinta: reaberto, chão A tingido");
    check(againB && !again.tintAt(*againB) && near(colorOf(again, againB), {1.0f, 0.0f, 0.0f}),
          "tinta: reaberto, chão B sem tinta");
}

/// Carimbo com cor (multiplicada e só no branco) e com profundidade (afunda e salta),
/// e a Mão num carimbo afundado.
void relief(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;
    using painter::DecalTint;
    fs::create_directories(folder / "decals");
    check(lkz::asset::saveImage(folder / "decals" / "porta.png", quadrants()), "relevo: gravar a porta");
    std::string report;
    std::string error;
    const painter::Palette library = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "relevo: gravar a cena");
    painter::Document document;
    document.setDecals(&library);
    check(document.open(source, error) && library.size() == 1, "relevo: abrir a cena");
    const auto hitFrom = [&](glm::vec3 origin, glm::vec3 direction) { return ray(document, origin, direction); };
    const auto atWall = [&](float x, float y) { return colorOf(document, hitFrom({x, y, 5.0f}, {0.0f, 0.0f, -1.0f})); };
    const auto faceAt = [&](float x, float y) {
        const auto hit = hitFrom({x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? document.faceIndex(hit->where) : -1;
    };
    const auto depthAt = [&](float x, float y) {
        const auto hit = hitFrom({x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? hit->position.z : 99.0f;
    };
    const auto normalOf = [&](const std::optional<lkz::asset::RayHit>& hit) {
        return hit ? document.faceNormal(document.faceIndex(hit->where)) : glm::vec3{0.0f};
    };
    const glm::vec3 gray{0.5f};
    const glm::vec3 red{1.0f, 0.0f, 0.0f};
    const glm::vec3 color{0.5f, 1.0f, 0.5f};

    // Porta: x de -0,5 a 0,5, y de 0,5 a 1,5 (vermelho em cima à esquerda, branco embaixo à direita).
    painter::DecalPlacement door;
    door.decal = 0;
    door.face = faceAt(0.0f, 1.0f);
    door.origin = {-0.5f, -1.5f};
    door.size = {1.0f, 1.0f};

    // Multiplicar: a imagem toda vezes a cor; a imagem da biblioteca continua uma só.
    painter::DecalPlacement multiply = door;
    multiply.tintMode = DecalTint::Multiply;
    multiply.tint = color;
    check(document.stamp(multiply) > 0, "relevo: carimbo multiplicado");
    check(near(atWall(-0.25f, 1.25f), {0.5f, 0.0f, 0.0f}) && near(atWall(0.25f, 0.75f), color),
          "relevo: multiplicado, o vermelho escurece e o branco vira a cor");
    int tintedDecals = 0;
    for (const auto& surface : document.surfaces()) {
        tintedDecals += surface.kind == painter::Document::Surface::Kind::Decal && near(glm::vec3{surface.color}, color) ? 1 : 0;
    }
    check(tintedDecals == 1, "relevo: a superfície do decalque leva a cor");
    std::string exported;
    check(document.exportGlb(folder / "modelos" / "multiplicado.glb", exported), "relevo: exportar o multiplicado");
    if (const auto back = lkz::asset::loadGltf(folder / "modelos" / "multiplicado.glb")) {
        int found = 0;
        for (const auto& material : back->materials) {
            if (material.target == SurfaceTarget::Decal) {
                const auto& image = back->images[static_cast<std::size_t>(material.image)];
                found += near(glm::vec3{material.baseColor}, color) && !image.uri.empty() ? 1 : 0;
            }
        }
        check(found == 1, "relevo: no .glb, a imagem da biblioteca com a cor no material");
    }
    painter::Document reopened;
    reopened.setDecals(&library);
    check(reopened.open(folder / "modelos" / "multiplicado.glb", error) &&
              near(colorOf(reopened, ray(reopened, {0.25f, 0.75f, 5.0f}, {0.0f, 0.0f, -1.0f})), color),
          "relevo: reaberto, o decalque continua com a cor");
    check(document.undo() && near(atWall(-0.25f, 1.25f), gray), "relevo: desfazer o multiplicado");

    // Só no branco: o vermelho fica, o branco vira a cor; a imagem tingida vai embutida.
    check(painter::neutralWeight({1.0f, 1.0f, 1.0f}) == 1.0f && painter::neutralWeight({0.5f, 0.5f, 0.5f}) == 1.0f &&
              painter::neutralWeight({1.0f, 0.0f, 0.0f}) == 0.0f && painter::neutralWeight({0.45f, 0.3f, 0.2f}) == 0.0f,
          "relevo: peso do branco (1 no cinza, 0 na cor e na ferrugem)");
    painter::DecalPlacement whiteOnly = door;
    whiteOnly.tintMode = DecalTint::WhiteOnly;
    whiteOnly.tint = color;
    check(document.stamp(whiteOnly) > 0, "relevo: carimbo só no branco");
    check(near(atWall(-0.25f, 1.25f), red) && near(atWall(0.25f, 0.75f), color),
          "relevo: só no branco, o vermelho fica e o branco vira a cor");
    check(document.exportGlb(folder / "modelos" / "branco.glb", exported), "relevo: exportar o só no branco");
    if (const auto back = lkz::asset::loadGltf(folder / "modelos" / "branco.glb")) {
        int found = 0;
        for (const auto& material : back->materials) {
            if (material.target == SurfaceTarget::Decal) {
                const auto& image = back->images[static_cast<std::size_t>(material.image)];
                found += glm::vec3{material.baseColor} == glm::vec3{1.0f} && image.uri.empty() ? 1 : 0;
            }
        }
        check(found == 1, "relevo: no .glb, a imagem tingida embutida");
    }
    check(document.undo(), "relevo: desfazer o só no branco");

    // Afundar 20 cm: a porta vai para z = -1,2 e o vão ganha quatro paredes viradas para dentro.
    painter::DecalPlacement sunk = door;
    sunk.depth = -0.2f;
    const std::size_t flat = document.model().triangleCount();
    check(document.stamp(sunk) > 0, "relevo: afundar");
    check(std::abs(depthAt(0.0f, 1.0f) + 1.2f) < 1e-3f && near(atWall(-0.25f, 1.25f), red), "relevo: a porta lá dentro");
    check(std::abs(depthAt(0.8f, 1.0f) + 1.0f) < 1e-3f && near(atWall(0.8f, 1.0f), gray), "relevo: a parede em volta no lugar");
    const auto left = hitFrom({0.0f, 1.0f, -1.1f}, {-1.0f, 0.0f, 0.0f});
    const auto right = hitFrom({0.0f, 1.0f, -1.1f}, {1.0f, 0.0f, 0.0f});
    const auto top = hitFrom({0.0f, 1.0f, -1.1f}, {0.0f, 1.0f, 0.0f});
    const auto bottom = hitFrom({0.0f, 1.0f, -1.1f}, {0.0f, -1.0f, 0.0f});
    check(left && std::abs(left->position.x + 0.5f) < 1e-3f && near(normalOf(left), {1.0f, 0.0f, 0.0f}) && right &&
              std::abs(right->position.x - 0.5f) < 1e-3f && near(normalOf(right), {-1.0f, 0.0f, 0.0f}) && top &&
              std::abs(top->position.y - 1.5f) < 1e-3f && near(normalOf(top), {0.0f, -1.0f, 0.0f}) && bottom &&
              std::abs(bottom->position.y - 0.5f) < 1e-3f && near(normalOf(bottom), {0.0f, 1.0f, 0.0f}),
          "relevo: as quatro paredes do vão, viradas para dentro");
    check(near(colorOf(document, left), gray) && targetOf(document, left) == SurfaceTarget::Unique,
          "relevo: o vão com o destino da parede");
    check(!document.stampAt(document.faceIndex(left->where)), "relevo: o vão não é um carimbo para a Mão pegar");

    // Mão: pegar tira o vão inteiro; desistir devolve; mover leva a profundidade junto.
    const std::size_t sunkTriangles = document.model().triangleCount();
    auto lifted = document.lift(faceAt(0.0f, 1.0f));
    check(lifted && std::abs(lifted->depth + 0.2f) < 1e-6f && std::abs(depthAt(0.0f, 1.0f) + 1.0f) < 1e-3f &&
              near(atWall(0.0f, 1.0f), gray) && !hitFrom({0.0f, 1.0f, -1.1f}, {-1.0f, 0.0f, 0.0f}),
          "relevo: pegar a porta afundada fecha a parede");
    document.cancelLift();
    check(std::abs(depthAt(0.0f, 1.0f) + 1.2f) < 1e-3f && document.model().triangleCount() == sunkTriangles,
          "relevo: desistir devolve o vão");
    lifted = document.lift(faceAt(0.0f, 1.0f));
    painter::DecalPlacement moved = *lifted;
    moved.origin.x = -0.9f; // x de -0,9 a 0,1
    check(document.stamp(moved) > 0 && std::abs(depthAt(-0.4f, 1.0f) + 1.2f) < 1e-3f &&
              std::abs(depthAt(0.3f, 1.0f) + 1.0f) < 1e-3f,
          "relevo: mover a porta afundada");
    check(document.undo() && std::abs(depthAt(0.3f, 1.0f) + 1.2f) < 1e-3f && document.model().triangleCount() == sunkTriangles,
          "relevo: desfazer o movimento");
    check(document.undo() && document.model().triangleCount() == flat && std::abs(depthAt(0.0f, 1.0f) + 1.0f) < 1e-3f,
          "relevo: desfazer o afundar");

    // Saltar 10 cm: a placa na frente, com as laterais viradas para fora.
    painter::DecalPlacement raised = door;
    raised.depth = 0.1f;
    check(document.stamp(raised) > 0 && std::abs(depthAt(0.0f, 1.0f) + 0.9f) < 1e-3f, "relevo: saltar");
    const auto outside = hitFrom({-0.8f, 1.0f, -0.95f}, {1.0f, 0.0f, 0.0f});
    check(outside && std::abs(outside->position.x + 0.5f) < 1e-3f && near(normalOf(outside), {-1.0f, 0.0f, 0.0f}),
          "relevo: a lateral da placa virada para fora");
    check(document.exportGlb(folder / "modelos" / "saltado.glb", exported), "relevo: exportar com relevo");
    painter::Document again;
    again.setDecals(&library);
    check(again.open(folder / "modelos" / "saltado.glb", error) &&
              std::abs(ray(again, {0.0f, 1.0f, 5.0f}, {0.0f, 0.0f, -1.0f})->position.z + 0.9f) < 1e-3f,
          "relevo: reaberto, a placa continua na frente");
    check(document.undo(), "relevo: desfazer o saltar");

    // Selecionados: duas portas planas; afundar e tingir as duas num passo só, pelo id.
    painter::DecalPlacement small = door;
    small.size = {0.4f, 0.4f};
    small.origin = {-0.9f, -1.9f}; // x de -0,9 a -0,5, y de 1,5 a 1,9
    check(document.stamp(small) > 0, "seleção: porta A");
    const int a = document.lastStampId();
    small.origin = {0.4f, -1.9f}; // x de 0,4 a 0,8
    check(document.stamp(small) > 0, "seleção: porta B");
    const int b = document.lastStampId();
    check(a >= 0 && b >= 0 && a != b && document.stampId(faceAt(-0.7f, 1.7f)) == a && document.stampId(faceAt(0.6f, 1.7f)) == b &&
              document.stampId(faceAt(0.0f, 1.0f)) == -1 && document.stampById(a) && document.stampFacesById(b).size() > 0,
          "seleção: ids estáveis pela face");
    const std::size_t two = document.model().triangleCount();
    std::vector<std::pair<int, painter::DecalPlacement>> changes;
    for (const int id : {a, b}) {
        painter::DecalPlacement next = *document.stampById(id);
        next.depth = -0.1f;
        next.tintMode = DecalTint::Multiply;
        next.tint = color;
        changes.emplace_back(id, next);
    }
    const std::vector<int> ids = document.replaceStamps(changes);
    check(ids.size() == 2 && ids[0] >= 0 && ids[1] >= 0 && std::abs(depthAt(-0.7f, 1.7f) + 1.1f) < 1e-3f &&
              std::abs(depthAt(0.6f, 1.7f) + 1.1f) < 1e-3f && near(atWall(-0.8f, 1.8f), {0.5f, 0.0f, 0.0f}),
          "seleção: as duas afundadas e tingidas");
    check(document.stampById(ids[0])->depth == -0.1f && !document.stampById(a), "seleção: id novo, o antigo não existe mais");
    int degenerate = 0;
    for (const auto& part : document.model().parts) {
        for (std::size_t t = 0; t < lkz::asset::triangleCount(part.mesh); ++t) {
            const auto i = lkz::asset::triangleAt(part.mesh, t);
            const glm::vec3 n = glm::cross(part.mesh.positions[i[1]] - part.mesh.positions[i[0]],
                                           part.mesh.positions[i[2]] - part.mesh.positions[i[0]]);
            degenerate += glm::length(n) < 1e-9f ? 1 : 0;
        }
    }
    check(degenerate == 0, "seleção: recarimbar no mesmo lugar não deixa triângulos de área zero");
    check(document.undo() && std::abs(depthAt(-0.7f, 1.7f) + 1.0f) < 1e-3f && std::abs(depthAt(0.6f, 1.7f) + 1.0f) < 1e-3f &&
              document.model().triangleCount() == two && document.stampById(a) && document.stampById(b),
          "seleção: um desfazer volta as duas (e os ids de antes)");
    check(document.redo() && std::abs(depthAt(0.6f, 1.7f) + 1.1f) < 1e-3f, "seleção: refazer");
    painter::DecalPlacement gone;
    gone.decal = -1;
    const std::vector<int> deleted = document.replaceStamps({{ids[0], gone}, {ids[1], gone}});
    check(deleted == std::vector<int>{-1, -1} && std::abs(depthAt(-0.7f, 1.7f) + 1.0f) < 1e-3f && near(atWall(-0.7f, 1.7f), gray) &&
              document.stampId(faceAt(0.6f, 1.7f)) == -1,
          "seleção: apagar as duas");
    check(document.undo() && std::abs(depthAt(0.6f, 1.7f) + 1.1f) < 1e-3f, "seleção: desfazer o apagar");
}

/// Esculpir: cortar um retângulo, Puxar (afundar e saltar com paredes) e Mover (deformar),
/// sem frestas; vértices em T de um carimbo não viram paredes falsas.
void sculpt(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using lkz::asset::SurfaceTarget;
    using painter::SculptMode;
    fs::create_directories(folder / "decals");
    check(lkz::asset::saveImage(folder / "decals" / "porta.png", quadrants()), "esculpir: gravar a porta");
    std::string report;
    std::string error;
    const painter::Palette library = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "esculpir: gravar a cena");
    painter::Document document;
    document.setDecals(&library);
    check(document.open(source, error), "esculpir: abrir a cena");
    const auto hitFrom = [&](glm::vec3 origin, glm::vec3 direction) { return ray(document, origin, direction); };
    const auto faceAt = [&](float x, float y) {
        const auto hit = hitFrom({x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? document.faceIndex(hit->where) : -1;
    };
    const auto depthAt = [&](float x, float y) {
        const auto hit = hitFrom({x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? hit->position.z : 99.0f;
    };
    const auto normalOf = [&](const std::optional<lkz::asset::RayHit>& hit) {
        return hit ? document.faceNormal(document.faceIndex(hit->where)) : glm::vec3{0.0f};
    };
    // Sem frestas: raios de frente numa grade da parede, todos acertam.
    const auto closed = [&]() {
        for (float x = -0.95f; x < 0.96f; x += 0.1f) {
            for (float y = 0.05f; y < 1.96f; y += 0.1f) {
                if (depthAt(x, y) > 50.0f) {
                    return false;
                }
            }
        }
        return true;
    };
    const glm::vec3 wallNormal{0.0f, 0.0f, 1.0f};
    const std::size_t start = document.model().triangleCount();

    // Retângulo x de -0,5 a 0,5, y de 0,5 a 1,5 (v = -y).
    const int wall = faceAt(0.0f, 1.0f);
    const std::vector<int> inside = document.cutRectangle(wall, {-0.5f, -1.5f}, {0.5f, -0.5f});
    check(!inside.empty() && std::abs(document.area(inside) - 1.0f) < 1e-4f && closed() && std::abs(depthAt(0.0f, 1.0f) + 1.0f) < 1e-4f,
          "esculpir: o retângulo cortado (1 m², a parede igual)");

    // Puxar para dentro 20 cm: o vão com quatro paredes viradas para dentro.
    check(document.beginSculpt(inside, SculptMode::Extrude, wallNormal), "esculpir: começar");
    document.updateSculpt(-0.1f);
    document.updateSculpt(-0.2f); // recomeça do início: -0,2 e não -0,3
    check(std::abs(depthAt(0.0f, 1.0f) + 1.2f) < 1e-4f && std::abs(depthAt(0.8f, 1.0f) + 1.0f) < 1e-4f && closed(),
          "esculpir: afundado ao vivo, sem frestas");
    const std::vector<int> pushed = document.endSculpt();
    check(pushed.size() == inside.size(), "esculpir: confirmar devolve as faces esculpidas");
    const auto left = hitFrom({0.0f, 1.0f, -1.1f}, {-1.0f, 0.0f, 0.0f});
    const auto top = hitFrom({0.0f, 1.0f, -1.1f}, {0.0f, 1.0f, 0.0f});
    check(left && std::abs(left->position.x + 0.5f) < 1e-4f && near(normalOf(left), {1.0f, 0.0f, 0.0f}) && top &&
              std::abs(top->position.y - 1.5f) < 1e-4f && near(normalOf(top), {0.0f, -1.0f, 0.0f}),
          "esculpir: as paredes do vão viradas para dentro");
    check(near(colorOf(document, left), {0.5f, 0.5f, 0.5f}) && targetOf(document, left) == SurfaceTarget::Unique,
          "esculpir: o vão com o acabamento da parede em volta");

    // De novo, nas mesmas faces: sai 50 cm (fica 30 cm para fora), as laterais viradas para fora.
    check(document.beginSculpt(pushed, SculptMode::Extrude, wallNormal), "esculpir: puxar de novo");
    document.updateSculpt(0.5f);
    document.endSculpt();
    const auto outside = hitFrom({-0.8f, 1.0f, -0.8f}, {1.0f, 0.0f, 0.0f});
    check(std::abs(depthAt(0.0f, 1.0f) + 0.7f) < 1e-4f && outside && std::abs(outside->position.x + 0.5f) < 1e-4f &&
              near(normalOf(outside), {-1.0f, 0.0f, 0.0f}) && closed(),
          "esculpir: saltado, as laterais para fora");
    check(document.undo() && document.undo() && document.undo() && document.model().triangleCount() == start &&
              std::abs(depthAt(0.0f, 1.0f) + 1.0f) < 1e-4f,
          "esculpir: desfazer volta a parede inteira");

    // Desistir no meio e confirmar sem andar não mudam nada.
    check(document.beginSculpt({faceAt(0.0f, 1.0f)}, SculptMode::Extrude, wallNormal), "esculpir: começar e desistir");
    document.updateSculpt(0.3f);
    document.cancelSculpt();
    check(document.model().triangleCount() == start && std::abs(depthAt(0.5f, 0.5f) + 1.0f) < 1e-4f, "esculpir: desistir");
    document.beginSculpt({faceAt(0.0f, 1.0f)}, SculptMode::Extrude, wallNormal);
    check(document.endSculpt().empty() && document.model().triangleCount() == start && !document.canUndo(),
          "esculpir: confirmar sem andar não deixa nada");

    // Mover: o meio da parede sai 30 cm e as vizinhas esticam (sem paredes novas nem frestas).
    const std::vector<int> middle = document.cutRectangle(faceAt(0.0f, 1.0f), {-0.5f, -1.5f}, {0.5f, -0.5f});
    document.beginSculpt(middle, SculptMode::Move, wallNormal);
    document.updateSculpt(0.3f);
    document.endSculpt();
    const float slope = depthAt(0.55f, 1.0f); // na rampa entre a borda (z = -1) e o meio (z = -0,7)
    const bool noWalls = std::none_of(document.model().parts.begin(), document.model().parts.end(),
                                      [](const lkz::asset::ModelPart& part) { return part.name == "esculpir"; });
    check(std::abs(depthAt(0.0f, 1.0f) + 0.7f) < 1e-4f && slope > -1.0f && slope < -0.7f && closed() && noWalls,
          "esculpir: mover deforma as vizinhas, sem paredes novas");
    check(document.undo() && document.undo() && document.model().triangleCount() == start, "esculpir: desfazer o mover");

    // Mover o chão A 50 cm para cima: a parede encostada nele estica junto.
    const auto floor = ray(document, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    document.beginSculpt(document.select(*floor, painter::Selection::Wall), SculptMode::Move, {0.0f, 1.0f, 0.0f});
    document.updateSculpt(0.5f);
    document.endSculpt();
    const auto raised = ray(document, {0.25f, 5.0f, 0.25f}, {0.0f, -1.0f, 0.0f});
    check(raised && std::abs(raised->position.y - 0.5f) < 1e-4f && depthAt(0.0f, 0.3f) > 50.0f &&
              std::abs(depthAt(0.0f, 0.7f) + 1.0f) < 1e-4f,
          "esculpir: o chão subiu e a parede encolheu junto");
    document.undo();

    // Vértices em T: uma porta carimbada na parede; puxar a parede inteira não cria paredes
    // por dentro (nas bordas da porta), só nas bordas de fora.
    painter::DecalPlacement door;
    door.decal = 0;
    door.face = faceAt(0.0f, 1.0f);
    door.origin = {-0.3f, -1.4f};
    door.size = {0.6f, 0.8f};
    check(document.stamp(door) > 0, "esculpir: porta na parede");
    std::vector<int> whole;
    for (int f = 0; f < document.faceCount(); ++f) {
        if (std::abs(document.faceNormal(f).z - 1.0f) < 1e-4f) {
            whole.push_back(f);
        }
    }
    document.beginSculpt(whole, SculptMode::Extrude, wallNormal);
    document.updateSculpt(0.2f);
    document.endSculpt();
    const auto across = hitFrom({-1.5f, 1.0f, -0.9f}, {1.0f, 0.0f, 0.0f});
    check(across && std::abs(across->position.x + 1.0f) < 1e-4f, "esculpir: a lateral de fora");
    const auto inner = hitFrom({-0.9f, 1.0f, -0.9f}, {1.0f, 0.0f, 0.0f});
    check(!inner || inner->position.x > 0.999f, "esculpir: nenhuma parede falsa nas bordas da porta");
    check(std::abs(depthAt(0.0f, 1.0f) + 0.8f) < 1e-4f && std::abs(depthAt(0.8f, 1.0f) + 0.8f) < 1e-4f && closed(),
          "esculpir: a parede e a porta saíram juntas");
    std::string exported;
    check(document.exportGlb(folder / "modelos" / "esculpido.glb", exported), "esculpir: exportar");
}

/// Carimbos gravados no .glb: reabrir devolve carimbos ajustáveis (cor, profundidade e o
/// vão), que a Mão tira, move e apaga; de novo depois de exportar e do UV único.
void reopen(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    using painter::DecalTint;
    fs::create_directories(folder / "decals");
    check(lkz::asset::saveImage(folder / "decals" / "porta.png", quadrants()), "reabrir: gravar a porta");
    std::string report;
    std::string error;
    const painter::Palette library = painter::Palette::load(folder / "decals", report, painter::Palette::Kind::Decals);
    const fs::path source = folder / "cena.glb";
    check(lkz::asset::saveGlb(source, scene()), "reabrir: gravar a cena");
    const glm::vec3 gray{0.5f};
    const glm::vec3 color{0.5f, 1.0f, 0.5f};
    const auto faceAt = [](const painter::Document& d, float x, float y) {
        const auto hit = ray(d, {x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? d.faceIndex(hit->where) : -1;
    };
    const auto depthAt = [](const painter::Document& d, float x, float y) {
        const auto hit = ray(d, {x, y, 5.0f}, {0.0f, 0.0f, -1.0f});
        return hit ? hit->position.z : 99.0f;
    };
    const auto colorAtWall = [](const painter::Document& d, float x, float y) {
        return colorOf(d, ray(d, {x, y, 5.0f}, {0.0f, 0.0f, -1.0f}));
    };

    painter::Document document;
    document.setDecals(&library);
    check(document.open(source, error), "reabrir: abrir a cena");
    // A: porta de x -0,5 a 0,5, y 0,5 a 1,5, afundada 20 cm e multiplicada. B: placa pequena no canto.
    painter::DecalPlacement a;
    a.decal = 0;
    a.face = faceAt(document, 0.0f, 1.0f);
    a.origin = {-0.5f, -1.5f};
    a.size = {1.0f, 1.0f};
    a.tintMode = DecalTint::Multiply;
    a.tint = color;
    a.depth = -0.2f;
    check(document.stamp(a) > 0, "reabrir: porta A");
    painter::DecalPlacement b = a;
    b.face = faceAt(document, 0.75f, 1.75f);
    b.origin = {0.6f, -1.9f};
    b.size = {0.3f, 0.3f};
    b.tintMode = DecalTint::None;
    b.tint = glm::vec3{1.0f};
    b.depth = 0.0f;
    check(document.stamp(b) > 0, "reabrir: placa B");
    std::string exported;
    const fs::path first = folder / "modelos" / "primeira.glb";
    check(document.exportGlb(first, exported), "reabrir: exportar");

    painter::Document again;
    again.setDecals(&library);
    check(again.open(first, error) && again.summary().find("2 carimbos ajustáveis") != std::string::npos,
          "reabrir: os dois carimbos voltam ajustáveis");
    const auto placedA = again.stampById(again.stampId(faceAt(again, 0.0f, 1.0f)));
    check(placedA && placedA->origin == a.origin && placedA->size == a.size && placedA->tintMode == DecalTint::Multiply &&
              near(placedA->tint, color) && std::abs(placedA->depth + 0.2f) < 1e-5f,
          "reabrir: a porta com a cor e a profundidade");
    check(again.stampId(faceAt(again, 0.75f, 1.75f)) >= 0 && std::abs(depthAt(again, 0.0f, 1.0f) + 1.2f) < 1e-4f &&
              near(colorAtWall(again, -0.25f, 1.25f), {0.5f, 0.0f, 0.0f}),
          "reabrir: a porta afundada e tingida, como estava");

    // Pegar a porta reaberta: a parede fecha, com a cor da parede em volta, e o vão sai junto.
    check(again.lift(faceAt(again, 0.0f, 1.0f)).has_value() && std::abs(depthAt(again, 0.0f, 1.0f) + 1.0f) < 1e-4f &&
              near(colorAtWall(again, 0.0f, 1.0f), gray) && !ray(again, {0.0f, 1.0f, -1.1f}, {-1.0f, 0.0f, 0.0f}),
          "reabrir: pegar a porta fecha a parede e tira o vão");
    again.cancelLift();
    check(std::abs(depthAt(again, 0.0f, 1.0f) + 1.2f) < 1e-4f && ray(again, {0.0f, 1.0f, -1.1f}, {-1.0f, 0.0f, 0.0f}).has_value(),
          "reabrir: desistir devolve a porta e o vão");

    // Mover a porta e apagar a placa.
    painter::DecalPlacement moved = *again.stampById(again.stampId(faceAt(again, 0.0f, 1.0f)));
    moved.origin.x = -0.9f; // x de -0,9 a 0,1
    const int idA = again.stampId(faceAt(again, 0.0f, 1.0f));
    const int idB = again.stampId(faceAt(again, 0.75f, 1.75f));
    painter::DecalPlacement gone;
    gone.decal = -1;
    const std::vector<int> ids = again.replaceStamps({{idA, moved}, {idB, gone}});
    check(ids.size() == 2 && ids[0] >= 0 && ids[1] == -1 && std::abs(depthAt(again, -0.4f, 1.0f) + 1.2f) < 1e-4f &&
              std::abs(depthAt(again, 0.3f, 1.0f) + 1.0f) < 1e-4f && near(colorAtWall(again, 0.3f, 1.0f), gray) &&
              near(colorAtWall(again, 0.75f, 1.75f), gray) && std::abs(depthAt(again, 0.75f, 1.75f) + 1.0f) < 1e-4f,
          "reabrir: mover a porta e apagar a placa");

    // Exportar de novo e reabrir: só a porta, no lugar novo; continua ajustável depois do UV único.
    const fs::path second = folder / "modelos" / "segunda.glb";
    check(again.exportGlb(second, exported), "reabrir: exportar de novo");
    painter::Document third;
    third.setDecals(&library);
    check(third.open(second, error) && third.summary().find("1 carimbo ajustável") != std::string::npos &&
              third.stampId(faceAt(third, -0.4f, 1.0f)) >= 0 && third.stampById(third.stampId(faceAt(third, -0.4f, 1.0f)))->origin.x == -0.9f,
          "reabrir: a segunda vez, a porta no lugar novo");
    lkz::asset::UnwrapOptions options;
    std::string uvReport;
    check(third.makeUniqueUv(options, uvReport) && third.stampId(faceAt(third, -0.4f, 1.0f)) >= 0 &&
              std::abs(depthAt(third, -0.4f, 1.0f) + 1.2f) < 1e-4f,
          "reabrir: depois do UV único, a porta continua ajustável");
    check(third.lift(faceAt(third, -0.4f, 1.0f)).has_value() && std::abs(depthAt(third, -0.4f, 1.0f) + 1.0f) < 1e-4f,
          "reabrir: e a Mão ainda a tira");

    // Um .glb de antes da lista (sem extras): os carimbos voltam pelo UV (retângulo e giro).
    painter::Document old;
    old.setDecals(&library);
    check(old.open(source, error), "reabrir: cena para o arquivo antigo");
    painter::DecalPlacement turned;
    turned.decal = 0;
    turned.face = faceAt(old, 0.0f, 1.0f);
    turned.origin = {-0.6f, -1.7f};
    turned.size = {0.8f, 1.2f};
    turned.quarterTurns = 1;
    turned.tintMode = DecalTint::Multiply;
    turned.tint = color;
    check(old.stamp(turned) > 0, "reabrir: porta girada e tingida");
    const glm::vec3 before = colorAtWall(old, -0.4f, 1.0f);
    const fs::path legacy = folder / "modelos" / "antigo.glb";
    check(old.exportGlb(legacy, exported), "reabrir: exportar o antigo");
    auto stripped = lkz::asset::loadGltf(legacy);
    check(stripped.has_value(), "reabrir: ler o antigo");
    if (stripped) {
        stripped->extras.clear(); // como um arquivo de antes da lista
        check(lkz::asset::saveGlb(legacy, *stripped), "reabrir: gravar sem a lista");
    }
    painter::Document legacyDoc;
    legacyDoc.setDecals(&library);
    check(legacyDoc.open(legacy, error) && legacyDoc.summary().find("1 carimbo ajustável") != std::string::npos,
          "reabrir: o carimbo do arquivo antigo volta pelo UV");
    const auto adopted = legacyDoc.stampById(legacyDoc.stampId(faceAt(legacyDoc, -0.4f, 1.0f)));
    check(adopted && glm::length(adopted->origin - turned.origin) < 1e-4f && glm::length(adopted->size - turned.size) < 1e-4f &&
              adopted->quarterTurns == 1 && adopted->tintMode == DecalTint::Multiply && near(adopted->tint, color),
          "reabrir: o retângulo, o giro e a cor certos");
    check(near(colorAtWall(legacyDoc, -0.4f, 1.0f), before) && legacyDoc.lift(faceAt(legacyDoc, -0.4f, 1.0f)).has_value() &&
              near(colorAtWall(legacyDoc, -0.4f, 1.0f), gray),
          "reabrir: a Mão tira o carimbo do arquivo antigo");
    legacyDoc.cancelLift();
    check(near(colorAtWall(legacyDoc, -0.4f, 1.0f), before), "reabrir: e devolve igual");
}

} // namespace

int main() {
    texelMap();

    const auto folder = std::filesystem::temp_directory_path() / "lkz_painter_test";
    std::filesystem::create_directories(folder);
    const auto source = folder / "quadrado.glb";
    const auto exported = folder / "quadrado_pintado.glb";
    check(lkz::asset::saveGlb(source, quad()), "gravar o quadrado");

    painter::Document document;
    std::string error;
    check(document.open(source, error), "abrir o quadrado");
    std::vector<painter::Document::Surface> surfaces = document.surfaces();
    check(surfaces.size() == 1 && document.canvasImage(0).width == 64, "tela de 64 x 64 (32 px/m em 2 m)");
    check(near(colorAt(document, 0.0f, 0.0f), glm::vec3{0.5f}), "cinza ao abrir");

    // Pincelada vermelha, dura, de 25 cm no meio, vista de cima.
    painter::Brush brush;
    brush.color = {1.0f, 0.0f, 0.0f};
    brush.radius = 0.25f;
    brush.hardness = 1.0f;
    document.beginStroke();
    document.dab({0.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f}, brush);
    std::vector<painter::CanvasPatch> patches;
    document.takePatches(patches);
    check(patches.size() == 1 && !patches[0].patch.strokeEnd, "um pedaço durante o traço");
    // A mesma pincelada de novo não acumula nem muda nada.
    document.dab({0.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f}, brush);
    patches.clear();
    document.takePatches(patches);
    check(patches.empty(), "pincelada repetida no mesmo traço não muda nada");
    document.endStroke();
    patches.clear();
    document.takePatches(patches);
    check(patches.size() == 1 && patches[0].patch.strokeEnd, "fim do traço manda os mipmaps");
    check(near(colorAt(document, 0.0f, 0.0f), glm::vec3{1.0f, 0.0f, 0.0f}), "vermelho no meio");
    check(near(colorAt(document, 0.8f, 0.8f), glm::vec3{0.5f}), "cinza longe do pincel");
    check(document.modified(), "modificado depois de pintar");

    // Vista de baixo: o lado de trás não é pintado.
    brush.color = {0.0f, 0.0f, 1.0f};
    document.beginStroke();
    document.dab({0.6f, 0.0f, 0.6f}, {0.6f, -5.0f, 0.6f}, brush);
    document.endStroke();
    check(near(colorAt(document, 0.6f, 0.6f), glm::vec3{0.5f}), "de costas para a câmera não pinta");

    check(document.undo(), "desfazer");
    check(near(colorAt(document, 0.0f, 0.0f), glm::vec3{0.5f}), "desfeito: cinza no meio");
    check(document.redo(), "refazer");
    check(near(colorAt(document, 0.0f, 0.0f), glm::vec3{1.0f, 0.0f, 0.0f}), "refeito: vermelho no meio");

    // Borracha numa metade do círculo: volta ao cinza do material.
    painter::Brush eraser = brush;
    eraser.erase = true;
    eraser.radius = 0.1f;
    document.beginStroke();
    document.dab({0.15f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f}, eraser);
    document.endStroke();
    check(near(colorAt(document, 0.18f, 0.0f), glm::vec3{0.5f}), "borracha volta ao original");
    check(near(colorAt(document, -0.15f, 0.0f), glm::vec3{1.0f, 0.0f, 0.0f}), "borracha não passa do raio");

    std::string report;
    check(document.exportGlb(exported, report), "exportar");
    check(!document.modified(), "exportado: não está mais modificado");
    const auto back = lkz::asset::loadGltf(exported);
    check(back.has_value(), "reler o exportado");
    if (back) {
        check(back->images.size() == 1 && back->images[0].width == 64, "uma imagem de 64 x 64 no .glb");
        check(back->materials.size() == 1 && back->materials[0].image == 0, "o material usa a imagem");
        check(back->materials[0].baseColor == glm::vec4{1.0f}, "cor base branca (a cor está na imagem)");
        painter::Document again;
        check(again.open(exported, error), "abrir o exportado no Pintor");
        // JPEG: tolerância maior.
        check(near(colorAt(again, -0.15f, 0.0f), glm::vec3{1.0f, 0.0f, 0.0f}, 0.08f), "exportado: vermelho");
        check(near(colorAt(again, 0.8f, 0.8f), glm::vec3{0.5f}, 0.08f), "exportado: cinza");
    }
    std::printf("%s\n", report.c_str());

    // UV único: a pintura vai junto para o UV novo, e o desfazer recomeça.
    check(!document.needsUniqueUv() && document.uvOverlap() < 0.01f, "quadrado: UV sem sobreposição");
    lkz::asset::UnwrapOptions options;
    options.texelsPerMeter = 64.0f;
    std::string uvReport;
    check(document.makeUniqueUv(options, uvReport), "gerar UV único");
    std::printf("%s\n", uvReport.c_str());
    check(near(colorAt(document, -0.15f, 0.0f), glm::vec3{1.0f, 0.0f, 0.0f}, 0.05f), "UV único: vermelho continua");
    check(near(colorAt(document, 0.8f, 0.8f), glm::vec3{0.5f}, 0.05f), "UV único: cinza continua");
    check(!document.canUndo() && document.modified(), "UV único: sem desfazer, modificado");

    materials(folder / "materiais");
    decals(folder / "decalques");
    hand(folder / "mao");
    tint(folder / "tinta");
    relief(folder / "relevo");
    sculpt(folder / "esculpir");
    reopen(folder / "reabrir");

    std::filesystem::remove_all(folder);
    if (failures == 0) {
        std::printf("painter_test: tudo certo\n");
    }
    return failures == 0 ? 0 : 1;
}
