#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <lkz/Asset/ModelData.hpp>
#include <lkz/Asset/Raycast.hpp>
#include <lkz/Asset/TexelMap.hpp>
#include <lkz/Asset/Unwrap.hpp>
#include <lkz/Render/MeshData.hpp>
#include <lkz/Render/TextureData.hpp>

namespace painter {

class Palette;

/// Pincel redondo no espaço 3D: pinta os texels cuja posição na superfície fica a até
/// `radius` metros do centro, virados para a câmera (não atravessa paredes finas).
struct Brush {
    glm::vec3 color{0.75f, 0.22f, 0.17f}; // 0..1
    int material = -1;                    // da paleta: pinta a imagem dele em escala real (-1 = a cor)
    bool tint = false;                    // com material: a imagem dele multiplicada pela cor
    float radius = 0.1f;                  // metros
    float hardness = 0.6f;                // 0 = borda toda suave, 1 = disco duro
    float opacity = 1.0f;
    bool erase = false;                   // volta à cor que o modelo tinha ao abrir
};

/// Como o balde escolhe as faces a partir da que está sob o mouse.
enum class Selection : std::uint8_t {
    Triangle, // só ela
    Wall,     // as do mesmo plano ligadas a ela por arestas
    Plane,    // todas as do mesmo plano
    Material, // todas as do mesmo material do modelo
};

/// O que o balde faz nas faces escolhidas.
struct Fill {
    lkz::asset::SurfaceTarget target = lkz::asset::SurfaceTarget::Shared;
    int material = -1;        // da paleta (-1 = a cor)
    glm::vec3 color{1.0f};
    bool tint = false;        // com material: a imagem dele multiplicada pela cor
    bool restore = false;     // volta como estava ao abrir (destino e pintura)
};

/// Destino de uma face e o que ela mostra:
///  - Shared: um material da paleta (`material`), repetido em metros; com `colored`, a
///    imagem dele multiplicada por `color` (tingida: a textura dá os tons, a cor dá a cor);
///  - Color: uma cor lisa (`color`);
///  - Decal: a imagem de um decalque (`material` = índice em Document::decalCount()), no UV
///    da própria face (o retângulo do carimbo);
///  - Unique: a tela do material dela. Sem tela (material sem UV), mostra o material da
///    paleta ou a cor que o balde deu, ou a cor do material; o UV único assa isso numa tela.
struct Face {
    lkz::asset::SurfaceTarget target = lkz::asset::SurfaceTarget::Unique;
    int material = -1;    // da paleta, ou o decalque (-1 = nenhum)
    bool colored = false; // `color` vale (num material da paleta, é a cor que o tinge)
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    bool operator==(const Face&) const = default;
};

/// Como a cor muda a imagem de um carimbo.
enum class DecalTint : std::uint8_t {
    None,      // a imagem como é
    Multiply,  // a imagem toda multiplicada pela cor (no .glb, a cor do material: a imagem continua uma só)
    WhiteOnly, // só as partes brancas e cinzas (sem saturação) recebem a cor; ferrugem, vidro e
               // maçaneta ficam como são. Gera uma imagem por cor (embutida no .glb)
};

/// Um carimbo: um retângulo no plano de uma face, nos eixos dela (surfaceAxes, em metros),
/// com a imagem de um decalque girada em quartos de volta.
struct DecalPlacement {
    int decal = -1;           // na biblioteca de decalques
    int face = -1;            // face clicada: o carimbo vale para o plano dela
    glm::vec2 origin{0.0f};   // canto de menor (u, v): em cima, à esquerda de quem olha
    glm::vec2 size{1.0f};     // largura (u) e altura (v), em metros
    int quarterTurns = 0;     // 0..3, no sentido horário: 1 = o alto da imagem vai para a direita
    DecalTint tintMode = DecalTint::None;
    glm::vec3 tint{1.0f};     // a cor (com tintMode)
    float depth = 0.0f;       // metros ao longo da normal: negativo afunda (porta), positivo salta (peitoril)
    /// Mesmo carimbo (sem olhar a face clicada).
    [[nodiscard]] bool sameAs(const DecalPlacement& other) const {
        return decal == other.decal && origin == other.origin && size == other.size &&
               quarterTurns == other.quarterTurns && tintMode == other.tintMode && tint == other.tint &&
               depth == other.depth;
    }
};

/// Peso da cor num pixel para DecalTint::WhiteOnly: 1 no branco e no cinza, 0 nas cores
/// saturadas, com uma passagem suave entre os dois (sem serrilhado na borda da ferrugem).
[[nodiscard]] float neutralWeight(const glm::vec3& rgb);
/// A cor de um pixel do decalque com a tinta do carimbo.
[[nodiscard]] glm::vec3 tintPixel(const glm::vec3& rgb, DecalTint mode, const glm::vec3& tint);
/// A imagem inteira com a tinta (alfa como está).
[[nodiscard]] lkz::asset::ImageData tintedImage(const lkz::asset::ImageData& image, DecalTint mode, const glm::vec3& tint);

/// Como o Esculpir muda as faces selecionadas.
enum class SculptMode : std::uint8_t {
    Extrude, // Puxar: a seleção anda e ganha paredes nas bordas; o resto do modelo fica
    Move,    // Mover: a seleção anda e as faces vizinhas esticam junto (deforma)
};

/// Mudança numa tela, para World::updateTexture.
struct CanvasPatch {
    int material = -1;
    lkz::TexturePatch patch;
};

/// Modelo aberto no Pintor. Cada material com UV ganha uma "tela": a imagem dele (ou a
/// cor, numa imagem nova) com a cor base já aplicada, que o pincel muda. Cada face tem um
/// destino (Face). A exportação separa as faces pelo destino: as únicas ficam no UV
/// original com as telas como imagens; as compartilhadas apontam para a imagem da paleta;
/// as de cor lisa usam uma imagem pequena de amostras.
class Document {
public:
    static constexpr int blockSize = 64;                          // desfazer guarda blocos deste lado
    static constexpr std::size_t undoBudget = 512u * 1024u * 1024u; // bytes guardados para desfazer
    static constexpr float texelsPerMeter = 32.0f;                 // tela nova de um material só cor
    static constexpr int swatchSize = 4;                           // lado de cada amostra de cor (px)

