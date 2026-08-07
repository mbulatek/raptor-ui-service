#include "raptor_ui/control_server.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <zmq.h>

namespace raptor::ui {
namespace {

using json = nlohmann::json;
constexpr char kSchemaVersion[] = "1.0";

std::filesystem::path endpoint_directory(const std::string& endpoint) {
    constexpr std::string_view prefix {"ipc://"};
    if (!endpoint.starts_with(prefix)) {
        return {};
    }
    return std::filesystem::path {endpoint.substr(prefix.size())}.parent_path();
}

std::uint64_t monotonic_time_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

json midi_json(const MidiEventSummary& midi) {
    return {
        {"available", midi.available},
        {"module_id", midi.module_id},
        {"global_port", midi.global_port},
        {"bytes_hex", midi.bytes_hex},
        {"sequence", midi.sequence},
        {"timestamp_ns", midi.timestamp_ns},
    };
}

json controller_lanes_json(const std::vector<SequencerControllerLaneSummary>& lanes) {
    json out = json::array();
    for (const auto& lane : lanes) {
        out.push_back({
            {"key", lane.key},
            {"label", lane.label},
            {"muted", lane.muted},
            {"event_count", lane.event_count},
        });
    }
    return out;
}

json ui_confirmation_json(const UiConfirmationSummary& confirmation) {
    return {
        {"active", confirmation.active},
        {"kind", confirmation.kind},
        {"title", confirmation.title},
        {"message", confirmation.message},
        {"confirm_label", confirmation.confirm_label},
        {"cancel_label", confirmation.cancel_label},
        {"confirm_selected", confirmation.confirm_selected},
    };
}

json presentation_json(const PresentationSummary& presentation) {
    return {
        {"view", presentation.view},
        {"page", presentation.page},
        {"page_index", presentation.page_index},
        {"page_count", presentation.page_count},
        {"focus_index", presentation.focus_index},
        {"editing", presentation.editing},
        {"confirmation", ui_confirmation_json(presentation.confirmation)},
    };
}

json sequencer_json(const UpstreamStatus& status) {
    json j = {
        {"reachable", status.reachable},
        {"service", status.service},
        {"summary", status.summary},
        {"timestamp_ns", status.timestamp_ns},
    };
    if (!status.transport.empty()) {
        j["transport"] = status.transport;
    }
    j["presentation"] = presentation_json(status.presentation);
    j["chord_pad_right_hand_octave"] = status.chord_pad_right_hand_octave;
    if (!status.recording_quantize.empty()) {
        j["recording_quantize"] = status.recording_quantize;
    }
    if (!status.loop_quantize.empty()) {
        j["loop_quantize"] = status.loop_quantize;
    }
    if (status.song.available) {
        json tracks = json::array();
        for (const auto& track : status.song.tracks) {
            tracks.push_back({
                {"id", track.id},
                {"name", track.name},
                {"muted", track.muted},
                {"midi_in", track.midi_in},
                {"midi_out", track.midi_out},
                {"midi_in_label", track.midi_in_label},
                {"midi_out_label", track.midi_out_label},
                {"midi_channel_in", track.midi_channel_in},
                {"midi_channel_out", track.midi_channel_out},
                {"send_sync_enabled", track.send_sync_enabled},
                {"controller_lanes", controller_lanes_json(track.controller_lanes)},
            });
        }
        j["song"] = {
            {"available", true},
            {"id", status.song.id},
            {"title", status.song.title},
            {"tracks", std::move(tracks)},
        };
    }
    return j;
}

json ui_json(const UiSnapshot& ui) {
    return {
        {"display_id", ui.display_id},
        {"display_driver", ui.display_driver},
        {"display_model", ui.display_model},
        {"layout", ui.layout},
        {"page_id", ui.page_id},
        {"page_type", ui.page_type},
        {"page_title", ui.page_title},
        {"page_variant", ui.page_variant},
        {"page_image_path", ui.page_image_path},
        {"page_image_x", ui.page_image_x},
        {"page_image_y", ui.page_image_y},
        {"view_id", ui.view_id},
        {"view_page_index", ui.view_page_index},
        {"view_page_count", ui.view_page_count},
        {"display_width", ui.display_width},
        {"display_height", ui.display_height},
        {"render_count", ui.render_count},
        {"last_midi", midi_json(ui.last_midi)},
        {"sequencer", sequencer_json(ui.sequencer)},
    };
}

json page_json(const PageConfig& page) {
    return {
        {"id", page.id},
        {"type", page.type},
        {"title", page.title},
        {"allowed_models", page.allowed_models},
        {"default_variant", page.default_variant},
        {"image", json{{"path", page.image.path}, {"x", page.image.x}, {"y", page.image.y}}},
    };
}

json assignment_json(const DisplayAssignmentConfig& assignment) {
    return {
        {"display_id", assignment.display_id},
        {"page_id", assignment.page_id},
    };
}

json scene_json(const SceneConfig& scene) {
    json assignments = json::array();
    for (const auto& assignment : scene.assignments) {
        assignments.push_back(assignment_json(assignment));
    }
    return {
        {"id", scene.id},
        {"title", scene.title},
        {"assignments", std::move(assignments)},
    };
}

json envelope(const std::string& service, const std::string& command, const std::string& request_id, bool ok) {
    return {
        {"schema_version", kSchemaVersion},
        {"service", service},
        {"timestamp_ns", monotonic_time_ns()},
        {"ok", ok},
        {"command", command},
        {"request_id", request_id.empty() ? json(nullptr) : json(request_id)},
    };
}

}  // namespace

struct ControlServer::Impl {
    void* context {nullptr};
    void* router {nullptr};
};

ControlServer::ControlServer(std::string endpoint, const ServiceConfig& config, std::shared_ptr<PageController> page_controller)
    : endpoint_(std::move(endpoint)), config_(&config), page_controller_(std::move(page_controller)), impl_(std::make_unique<Impl>()) {
    snapshot_.ui_events_endpoint = config.ipc.ui_events_endpoint;
    snapshot_.ui_control_endpoint = config.ipc.ui_control_endpoint;

    const auto directory = endpoint_directory(endpoint_);
    if (!directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
    }
    impl_->context = zmq_ctx_new();
    if (impl_->context == nullptr) {
        spdlog::error("ui control server init failed endpoint={} step=zmq_ctx_new err={}", endpoint_, zmq_strerror(zmq_errno()));
        return;
    }
    impl_->router = zmq_socket(impl_->context, ZMQ_ROUTER);
    if (impl_->router == nullptr) {
        spdlog::error("ui control server init failed endpoint={} step=zmq_socket(ZMQ_ROUTER) err={}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_ctx_term(impl_->context);
        impl_->context = nullptr;
        return;
    }
    constexpr int linger_ms = 0;
    (void)zmq_setsockopt(impl_->router, ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
    if (zmq_bind(impl_->router, endpoint_.c_str()) != 0) {
        spdlog::error("ui control server init failed endpoint={} step=zmq_bind err={}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_close(impl_->router);
        zmq_ctx_term(impl_->context);
        impl_->router = nullptr;
        impl_->context = nullptr;
    }
}

ControlServer::~ControlServer() {
    if (impl_) {
        if (impl_->router != nullptr) {
            zmq_close(impl_->router);
        }
        if (impl_->context != nullptr) {
            zmq_ctx_term(impl_->context);
        }
    }
}

void ControlServer::set_snapshot(ServiceSnapshot snapshot) {
    snapshot_ = std::move(snapshot);
}

void ControlServer::poll_once() {
    if (!(impl_ && impl_->router != nullptr)) {
        return;
    }

    zmq_pollitem_t items[] = {{impl_->router, 0, ZMQ_POLLIN, 0}};
    if (zmq_poll(items, 1, 0) <= 0 || (items[0].revents & ZMQ_POLLIN) == 0) {
        return;
    }

    zmq_msg_t identity;
    zmq_msg_t frame;
    zmq_msg_init(&identity);
    zmq_msg_init(&frame);
    if (zmq_msg_recv(&identity, impl_->router, 0) < 0) {
        zmq_msg_close(&identity);
        zmq_msg_close(&frame);
        return;
    }

    // ROUTER receives multipart messages. For REQ clients the frames are:
    //   [identity][empty delimiter][payload]
    // For DEALER clients the frames are typically:
    //   [identity][payload]
    // We must consume the optional empty delimiter, otherwise we parse an empty
    // body and desynchronize subsequent receives.
    if (zmq_msg_more(&identity) == 0) {
        zmq_msg_close(&identity);
        zmq_msg_close(&frame);
        return;
    }

    if (zmq_msg_recv(&frame, impl_->router, 0) < 0) {
        zmq_msg_close(&identity);
        zmq_msg_close(&frame);
        return;
    }

    if (zmq_msg_size(&frame) == 0 && zmq_msg_more(&frame) != 0) {
        zmq_msg_close(&frame);
        zmq_msg_init(&frame);
        if (zmq_msg_recv(&frame, impl_->router, 0) < 0) {
            zmq_msg_close(&identity);
            zmq_msg_close(&frame);
            return;
        }
    }

    // Drain any unexpected extra frames so we don't break the next request.
    while (zmq_msg_more(&frame) != 0) {
        zmq_msg_t junk;
        zmq_msg_init(&junk);
        if (zmq_msg_recv(&junk, impl_->router, 0) < 0) {
            zmq_msg_close(&junk);
            break;
        }
        zmq_msg_close(&junk);
    }

    const auto* raw = static_cast<const char*>(zmq_msg_data(&frame));
    const std::string request_text {raw, raw + zmq_msg_size(&frame)};
    spdlog::debug("ui control req endpoint={} bytes={}", endpoint_, request_text.size());
    zmq_msg_close(&frame);

    std::string command = "unknown";
    std::string request_id;
    json reply;
    try {
        const auto request = json::parse(request_text);
        if (!request.is_object()) {
            throw std::runtime_error("request must be a JSON object");
        }
        if (request.contains("command") && request["command"].is_string()) {
            command = request["command"].get<std::string>();
        }
        if (request.contains("request_id") && request["request_id"].is_string()) {
            request_id = request["request_id"].get<std::string>();
        }
        spdlog::debug("ui control parsed command={} request_id={}", command, request_id);
        const auto data = request.contains("data") && request["data"].is_object() ? request["data"] : json::object();

        if (command == "help") {
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{{"commands", json::array({"help", "ping", "status", "list-pages", "list-assignments", "list-scenes", "assign-page", "swap-pages", "activate-scene"})}};
        } else if (command == "ping") {
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{{"message", "pong"}};
        } else if (command == "status") {
            json displays = json::array();
            for (const auto& ui : snapshot_.displays) {
                displays.push_back(ui_json(ui));
            }
            json assignments = json::array();
            if (page_controller_) {
                for (const auto& assignment : page_controller_->assignments()) {
                    assignments.push_back(assignment_json(assignment));
                }
            }
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{
                {"ui_events_endpoint", snapshot_.ui_events_endpoint},
                {"ui_control_endpoint", snapshot_.ui_control_endpoint},
                {"display_count", snapshot_.displays.size()},
                {"active_scene", page_controller_ ? page_controller_->active_scene_id() : std::string {}},
                {"displays", std::move(displays)},
                {"assignments", std::move(assignments)}
            };
        } else if (command == "list-pages") {
            json pages = json::array();
            if (page_controller_) {
                for (const auto& page : page_controller_->pages()) {
                    pages.push_back(page_json(page));
                }
            }
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{{"pages", std::move(pages)}};
        } else if (command == "list-assignments") {
            json assignments = json::array();
            if (page_controller_) {
                for (const auto& assignment : page_controller_->assignments()) {
                    assignments.push_back(assignment_json(assignment));
                }
            }
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{{"assignments", std::move(assignments)}};
        } else if (command == "list-scenes") {
            json scenes = json::array();
            if (page_controller_) {
                for (const auto& scene : page_controller_->scenes()) {
                    scenes.push_back(scene_json(scene));
                }
            }
            reply = envelope(snapshot_.service_name, command, request_id, true);
            reply["data"] = json{{"active_scene", page_controller_ ? page_controller_->active_scene_id() : std::string {}}, {"scenes", std::move(scenes)}};
        } else if (command == "assign-page") {
            if (!data.contains("display_id") || !data["display_id"].is_string() || !data.contains("page_id") || !data["page_id"].is_string()) {
                reply = envelope(snapshot_.service_name, command, request_id, false);
                reply["error"] = json{{"code", "invalid-request"}, {"message", "assign-page requires string fields data.display_id and data.page_id"}};
            } else {
                std::string error;
                const auto ok = page_controller_ && page_controller_->assign_page(data["display_id"].get<std::string>(), data["page_id"].get<std::string>(), error);
                reply = envelope(snapshot_.service_name, command, request_id, ok);
                if (ok) {
                    spdlog::info("ui scene/page change command=assign-page display_id={} page_id={} active_scene={}",
                                 data["display_id"].get<std::string>(),
                                 data["page_id"].get<std::string>(),
                                 page_controller_->active_scene_id());
                    reply["data"] = json{{"message", "page assigned"}, {"active_scene", page_controller_->active_scene_id()}};
                } else {
                    reply["error"] = json{{"code", "assignment-failed"}, {"message", error.empty() ? "failed to assign page" : error}};
                }
            }
        } else if (command == "swap-pages") {
            if (!data.contains("display_a") || !data["display_a"].is_string() || !data.contains("display_b") || !data["display_b"].is_string()) {
                reply = envelope(snapshot_.service_name, command, request_id, false);
                reply["error"] = json{{"code", "invalid-request"}, {"message", "swap-pages requires string fields data.display_a and data.display_b"}};
            } else {
                std::string error;
                const auto ok = page_controller_ && page_controller_->swap_pages(data["display_a"].get<std::string>(), data["display_b"].get<std::string>(), error);
                reply = envelope(snapshot_.service_name, command, request_id, ok);
                if (ok) {
                    spdlog::info("ui scene/page change command=swap-pages display_a={} display_b={} active_scene={}",
                                 data["display_a"].get<std::string>(),
                                 data["display_b"].get<std::string>(),
                                 page_controller_->active_scene_id());
                    reply["data"] = json{{"message", "pages swapped"}, {"active_scene", page_controller_->active_scene_id()}};
                } else {
                    reply["error"] = json{{"code", "swap-failed"}, {"message", error.empty() ? "failed to swap pages" : error}};
                }
            }
        } else if (command == "activate-scene") {
            if (!data.contains("scene_id") || !data["scene_id"].is_string()) {
                reply = envelope(snapshot_.service_name, command, request_id, false);
                reply["error"] = json{{"code", "invalid-request"}, {"message", "activate-scene requires string field data.scene_id"}};
            } else {
                std::string error;
                const auto ok = page_controller_ && page_controller_->activate_scene(data["scene_id"].get<std::string>(), error);
                reply = envelope(snapshot_.service_name, command, request_id, ok);
                if (ok) {
                    spdlog::info("ui scene change command=activate-scene scene_id={} active_scene={}",
                                 data["scene_id"].get<std::string>(),
                                 page_controller_->active_scene_id());
                    reply["data"] = json{{"message", "scene activated"}, {"active_scene", page_controller_->active_scene_id()}};
                } else {
                    reply["error"] = json{{"code", "scene-activation-failed"}, {"message", error.empty() ? "failed to activate scene" : error}};
                }
            }
        } else {
            reply = envelope(snapshot_.service_name, command, request_id, false);
            reply["error"] = json{{"code", "unknown-command"}, {"message", "Command is not supported"}};
        }
    } catch (const std::exception&) {
        reply = envelope(snapshot_.service_name, command, request_id, false);
        reply["error"] = json{{"code", "invalid-json"}, {"message", "Request body is not valid JSON"}};
    }

    const auto payload = reply.dump();
    spdlog::debug("ui control reply command={} bytes={}", command, payload.size());
    (void)zmq_send(impl_->router, zmq_msg_data(&identity), zmq_msg_size(&identity), ZMQ_SNDMORE);
    // ROUTER->REQ reply should include empty delimiter frame.
    // Keep it unconditional to stay compatible across REQ variants.
    (void)zmq_send(impl_->router, "", 0, ZMQ_SNDMORE);
    (void)zmq_send(impl_->router, payload.data(), payload.size(), 0);
    zmq_msg_close(&identity);
}

}  // namespace raptor::ui
