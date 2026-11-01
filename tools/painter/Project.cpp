#include "Project.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace painter {

namespace {

std::filesystem::path pathFrom(const std::filesystem::path& base, const std::string& text) {
    const std::filesystem::path relative{std::u8string{text.begin(), text.end()}};
    return (base / relative).lexically_normal();
}

/// `path` dentro de `folder` (ou igual a ela)?
bool inside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const auto relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

std::optional<Project> Project::load(const std::filesystem::path& file, std::string& error) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        error = "não abriu " + file.string();
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(stream, nullptr, false, true);
    if (json.is_discarded() || !json.is_object()) {
        error = file.string() + " não é um JSON válido";
        return std::nullopt;
    }
    Project project;
    project.file = std::filesystem::absolute(file);
    const auto base = project.file.parent_path();
    project.name = json.value("nome", base.filename().string());
    if (const auto roots = json.find("abrir"); roots != json.end() && roots->is_array()) {
        for (const auto& root : *roots) {
            if (root.is_string()) {
                project.roots.push_back(pathFrom(base, root.get<std::string>()));
            }
        }
    }
    project.output = pathFrom(base, json.value("saida", std::string{"."}));
    project.work = pathFrom(base, json.value("projetos", std::string{"_projetos"}));
    if (const auto palette = json.find("paleta"); palette != json.end() && palette->is_string()) {
        project.palette = pathFrom(base, palette->get<std::string>());
    }
    if (const auto decals = json.find("decalques"); decals != json.end() && decals->is_string()) {
        project.decals = pathFrom(base, decals->get<std::string>());
    }
    if (project.roots.empty()) {
        project.roots.push_back(project.output);
    }
    return project;
}

std::vector<std::string> Project::outputFolders() const {
    std::vector<std::string> folders;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{output, ec}) {
        const auto name = entry.path().filename().u8string();
        if (entry.is_directory(ec) && !name.empty() && name.front() != u8'_') {
            folders.emplace_back(name.begin(), name.end());
        }
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

std::optional<std::string> Project::folderOf(const std::filesystem::path& model) const {
    if (!inside(model, output)) {
        return std::nullopt;
    }
    const auto relative = model.lexically_normal().lexically_relative(output.lexically_normal());
    // Só a primeira subpasta, e nunca uma de trabalho.
    const auto first = relative.begin()->u8string();
    if (std::distance(relative.begin(), relative.end()) < 2 || first.empty() || first.front() == u8'_') {
        return std::nullopt;
    }
    return std::string{first.begin(), first.end()};
}

} // namespace painter
