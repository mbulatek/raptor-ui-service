#include "raptor_ui/config.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace raptor::ui {
namespace {

constexpr std::string_view kPageTypes[] = {
    "boot",
    "transport",
    "playing",
    "recording",
    "song",
    "track",
    "settings",
    "chords",
    "midi_monitor",
    "status",
};

template <typename T>
void assign_if_present(const YAML::Node& node, const char* key, T& target) {
    if (node[key]) {
        target = node[key].as<T>();
    }
}

void load_global_ipc_overrides(const std::string& path, IpcConfig& ipc) {
    if (path.empty()) {
        return;
    }

    const auto root = YAML::LoadFile(path);
    if (const auto ipc_root = root["ipc"]) {
        if (const auto ui = ipc_root["ui"]) {
            assign_if_present(ui, "events_endpoint", ipc.ui_events_endpoint);
            assign_if_present(ui, "control_endpoint", ipc.ui_control_endpoint);
        }
        if (const auto midi_io = ipc_root["midi_io"]) {
            assign_if_present(midi_io, "events_endpoint", ipc.midi_events_endpoint);
            assign_if_present(midi_io, "control_endpoint", ipc.midi_control_endpoint);
        }
        if (const auto seq = ipc_root["seq"]) {
            assign_if_present(seq, "control_endpoint", ipc.sequencer_control_endpoint);
        }
    }
}

bool is_supported_page_type(const std::string& type) {
    return std::find(std::begin(kPageTypes), std::end(kPageTypes), type) != std::end(kPageTypes);
}

bool page_supported_by_display(const PageConfig& page, const DisplayConfig& display) {
    return page.allowed_models.empty() ||
           std::find(page.allowed_models.begin(), page.allowed_models.end(), display.model) != page.allowed_models.end();
}

DisplayConfig parse_display(const YAML::Node& node) {
    DisplayConfig display;
    assign_if_present(node, "id", display.id);
    assign_if_present(node, "driver", display.driver);
    assign_if_present(node, "model", display.model);
    assign_if_present(node, "layout", display.layout);
    assign_if_present(node, "rotation", display.rotation);
    assign_if_present(node, "refresh_period_ms", display.refresh_period_ms);
    assign_if_present(node, "force_clear_each_frame", display.force_clear_each_frame);
    if (const auto hardware = node["hardware"]) {
        assign_if_present(hardware, "gpiochip", display.hardware.gpiochip);
        assign_if_present(hardware, "spi_device", display.hardware.spi_device);
        assign_if_present(hardware, "spi_channel", display.hardware.spi_channel);
        assign_if_present(hardware, "spi_speed_hz", display.hardware.spi_speed_hz);
        assign_if_present(hardware, "spi_flags", display.hardware.spi_flags);
        assign_if_present(hardware, "cs_gpio", display.hardware.cs_gpio);
        assign_if_present(hardware, "reset_gpio", display.hardware.reset_gpio);
        assign_if_present(hardware, "dc_gpio", display.hardware.dc_gpio);
    }
    return display;
}

PageConfig parse_page(const YAML::Node& node) {
    PageConfig page;
    assign_if_present(node, "id", page.id);
    assign_if_present(node, "type", page.type);
    assign_if_present(node, "title", page.title);
    assign_if_present(node, "default_variant", page.default_variant);
    if (const auto allowed_models = node["allowed_models"]) {
        if (!allowed_models.IsSequence()) {
            throw std::runtime_error("page.allowed_models must be a YAML sequence");
        }
        for (const auto& model : allowed_models) {
            page.allowed_models.push_back(model.as<std::string>());
        }
    }
    if (const auto image = node["image"]) {
        assign_if_present(image, "path", page.image.path);
        assign_if_present(image, "x", page.image.x);
        assign_if_present(image, "y", page.image.y);
    }
    return page;
}

DisplayAssignmentConfig parse_assignment(const YAML::Node& node) {
    DisplayAssignmentConfig assignment;
    assign_if_present(node, "display_id", assignment.display_id);
    assign_if_present(node, "page_id", assignment.page_id);
    return assignment;
}

SceneConfig parse_scene(const YAML::Node& node) {
    SceneConfig scene;
    assign_if_present(node, "id", scene.id);
    assign_if_present(node, "title", scene.title);
    if (const auto assignments = node["assignments"]) {
        if (!assignments.IsSequence()) {
            throw std::runtime_error("scene.assignments must be a YAML sequence");
        }
        for (const auto& assignment : assignments) {
            scene.assignments.push_back(parse_assignment(assignment));
        }
    }
    return scene;
}

void validate_display(const DisplayConfig& display, std::unordered_set<std::string>& ids) {
    if (display.id.empty()) {
        throw std::runtime_error("display.id must not be empty");
    }
    if (!ids.insert(display.id).second) {
        throw std::runtime_error("duplicate display.id: " + display.id);
    }
    if (display.driver != "waveshare") {
        throw std::runtime_error("Only display.driver = waveshare is currently supported");
    }
    if (display.model.empty()) {
        throw std::runtime_error("display.model must not be empty for display " + display.id);
    }
    if (display.layout.empty()) {
        throw std::runtime_error("display.layout must not be empty for display " + display.id);
    }
    if (display.refresh_period_ms == 0) {
        throw std::runtime_error("display.refresh_period_ms must be greater than zero for display " + display.id);
    }
    if (display.hardware.gpiochip < 0 || display.hardware.spi_device < 0 || display.hardware.spi_channel < 0) {
        throw std::runtime_error("display hardware bus values must be >= 0 for display " + display.id);
    }
    if (display.hardware.spi_speed_hz <= 0) {
        throw std::runtime_error("display.hardware.spi_speed_hz must be > 0 for display " + display.id);
    }
    if (display.hardware.cs_gpio < -1) {
        throw std::runtime_error("display.hardware.cs_gpio must be >= -1 for display " + display.id);
    }
    if (display.hardware.reset_gpio < 0 || display.hardware.dc_gpio < 0) {
        throw std::runtime_error("display reset/dc GPIO pins must be >= 0 for display " + display.id);
    }
}

void validate_page(const PageConfig& page, std::unordered_set<std::string>& ids) {
    if (page.id.empty()) {
        throw std::runtime_error("page.id must not be empty");
    }
    if (!ids.insert(page.id).second) {
        throw std::runtime_error("duplicate page.id: " + page.id);
    }
    if (!is_supported_page_type(page.type)) {
        throw std::runtime_error("unsupported page.type for page " + page.id + ": " + page.type);
    }
    if (page.title.empty()) {
        throw std::runtime_error("page.title must not be empty for page " + page.id);
    }
    if (page.default_variant.empty()) {
        throw std::runtime_error("page.default_variant must not be empty for page " + page.id);
    }
}

void validate_assignment_collection(const std::vector<DisplayAssignmentConfig>& assignments,
                                    const std::vector<DisplayConfig>& displays,
                                    const std::vector<PageConfig>& pages,
                                    const std::string& context) {
    std::unordered_set<std::string> display_ids;
    for (const auto& display : displays) {
        display_ids.insert(display.id);
    }
    std::unordered_set<std::string> page_ids;
    for (const auto& page : pages) {
        page_ids.insert(page.id);
    }

    std::unordered_set<std::string> assigned_displays;
    for (const auto& assignment : assignments) {
        if (assignment.display_id.empty() || assignment.page_id.empty()) {
            throw std::runtime_error(context + ": assignment.display_id and assignment.page_id must not be empty");
        }
        if (!display_ids.contains(assignment.display_id)) {
            throw std::runtime_error(context + ": assignment references unknown display_id: " + assignment.display_id);
        }
        if (!page_ids.contains(assignment.page_id)) {
            throw std::runtime_error(context + ": assignment references unknown page_id: " + assignment.page_id);
        }
        if (!assigned_displays.insert(assignment.display_id).second) {
            throw std::runtime_error(context + ": duplicate assignment for display_id: " + assignment.display_id);
        }

        const auto display_it = std::find_if(displays.begin(), displays.end(), [&](const auto& display) {
            return display.id == assignment.display_id;
        });
        const auto page_it = std::find_if(pages.begin(), pages.end(), [&](const auto& page) {
            return page.id == assignment.page_id;
        });
        if (display_it == displays.end() || page_it == pages.end() || !page_supported_by_display(*page_it, *display_it)) {
            throw std::runtime_error(context + ": assignment is not compatible with target display: " + assignment.display_id + " -> " + assignment.page_id);
        }
    }

    for (const auto& display : displays) {
        if (!assigned_displays.contains(display.id)) {
            throw std::runtime_error(context + ": missing page assignment for display: " + display.id);
        }
    }
}

void validate_scene(const SceneConfig& scene,
                    const std::vector<DisplayConfig>& displays,
                    const std::vector<PageConfig>& pages,
                    std::unordered_set<std::string>& ids) {
    if (scene.id.empty()) {
        throw std::runtime_error("scene.id must not be empty");
    }
    if (!ids.insert(scene.id).second) {
        throw std::runtime_error("duplicate scene.id: " + scene.id);
    }
    if (scene.title.empty()) {
        throw std::runtime_error("scene.title must not be empty for scene " + scene.id);
    }
    validate_assignment_collection(scene.assignments, displays, pages, "scene " + scene.id);
}

void validate_config(ServiceConfig& config) {
    if (config.displays.empty()) {
        throw std::runtime_error("config must define at least one display");
    }
    if (config.ipc.ui_events_endpoint.empty() ||
        config.ipc.ui_control_endpoint.empty() ||
        config.ipc.sequencer_control_endpoint.empty()) {
        throw std::runtime_error("IPC endpoints must not be empty");
    }
    if (config.logging.level.empty()) {
        throw std::runtime_error("logging.level must not be empty");
    }

    std::unordered_set<std::string> display_ids;
    for (const auto& display : config.displays) {
        validate_display(display, display_ids);
    }

    if (config.pages.empty()) {
        config.pages.push_back(PageConfig {
            .id = "status",
            .type = "status",
            .title = "Status",
            .allowed_models = {},
            .default_variant = "auto",
        });
    }

    std::unordered_set<std::string> page_ids;
    for (const auto& page : config.pages) {
        validate_page(page, page_ids);
    }

    std::unordered_set<std::string> scene_ids;
    for (const auto& scene : config.scenes) {
        validate_scene(scene, config.displays, config.pages, scene_ids);
    }

    if (!config.initial_scene.empty() && !scene_ids.contains(config.initial_scene)) {
        throw std::runtime_error("initial_scene references unknown scene: " + config.initial_scene);
    }

    if (config.assignments.empty()) {
        if (!config.initial_scene.empty()) {
            const auto scene_it = std::find_if(config.scenes.begin(), config.scenes.end(), [&](const auto& scene) {
                return scene.id == config.initial_scene;
            });
            config.assignments = scene_it->assignments;
        } else if (!config.scenes.empty()) {
            config.assignments = config.scenes.front().assignments;
            config.initial_scene = config.scenes.front().id;
        } else {
            for (const auto& display : config.displays) {
                const auto page_it = std::find_if(config.pages.begin(), config.pages.end(), [&](const auto& page) {
                    return page_supported_by_display(page, display);
                });
                if (page_it == config.pages.end()) {
                    throw std::runtime_error("no compatible page found for display: " + display.id);
                }
                config.assignments.push_back(DisplayAssignmentConfig {.display_id = display.id, .page_id = page_it->id});
            }
        }
    }

    validate_assignment_collection(config.assignments, config.displays, config.pages, "service assignments");
}

}  // namespace

