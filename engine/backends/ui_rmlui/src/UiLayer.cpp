#include "lkz/ui_rmlui/UiLayer.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <spdlog/spdlog.h>

namespace lkz::ui_rmlui {

namespace {

class SystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT: spdlog::error("RmlUi: {}", message); break;
        case Rml::Log::LT_WARNING: spdlog::warn("RmlUi: {}", message); break;
        case Rml::Log::LT_INFO: spdlog::info("RmlUi: {}", message); break;
        default: spdlog::debug("RmlUi: {}", message); break;
        }
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

Rml::Variant toVariant(const UiValue& value) {
    return std::visit([](const auto& v) { return Rml::Variant{v}; }, value);
}

} // namespace

struct UiLayer::Impl {
    explicit Impl(UiRenderer& target)
        : renderer{target} {}

    SystemInterface system;
    UiRenderer& renderer;
    bool initialized = false;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle model;
    std::unordered_map<std::string, Rml::Variant> values;
    std::vector<UiEvent> events;
    float designHeight = 720.0f;

    bool initialize() {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        if (!renderer.initialize() || !Rml::Initialise()) {
            spdlog::error("Interface: falha ao iniciar a RmlUi");
            return false;
        }
        context = Rml::CreateContext("lkz", {1280, 720}); // render() acerta o tamanho
        if (context == nullptr) {
            spdlog::error("Interface: falha ao criar o contexto da RmlUi");
            return false;
        }
        Rml::Debugger::Initialise(context);
        initialized = true;
        return true;
    }

    void setValue(const std::string& name, Rml::Variant value) {
        auto it = values.find(name);
        if (it == values.end()) {
            return; // nome que o documento não declarou
        }
        if (!(it->second == value)) {
            it->second = std::move(value);
            if (model) {
                model.DirtyVariable(name);
            }
        }
    }
};

UiLayer::UiLayer(UiRenderer& renderer)
    : impl_{std::make_unique<Impl>(renderer)} {}

UiLayer::~UiLayer() {
    if (impl_->initialized) {
        Rml::Shutdown(); // antes da janela fechar: libera texturas e geometria
        impl_->renderer.release();
    }
}

bool UiLayer::load(const UiDocument& document) {
    auto& state = *impl_;
    if (!state.initialized && !state.initialize()) {
        return false;
    }
    if (state.document != nullptr) {
        state.document->Close();
        state.document = nullptr;
        state.context->Update(); // o documento sai de fato aqui, antes do modelo
        state.context->RemoveDataModel("ui");
        state.model = {};
    }
    for (const auto& font : document.fonts) {
        if (!Rml::LoadFontFace(font)) {
            spdlog::warn("Interface: não foi possível carregar a fonte '{}'", font);
        }
    }

    Rml::DataModelConstructor constructor = state.context->CreateDataModel("ui");
    if (!constructor) {
        spdlog::error("Interface: falha ao criar o modelo de dados 'ui'");
        return false;
    }
    state.values.clear();
    for (const auto& [name, value] : document.values) {
        state.values[name] = toVariant(value);
    }
    for (const char* name : {"lkzFps", "lkzDrawCalls", "lkzTriangles", "lkzObjects", "lkzLodColors"}) {
        state.values.try_emplace(name, Rml::Variant{0});
    }
    for (const auto& [name, value] : state.values) {
        const std::string key = name;
        constructor.BindFunc(key, [this, key](Rml::Variant& out) { out = impl_->values[key]; });
    }
    constructor.BindEventCallback("action", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        UiEvent event;
        event.action = !args.empty() ? args[0].Get<Rml::String>() : Rml::String{};
        event.argument = args.size() > 1 ? args[1].Get<Rml::String>() : Rml::String{};
        impl_->events.push_back(std::move(event));
    });
    state.model = constructor.GetModelHandle();
    state.designHeight = std::max(document.designHeight, 1.0f);

    // A RmlUi acha a pasta do documento (para imagens e .rcss relativos) pela '/'.
    std::string path = document.path;
    std::replace(path.begin(), path.end(), '\\', '/');
    state.document = state.context->LoadDocument(path);
    if (state.document == nullptr) {
        spdlog::error("Interface: não foi possível carregar '{}'", document.path);
        return false;
    }
    state.document->Show();
    spdlog::info("Interface: '{}' carregada ({} valores, {} fontes)", document.path, document.values.size(),
                 document.fonts.size());
    return true;
}

bool UiLayer::active() const {
    return impl_->document != nullptr;
}

void UiLayer::setValues(std::span<const std::pair<std::string, UiValue>> values) {
    for (const auto& [name, value] : values) {
        impl_->setValue(name, toVariant(value));
    }
}

void UiLayer::setStats(const UiStats& stats) {
    auto& state = *impl_;
    state.setValue("lkzFps", Rml::Variant{stats.fps});
    state.setValue("lkzDrawCalls", Rml::Variant{static_cast<int>(stats.drawCalls)});
    state.setValue("lkzTriangles", Rml::Variant{static_cast<double>(stats.triangles) / 1e6});
    state.setValue("lkzObjects", Rml::Variant{static_cast<int>(stats.objects)});
    state.setValue("lkzLodColors", Rml::Variant{stats.lodColors});
}

bool UiLayer::processMouse(const UiMouse& mouse) {
    auto& state = *impl_;
    if (state.context == nullptr) {
        return false;
    }
    state.context->ProcessMouseMove(static_cast<int>(mouse.position.x), static_cast<int>(mouse.position.y), 0);
    // O fundo do documento (e a raiz) não segura o mouse: só painéis e botões.
    Rml::Element* hover = state.context->GetHoverElement();
    const bool over = hover != nullptr && hover != state.context->GetRootElement() && hover != state.document;
    for (int button = 0; button < 3; ++button) {
        if (mouse.pressed[static_cast<std::size_t>(button)]) {
            state.context->ProcessMouseButtonDown(button, 0);
        }
        if (mouse.released[static_cast<std::size_t>(button)]) {
            state.context->ProcessMouseButtonUp(button, 0);
        }
    }
    if (over && mouse.wheel != 0.0f) {
        state.context->ProcessMouseWheel(-mouse.wheel, 0); // RmlUi: positivo = rolar para baixo
    }
    return over;
}

std::vector<UiEvent> UiLayer::takeEvents() {
    std::vector<UiEvent> events;
    events.swap(impl_->events);
    return events;
}

void UiLayer::toggleDebugger() {
    if (impl_->initialized) {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
    }
}

void UiLayer::render(int width, int height) {
    auto& state = *impl_;
    if (state.context == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (state.context->GetDimensions() != Rml::Vector2i{width, height}) {
        state.context->SetDimensions({width, height});
    }
    state.context->SetDensityIndependentPixelRatio(static_cast<float>(height) / state.designHeight);
    state.context->Update();
    state.renderer.beginFrame(width, height);
    state.context->Render();
    state.renderer.endFrame();
}

} // namespace lkz::ui_rmlui