    /// A paleta dos materiais compartilhados e a biblioteca de decalques (precisam viver
    /// mais que o documento). Valem para o próximo open.
    void setPalette(const Palette* palette) { palette_ = palette; }
    void setDecals(const Palette* decals) { decalLibrary_ = decals; }

    bool open(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] bool isOpen() const { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const lkz::asset::ModelData& model() const { return model_; }
    [[nodiscard]] const lkz::asset::MeshRaycaster& raycaster() const { return raycaster_; }
    /// Uma linha para a tela: triângulos, materiais, telas e avisos.
    [[nodiscard]] const std::string& summary() const { return summary_; }
    /// A maior fração de texels pintados por mais de uma face, entre as telas (0..1).
    [[nodiscard]] float uvOverlap() const { return overlap_; }
    /// UV sobreposto ou material sem UV: o UV único resolve.
    [[nodiscard]] bool needsUniqueUv() const { return needsUniqueUv_; }
    /// Sobreposição (0..1) da tela onde a face pinta: acima de 0, pintar a face pinta
    /// outras junto.
    [[nodiscard]] float faceOverlap(int face) const;

    // --- Faces ---
    [[nodiscard]] int faceCount() const { return static_cast<int>(faces_.size()); }
    [[nodiscard]] int faceIndex(const lkz::asset::TriangleRef& ref) const;
    [[nodiscard]] const Face& face(int index) const { return faces_[static_cast<std::size_t>(index)]; }
    /// Faces que o balde pegaria a partir da face atingida.
    [[nodiscard]] std::vector<int> select(const lkz::asset::RayHit& hit, Selection mode) const;
    /// Área das faces, em m².
    [[nodiscard]] float area(const std::vector<int>& faces) const;
    /// Aplica o balde (um passo de desfazer). Devolve em quantas faces foi aplicado.
    /// `withoutCanvas` conta as que pediram Único sem ter tela (material sem UV): elas
    /// ficam com a aparência pedida até o UV único assá-la.
    int fill(const std::vector<int>& faces, const Fill& fill, int* withoutCanvas = nullptr);
    /// Os destinos ou a malha mudaram (balde, carimbo, desfazer, escala da paleta): refazer
    /// as superfícies.
    bool takeGeometryChanged();
    /// A escala de um material da paleta mudou.
    void paletteChanged() { geometryChanged_ = true; }

    // --- Decalques ---
    [[nodiscard]] glm::vec3 faceNormal(int face) const { return faceNormal_[static_cast<std::size_t>(face)]; }
    [[nodiscard]] glm::vec3 faceCenter(int face) const { return faceCenter_[static_cast<std::size_t>(face)]; }
    /// Carimba: as faces do plano da face clicada são cortadas no retângulo, e os pedaços de
    /// dentro mostram o decalque (UV = o retângulo). Com transparência, o decalque é composto
    /// sobre o que a parede mostrava, numa imagem só desse carimbo. Com cor, a imagem é
    /// tingida (DecalTint). Com profundidade, os pedaços de dentro andam ao longo da normal e
    /// ganham as paredes do vão (com o destino da parede de onde saíram). Um passo de desfazer.
    /// Devolve quantos triângulos de decalque foram criados (0 = o retângulo não pegou nada).
    int stamp(const DecalPlacement& placement);
    /// Retângulo do carimbo como malha (4 vértices), afastado `lift` metros da face, para a
    /// prévia na tela. UV = a imagem do decalque já girada.
    [[nodiscard]] lkz::MeshData decalQuad(const DecalPlacement& placement, float lift) const;
    /// Imagens de decalque em uso (as da biblioteca e as compostas de cada carimbo com
    /// transparência). Face::material de uma face Decal aponta para cá.
    [[nodiscard]] int decalCount() const { return static_cast<int>(decals_.size()); }
    [[nodiscard]] const lkz::asset::ImageData& decalImage(int decal) const;
    /// Decalque da biblioteca no ponto atingido (-1 = nenhum).
    [[nodiscard]] int decalAt(const lkz::asset::RayHit& hit) const;

    // --- Ajustar um carimbo já posto (os desta sessão; os de um arquivo reaberto ou de
    // antes do UV único ficam fixos) ---
    /// O carimbo que a face mostra, com `face` = ela.
    [[nodiscard]] std::optional<DecalPlacement> stampAt(int face) const;
    /// As faces do carimbo que a face mostra (vazio = não é um carimbo ajustável).
    [[nodiscard]] std::vector<int> stampFaces(int face) const;
    /// Id do carimbo ajustável que a face mostra: não muda enquanto ele existe, nem com o
    /// desfazer (-1 = nenhum).
    [[nodiscard]] int stampId(int face) const;
    /// O carimbo pelo id, com `face` = uma face dele (nada se ele não existe mais).
    [[nodiscard]] std::optional<DecalPlacement> stampById(int id) const;
    [[nodiscard]] std::vector<int> stampFacesById(int id) const;
    /// Id do carimbo que o último stamp() criou.
    [[nodiscard]] int lastStampId() const { return lastStampId_; }
    /// Troca carimbos postos por outros (cor, profundidade, giro, tamanho, no mesmo plano), ou
    /// os apaga (`decal` < 0), num passo só de desfazer. Devolve o id novo de cada um (-1 =
    /// apagado, ou o retângulo novo não pegou nada).
    std::vector<int> replaceStamps(const std::vector<std::pair<int, DecalPlacement>>& changes);
    /// Tira da parede o carimbo que a face mostra: o que ele cobria volta (com o destino atual
    /// do resto daquela face). Um passo de desfazer, que o próximo stamp() completa: mover é
    /// lift + stamp. Devolve o carimbo, com `face` = uma face do plano dele.
    std::optional<DecalPlacement> lift(int face);
    [[nodiscard]] bool lifted() const { return liftOpen_; }
    /// Desfaz o lift: o carimbo volta como estava.
    void cancelLift();
    /// Encerra o lift sem carimbar de novo: o carimbo fica apagado.
    void endLift() { liftOpen_ = false; }

    /// O que mostrar: as faces agrupadas pelo que elas mostram. `highlight` (índices de
    /// faces) vira superfícies à parte, para o realce.
    struct Surface {
        enum class Kind : std::uint8_t { Material, Shared, Color, Decal };
        Kind kind = Kind::Material;
        int index = 0; // material do modelo, material da paleta, 0 (cores) ou decalque
        bool doubleSided = false;
        bool highlighted = false;
        lkz::MeshData mesh;
        glm::vec4 color{1.0f}; // cor da instância: branca com imagem (a cor está nela)
    };
    [[nodiscard]] std::vector<Surface> surfaces(const std::vector<int>& highlight = {}) const;
    [[nodiscard]] bool hasCanvas(int material) const;
    /// A tela do material, como imagem (vazia se não tem).
    [[nodiscard]] lkz::TextureImage canvasImage(int material) const;
    /// As amostras das faces de cor lisa.
    [[nodiscard]] lkz::TextureImage swatchImage() const;

    void beginStroke();
    /// Uma pincelada centrada em `center` (na superfície), vista de `eye`. Só nas faces
    /// únicas com tela.
    void dab(const glm::vec3& center, const glm::vec3& eye, const Brush& brush);
    void endStroke();
    [[nodiscard]] bool stroking() const { return stroking_; }
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const { return !undo_.empty() && !stroking_; }
    [[nodiscard]] bool canRedo() const { return !redo_.empty() && !stroking_; }
    [[nodiscard]] bool modified() const { return revision_ != savedRevision_; }

    // --- Esculpir ---
    /// Corta as faces do plano da face no retângulo [lo, hi] (eixos surfaceAxes, em metros),
    /// sem mudar a aparência, e devolve as faces de dentro. Um passo de desfazer.
    std::vector<int> cutRectangle(int face, const glm::vec2& lo, const glm::vec2& hi);
    /// Começa a esculpir as faces: elas andam `direction` (unitária) vezes a distância de
    /// updateSculpt. Antes, divide as arestas com vértices em T na região (senão abririam
    /// frestas ou paredes falsas).
    bool beginSculpt(const std::vector<int>& faces, SculptMode mode, const glm::vec3& direction);
    /// Refaz a escultura com a distância nova (metros), a partir do começo.
    void updateSculpt(float distance);
    /// Confirma: um passo de desfazer (distância 0 = nada muda). Devolve as faces esculpidas.
    std::vector<int> endSculpt();
    /// Desiste: o modelo volta como antes do beginSculpt.
    void cancelSculpt();
    [[nodiscard]] bool sculpting() const { return sculpt_ != nullptr; }
    /// As faces sendo esculpidas, na numeração atual (vazio sem escultura).
    [[nodiscard]] std::vector<int> sculptSelection() const;
    [[nodiscard]] float sculptDistance() const;
    /// Muda a cada mudança na malha (índices das faces mudam junto).
    [[nodiscard]] std::uint64_t geometryVersion() const { return geometryVersion_; }

    /// Cor da superfície no ponto atingido (conta-gotas).
    [[nodiscard]] std::optional<glm::vec3> colorAt(const lkz::asset::RayHit& hit) const;
    /// Material da paleta no ponto atingido (-1 = nenhum).
    [[nodiscard]] int paletteAt(const lkz::asset::RayHit& hit) const;
    /// Cor que tinge o material da paleta no ponto atingido (nenhuma = sem tinta).
    [[nodiscard]] std::optional<glm::vec3> tintAt(const lkz::asset::RayHit& hit) const;
    /// O que mudou nas telas desde a última chamada.
    void takePatches(std::vector<CanvasPatch>& out);

    /// Grava o .glb e relê o arquivo para conferir. Os materiais da paleta ficam como
    /// imagens externas, com o caminho relativo ao .glb. `report` diz o que foi gravado,
    /// ou o erro.
    bool exportGlb(const std::filesystem::path& path, std::string& report);

    /// Troca o UV das faces únicas por um único, sem sobreposição (lkz::asset::bakeUnique),
    /// com uma tela por grupo de materiais e a pintura atual assada nela. As faces
    /// compartilhadas e de cor lisa ficam como estão. Apaga o desfazer.
    bool makeUniqueUv(const lkz::asset::UnwrapOptions& options, std::string& report);

private:
    struct Canvas {
        int material = -1;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;      // RGBA: a pintura atual
        std::vector<std::uint8_t> strokeAlpha; // cobertura do traço atual por texel (0..255)
        std::vector<int> strokeBlock;          // bloco -> posição em Stroke::blocks (-1 = intocado)
        int blocksX = 0;
        int blocksY = 0;
        std::vector<lkz::asset::TexelRect> dirty;  // mudado desde o último takePatches
        std::vector<lkz::asset::TexelRect> finish; // fim de traço ou desfazer: reenviar com mipmaps
        const lkz::asset::ImageData* source = nullptr; // como era ao abrir: source x tint
        glm::vec4 tint{1.0f};
        float overlap = 0.0f; // fração dos texels pintados por mais de uma face
    };
    struct SavedBlock {
        int canvas = -1;
        int block = -1;
        std::vector<std::uint8_t> pixels; // blockSize x blockSize x 4 (a parte fora da tela fica 0)
    };
    struct SavedFace {
        int face = -1;
        Face state; // o outro estado (antes, para desfazer; depois, para refazer)
    };
    /// O que um triângulo de carimbo cobria, para descarimbar.
    struct Origin {
        int part = -1;   // partId_ da parte de onde veio
        int family = -1; // family_ da face de onde veio
        Face face;
        Face initial;
        std::array<glm::vec2, 3> uv{}; // o UV que ele tinha lá, por vértice
        std::shared_ptr<const Origin> under; // se lá também era um carimbo: o que aquele cobria
    };
    /// Uma parte criada por um carimbo: a do decalque, ou uma das paredes do vão dele.
    struct StampRecord {
        DecalPlacement placement;
        std::vector<Origin> origins; // por triângulo da parte (as paredes do vão não voltam a lugar nenhum)
        glm::vec3 offset{0.0f};      // o quanto os triângulos do decalque andaram (profundidade)
        int owner = -1;              // parede do vão: partId_ da parte do decalque dela (sai junto)
    };
    using StampRef = std::shared_ptr<const StampRecord>;

    /// Malha e destinos inteiros (o carimbo corta triângulos e cria partes e materiais).
    struct Geometry {
        std::vector<lkz::asset::ModelPart> parts;
        std::vector<lkz::asset::MaterialData> materials;
        std::vector<int> canvasOf;
        std::vector<int> decalOf;
        std::vector<Face> faces;
        std::vector<Face> initialFaces;
        std::vector<int> partId;
        std::vector<StampRef> stampOf;
        std::vector<int> family;
        [[nodiscard]] std::size_t bytes() const;
    };
    struct Stroke {
        std::vector<SavedBlock> blocks;
        std::vector<SavedFace> faces;
        std::shared_ptr<Geometry> geometry; // a outra versão da malha (carimbo)
        [[nodiscard]] std::size_t bytes() const {
            return blocks.size() * blockSize * blockSize * 4 + faces.size() * sizeof(SavedFace) +
                   (geometry ? geometry->bytes() : 0);
        }
    };
    /// Uma imagem de decalque: a da biblioteca (dividida por todos os carimbos dela) ou a
    /// composta de um carimbo com transparência.
    struct DecalEntry {
        int library = -1;                 // índice na biblioteca (-1 = só a composta)
        std::string id;                   // nome do arquivo na biblioteca
        lkz::asset::ImageData composite;  // vazia = a imagem da biblioteca
        glm::vec3 whiteOnly{-1.0f};       // a imagem da biblioteca tingida só no branco com esta cor
    };

    /// O que uma face mostra.
    enum class Look : std::uint8_t { Canvas, Original, Palette, Color, Decal };
    [[nodiscard]] Look lookOf(int face) const;

    enum class Compose : std::uint8_t {
        Export, // materiais da paleta como imagens externas (caminho relativo ao arquivo)
        Bake,   // para o UV único: faces únicas sem tela entram com a aparência delas
    };

    /// Novo modelo: normais, material padrão, destinos, telas, vizinhança, resumo.
    void setModel(lkz::asset::ModelData model);
    /// Índices, normais, centros, áreas e vizinhança das faces (não mexe nos destinos).
    void buildFaceGeometry();
    void swapGeometry(Geometry& other);
    /// Põe uma cópia da geometria guardada no documento (refaz faces e o raycaster).
    void restoreGeometry(const Geometry& geometry);
    /// Esculpir em andamento: o antes (para desfazer) e a malha preparada (vértices em T já
    /// divididos), de onde cada updateSculpt recomeça.
    struct Sculpt {
        std::shared_ptr<Geometry> before;
        std::shared_ptr<Geometry> prepared;
        std::vector<char> selected; // por face, na numeração da preparada
        SculptMode mode = SculptMode::Extrude;
        glm::vec3 direction{0.0f, 0.0f, 1.0f};
        float distance = 0.0f;
    };
    /// Divide os triângulos da região das faces marcadas que têm um vértice de outro
    /// triângulo no meio de uma aresta (vértice em T). As marcas seguem os pedaços.
    void splitTJunctions(std::vector<char>& selected);
    /// A escultura de sculpt_ sobre a malha preparada.
    void applySculpt();
    [[nodiscard]] std::shared_ptr<Geometry> snapshot() const;
    [[nodiscard]] static Origin reshaped(const Origin& origin, const std::array<glm::vec3, 3>& b);
    /// Devolve os triângulos do carimbo da parte às partes de onde vieram e apaga a parte.
    /// Devolve uma das faces devolvidas (na numeração nova), ou -1.
    int removeStamp(std::size_t part);
    /// extras do modelo com a lista dos carimbos ajustáveis (decalque, plano, retângulo, giro,
    /// cor, profundidade), para reabrir.
    [[nodiscard]] std::string extrasWithStamps() const;
    /// Ao abrir: os carimbos da lista de extras voltam a ser partes ajustáveis. Os triângulos
    /// de decalque no retângulo de cada um (e as paredes do vão) saem para partes próprias, e
    /// o que fica embaixo é a parede em volta (o destino dela, com o UV estendido). Os grupos
    /// de decalque fora da lista (um arquivo de antes dela) voltam pelo UV, que diz o
    /// retângulo e o giro. Devolve quantos voltaram.
    int restoreStamps();
    /// A parte do carimbo de id `id` (-1 = não existe).
    [[nodiscard]] int partOfStamp(int id) const;
    /// Entrada de decalque para um da biblioteca (a mesma para todos os carimbos dele).
    int libraryDecal(int library);
    /// Entrada da imagem da biblioteca tingida só no branco (uma por decalque e cor).
    int whiteOnlyDecal(int library, const glm::vec3& tint);
    /// Cor que a face mostra no ponto (com as baricêntricas no triângulo dela).
    [[nodiscard]] glm::vec4 lookColor(int face, const glm::vec3& position, const glm::vec3& barycentric) const;
    /// O modelo separado por destino: as faces únicas por parte, as compartilhadas por
    /// material da paleta, as de cor lisa num material de amostras.
    [[nodiscard]] lkz::asset::ModelData composed(Compose mode, const std::filesystem::path& output,
                                                 std::string* notes = nullptr) const;
    /// Cores distintas das faces que mostram cor lisa, na ordem das amostras.
    [[nodiscard]] std::vector<std::array<std::uint8_t, 4>> swatchColors() const;
    [[nodiscard]] int canvasOfFace(int face) const;
    [[nodiscard]] int materialOfFace(int face) const;
    /// Cor que o pincel ou o balde põe no texel em `position` de uma face. Com `tint`, a
    /// imagem do material multiplicada por `color`.
    [[nodiscard]] std::array<std::uint8_t, 4> sourceColor(int paletteMaterial, const glm::vec3& color,
                                                          const glm::vec3& position, int face,
                                                          float texelsPerMeter, bool tint = false) const;
    void paint(int canvasIndex, int x, int y, float alpha, const std::array<std::uint8_t, 4>& target, bool keepAlpha);
    /// Todos os texels de uma face na tela dela, com a cor de `colorAt(x, y, posição,
    /// texels por metro)`.
    template <class ColorAt>
    void paintFace(int face, bool keepAlpha, ColorAt&& colorAt);
    SavedBlock& saveBlock(int canvasIndex, int block);
    void original(const Canvas& canvas, int x, int y, std::uint8_t* rgba) const;
    void swapStroke(Stroke& stroke);
    /// Guarda um passo de desfazer (apaga o refazer, respeita o orçamento).
    void pushUndo(Stroke stroke);
    [[nodiscard]] lkz::asset::TexelRect blockRect(const Canvas& canvas, int block) const;
    [[nodiscard]] bool coplanar(int seed, int face) const;

    const Palette* palette_ = nullptr;
    const Palette* decalLibrary_ = nullptr;
    std::vector<DecalEntry> decals_;   // só cresce (o desfazer não apaga entradas)
    std::vector<int> decalOf_;         // material -> decalque (-1 = não é material de decalque)
    std::filesystem::path path_;
    lkz::asset::ModelData model_;
    lkz::asset::MeshRaycaster raycaster_;
    std::vector<Canvas> canvases_;
    std::vector<int> canvasOf_; // material -> tela (-1 = sem)
    std::string summary_;
    float overlap_ = 0.0f;
    bool needsUniqueUv_ = false;

    // Faces: índice plano = firstFace_[parte] + triângulo.
    std::vector<Face> faces_;
    std::vector<Face> initialFaces_;
    std::vector<int> firstFace_;
    std::vector<lkz::asset::TriangleRef> faceRef_;
    std::vector<glm::vec3> faceNormal_;
    std::vector<glm::vec3> faceCenter_;
    std::vector<float> faceArea_;
    std::vector<std::vector<int>> neighbors_; // faces que dividem uma aresta (vértices soldados)
    // Por face: o triângulo do arquivo de onde ela veio (os cortes do carimbo mantêm).
    std::vector<int> family_;
    // Por parte: id que não muda quando outra parte sai, e o carimbo que a criou (ou nulo).
    std::vector<int> partId_;
    std::vector<StampRef> stampOf_;
    int nextPartId_ = 0;
    bool liftOpen_ = false; // lift() feito, esperando o stamp() que o completa
    int lastStampId_ = -1;
    std::shared_ptr<Sculpt> sculpt_;
    std::uint64_t geometryVersion_ = 0;
    bool geometryChanged_ = false;

    bool stroking_ = false;
    Stroke current_;
    std::vector<Stroke> undo_;
    std::vector<Stroke> redo_;
    std::uint64_t revision_ = 0;
    std::uint64_t savedRevision_ = 0;
    std::vector<lkz::asset::TriangleRef> near_; // reaproveitado entre pinceladas
    lkz::asset::TexelRect touched_{};           // texels mudados no triângulo atual (paint)
};

} // namespace painter
