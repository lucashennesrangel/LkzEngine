#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "lkz/Input/Input.hpp"

namespace lkz {

enum class PrimitiveShape : std::uint8_t {
    Box,
    Sphere,
    Cylinder,
    Plane,  // quadrado no plano XZ, virado para +Y; a imagem cobre a face inteira
    Custom, // geometria do jogo (World::uploadMesh); nada é desenhado até ela chegar
    Model,  // arquivo 3D em PrimitiveMesh::model (glTF .glb/.gltf, OBJ, IQM...)
};

/// Como a imagem se repete quando as coordenadas de textura passam de 0..1.
enum class TextureTiling : std::uint8_t {
    Repeat, // grade comum: a imagem se repete igual, lado a lado
    /// Hex-tiling: a superfície vira uma grade de hexágonos, e cada um mostra a imagem com
    /// deslocamento e rotação aleatórios, misturada com os vizinhos nas bordas. A grade de
    /// repetição some em texturas orgânicas repetidas muitas vezes (grama, terra,
    /// cascalho, pedra). Custa 3 leituras da imagem por pixel. A imagem precisa emendar
    /// sem costura nas bordas (cada hexágono lê de um ponto qualquer dela).
    Hex,
};

/// Ajustes do TextureTiling::Hex (Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022).
/// Um hexágono mede cerca de 0,29 altura da imagem. Em imagem não quadrada, a grade e as
/// rotações respeitam a proporção dela (o desenho não estica).
struct HexTiling {
    float rotation = 1.0f; // quanto cada hexágono pode girar: 0 = só desloca, 1 = qualquer ângulo
    float contrast = 0.5f; // transição entre hexágonos: 0.5 = padrão; acima = mais nítida, abaixo = mais suave
};

/// Tamanho na tela: quanto da altura da tela o diâmetro da esfera envolvente do objeto
/// ocupa (1 = a tela inteira, 0.1 = um décimo). Não depende da resolução; é o que
/// decide LOD, impostor e descarte.
///
/// Um nível de detalhe a mais (LOD): versão mais leve da mesh, usada quando ela fica
/// pequena na tela. O nível 0 é a própria mesh.
struct MeshLod {
    float screenSize = 0.1f;     // entra quando o objeto ocupa menos que isto da tela
    float triangleRatio = 0.25f; // gerado simplificando o nível 0 para esta fração dos triângulos...
    std::string model{};         // ...ou lido deste arquivo (mesmas unidades e partes do original)
};

/// Impostor: de longe, o objeto vira um quadrado virado para a câmera, pintado com
/// "fotos" dele tiradas de N x N direções do hemisfério de cima (octaedro), assadas em
/// initialize() (cor e normal; a luz é aplicada na hora). Dois triângulos por objeto.
struct MeshImpostor {
    bool enabled = false;
    float screenSize = 0.03f; // entra quando o objeto ocupa menos que isto da tela
    int frames = 8;           // N (2 a 16): N x N vistas
    int frameSize = 128;      // pixels de cada vista
};

/// Geometria de um id de mesh: a primitiva ocupa uma caixa de `size` centrada em
/// `offset`, no espaço do objeto (antes da matriz de modelo).
/// Ex.: `offset.y = size.y / 2` apoia o objeto no chão em vez de enterrá-lo pela metade.
///
/// Em `Custom`, `size` e `offset` não se aplicam: a malha chega pronta.
///
/// Em `Model`, a geometria vem do arquivo `model`, nas unidades dele (glTF: metros, Y
/// para cima), com as transformações dos nós já aplicadas. `size` multiplica cada eixo
/// (1 = tamanho do arquivo) e `offset` desloca. Cada malha do arquivo vira uma parte
/// com a imagem e a cor base do seu material; todas as partes são desenhadas para cada
/// instância. Animações não são tocadas.
///
/// `texture` é o caminho de uma imagem (PNG, JPG...) aplicada pelas coordenadas de
/// textura da primitiva e multiplicada pela cor da instância. Vazio = só a cor (em
/// `Model`: as imagens do arquivo; preenchida, substitui todas). No `Plane`, o topo da
/// imagem fica em -Z e a esquerda em -X. `tiling` escolhe como ela se repete (`hex` só
/// vale com TextureTiling::Hex).
///
/// `doubleSided` desenha os dois lados de cada triângulo (folhagem, tecido, placas
/// finas); a luz vale para o lado visto. Por padrão, só a face da frente aparece.
///
/// Desempenho (para meshes com muitas cópias): `lods` (do mais detalhado ao mais
/// leve), `impostor` e `cullScreenSize` são escolhidos por cópia, pelo tamanho na
/// tela. Tudo o que está fora da câmera é sempre descartado. Em `Custom`, lods e
/// impostor não se aplicam (a malha muda a qualquer momento).
struct PrimitiveMesh {
    PrimitiveShape shape = PrimitiveShape::Box;
    glm::vec3 size{1.0f};
    glm::vec3 offset{0.0f};
    std::string texture{};
    TextureTiling tiling = TextureTiling::Repeat;
    HexTiling hex{};
    std::string model{};
    bool doubleSided = false;
    std::vector<MeshLod> lods{};
    MeshImpostor impostor{};
    float cullScreenSize = 0.0f; // menor que isto na tela: não desenha (0 = sempre desenha)
    /// Textura feita em tempo de execução (World::uploadTexture com este id) no lugar de
    /// `texture` e das imagens do modelo, em todas as partes. 0 = não usa. Até ela chegar,
    /// a mesh aparece sem imagem.
    std::uint32_t runtimeTexture = 0;
};

/// Janela e cena de um backend com janela (raylib, NVRHI...). Os mesmos dados servem a
/// qualquer um deles.
struct RenderConfig {
    std::string title = "Lkz Engine";
    int width = 1280;
    int height = 720;
    bool vsync = true;
    bool msaa = true;
    bool showStats = true; // FPS, objetos, draw calls e triângulos desenhados no canto da tela
    bool escapeClosesWindow = true; // false: ESC vira uma tecla comum (lkz::Key::Escape)
    /// Liga/desliga a cor de depuração dos níveis de detalhe: nível 0 normal, 1 azul,
    /// 2 amarelo, 3+ laranja, impostor magenta. Só F1 a F12; outra tecla desativa.
    Key lodDebugKey = Key::F3;
    /// Mostra/esconde o depurador visual da RmlUi (elementos, estilos, eventos) quando há
    /// interface (World::showUi). Só F1 a F12; outra tecla desativa.
    Key uiDebuggerKey = Key::F8;
    glm::vec4 clearColor{0.08f, 0.09f, 0.11f, 1.0f};
    glm::vec3 lightDirection{-0.4f, -1.0f, -0.3f}; // para onde a luz aponta

    /// Geometria de cada id de RenderComponent::mesh. Id sem entrada vira um cubo unitário.
    std::unordered_map<std::uint32_t, PrimitiveMesh> meshes;
};

} // namespace lkz