ServiceConfig load_config(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("config path must not be empty");
    }

    ServiceConfig config;
    config.config_path = path;

    const auto root = YAML::LoadFile(path);
    assign_if_present(root, "ipc_config_path", config.ipc_config_path);

    if (const auto displays = root["displays"]) {
        if (!displays.IsSequence()) {
            throw std::runtime_error("displays must be a YAML sequence");
        }
        for (const auto& node : displays) {
            config.displays.push_back(parse_display(node));
        }
    } else if (const auto display = root["display"]) {
        config.displays.push_back(parse_display(display));
    }

    if (const auto pages = root["pages"]) {
        if (!pages.IsSequence()) {
            throw std::runtime_error("pages must be a YAML sequence");
        }
        for (const auto& node : pages) {
            config.pages.push_back(parse_page(node));
        }
    }

    if (const auto assignments = root["assignments"]) {
        if (!assignments.IsSequence()) {
            throw std::runtime_error("assignments must be a YAML sequence");
        }
        for (const auto& node : assignments) {
            config.assignments.push_back(parse_assignment(node));
        }
    }

    if (const auto scenes = root["scenes"]) {
        if (!scenes.IsSequence()) {
            throw std::runtime_error("scenes must be a YAML sequence");
        }
        for (const auto& node : scenes) {
            config.scenes.push_back(parse_scene(node));
        }
    }

    assign_if_present(root, "initial_scene", config.initial_scene);

    load_global_ipc_overrides(config.ipc_config_path, config.ipc);

    if (const auto ipc = root["ipc"]) {
        assign_if_present(ipc, "ui_events_endpoint", config.ipc.ui_events_endpoint);
        assign_if_present(ipc, "ui_control_endpoint", config.ipc.ui_control_endpoint);
        assign_if_present(ipc, "midi_events_endpoint", config.ipc.midi_events_endpoint);
        assign_if_present(ipc, "midi_control_endpoint", config.ipc.midi_control_endpoint);
        assign_if_present(ipc, "sequencer_control_endpoint", config.ipc.sequencer_control_endpoint);
    }

    if (const auto logging = root["logging"]) {
        assign_if_present(logging, "level", config.logging.level);
    }

    validate_config(config);
    return config;
}

}  // namespace raptor::ui
