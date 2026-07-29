#include "raptor_ui/application.hpp"

#include "raptor_ui/control_server.hpp"
#include "raptor_ui/display_backend.hpp"
#include "raptor_ui/ipc.hpp"
#include "raptor_ui/page_controller.hpp"
#include "raptor_ui/ui_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace raptor::ui {
namespace {

struct DisplayRuntime {
    const DisplayConfig* config {nullptr};
    std::unique_ptr<DisplayBackend> backend;
    UiSnapshot snapshot;
    std::chrono::steady_clock::time_point next_render;
};

bool page_supported_by_display(const PageConfig& page, const DisplayConfig& display) {
    return page.allowed_models.empty() ||
           std::find(page.allowed_models.begin(), page.allowed_models.end(), display.model) != page.allowed_models.end();
}

const PageConfig* find_page_for_display_by_type(const PageController& controller,
                                                const std::string& display_id,
                                                const std::string& page_type) {
    const auto* disp = controller.display(display_id);
    if (disp == nullptr) {
        return nullptr;
    }
    for (const auto& page : controller.pages()) {
        if (page.type == page_type && page_supported_by_display(page, *disp)) {
            return &page;
        }
    }
    return nullptr;
}

void apply_page(UiSnapshot& snapshot, const PageConfig& page) {
    snapshot.page_id = page.id;
    snapshot.page_type = page.type;
    snapshot.page_title = page.title;
    snapshot.page_variant = page.default_variant;
    snapshot.page_image_path = page.image.path;
    snapshot.page_image_x = page.image.x;
    snapshot.page_image_y = page.image.y;
}

void apply_context_fallback_page(UiSnapshot& snapshot, const std::string& context) {
    snapshot.page_id = "context_" + context;
    snapshot.page_type = context;
    snapshot.page_title = context == "song"
        ? "Song"
        : (context == "track" ? "Track" : (context == "chords" ? "Chords" : "Settings"));
    snapshot.page_variant = "square";
    snapshot.page_image_path.clear();
    snapshot.page_image_x = 0;
    snapshot.page_image_y = 0;
}

std::string normalize_transport(std::string_view transport) {
    std::string normalized;
    normalized.reserve(transport.size());
    for (const unsigned char c : transport) {
        normalized.push_back(static_cast<char>(std::tolower(c)));
    }
    return normalized;
}

enum class TransportOverlay {
    None,
    Playing,
    Recording,
};

TransportOverlay overlay_for_transport(std::string_view normalized_transport) {
    if (normalized_transport == "recording") {
        return TransportOverlay::Recording;
    }
    if (normalized_transport == "playing") {
        return TransportOverlay::Playing;
    }
    return TransportOverlay::None;
}

const char* overlay_name(TransportOverlay overlay) {
    switch (overlay) {
        case TransportOverlay::Playing:
            return "playing";
        case TransportOverlay::Recording:
            return "recording";
        default:
            return "none";
    }
}

const char* overlay_page_type(TransportOverlay overlay) {
    switch (overlay) {
        case TransportOverlay::Playing:
            return "playing";
        case TransportOverlay::Recording:
            return "recording";
        default:
            return "";
    }
}

[[maybe_unused]] void maybe_override_transport_page(UiSnapshot& snapshot, const PageController& controller) {
    // During active transport states we can override the assigned page with dedicated
    // transport pages (playing/recording). When transport returns to stopped, assignment
    // is restored by apply_page_assignment() in the next loop iteration.
    const std::string normalized_transport = normalize_transport(snapshot.sequencer.transport);
    const TransportOverlay overlay = overlay_for_transport(normalized_transport);

    static std::unordered_map<std::string, TransportOverlay> last_overlay_by_display;
    const auto last_it = last_overlay_by_display.find(snapshot.display_id);
    const TransportOverlay last_overlay = (last_it == last_overlay_by_display.end()) ? TransportOverlay::None : last_it->second;
    if (last_overlay != overlay) {
        spdlog::debug(
            "ui transport-state display={} transport_raw='{}' normalized='{}' overlay={}",
            snapshot.display_id,
            snapshot.sequencer.transport,
            normalized_transport,
            overlay_name(overlay));
        last_overlay_by_display[snapshot.display_id] = overlay;
    }

    if (overlay == TransportOverlay::None) {
        return;
    }

    const std::string page_type = overlay_page_type(overlay);
    const auto* transport_page = find_page_for_display_by_type(controller, snapshot.display_id, page_type);
    if (transport_page == nullptr) {
        static std::unordered_map<std::string, std::uint64_t> missing_transport_page_counts;
        auto& count = missing_transport_page_counts[snapshot.display_id + "|" + page_type];
        ++count;
        if (count == 1 || (count % 100) == 0) {
            spdlog::warn(
                "ui {}-page missing display={} model={} transport_raw='{}' misses={}",
                page_type,
                snapshot.display_id,
                snapshot.display_model,
                snapshot.sequencer.transport,
                count);
        }
        return;
    }

    if (snapshot.page_id != transport_page->id) {
        spdlog::trace(
            "ui page override display={} from={}({}) to={}({}) transport_raw='{}'",
            snapshot.display_id,
            snapshot.page_id,
            snapshot.page_type,
            transport_page->id,
            transport_page->type,
            snapshot.sequencer.transport);
    }

    apply_page(snapshot, *transport_page);
}

void maybe_apply_context_page(UiSnapshot& snapshot, const PageController& controller) {
    const std::string& context = snapshot.sequencer.input_context;
    if (context != "song" && context != "track" && context != "settings" && context != "chords") {
        return;
    }
    if (snapshot.page_type == context) {
        return;
    }

    const auto* context_page = find_page_for_display_by_type(controller, snapshot.display_id, context);
    if (context_page == nullptr) {
        static std::unordered_map<std::string, std::uint64_t> missing_context_page_counts;
        auto& count = missing_context_page_counts[snapshot.display_id + "|" + context];
        ++count;
        if (count == 1 || (count % 100) == 0) {
            spdlog::warn(
                "ui context page missing display={} model={} context={} misses={}",
                snapshot.display_id,
                snapshot.display_model,
                context,
                count);
        }
        apply_context_fallback_page(snapshot, context);
        return;
    }

    apply_page(snapshot, *context_page);
}

}  // namespace

Application::Application(ServiceConfig config) : config_(std::move(config)) {}

int Application::run() {
    try {
        spdlog::debug(
            "ui start displays={} ui_events={} ui_control={} seq_control={}",
            config_.displays.size(),
            config_.ipc.ui_events_endpoint,
            config_.ipc.ui_control_endpoint,
            config_.ipc.sequencer_control_endpoint);

        auto page_controller = std::make_shared<PageController>(config_);
        spdlog::info("ui active scene at startup={}", page_controller->active_scene_id());

        std::vector<DisplayRuntime> displays;
        displays.reserve(config_.displays.size());

        for (const auto& display_config : config_.displays) {
            spdlog::debug(
                "ui display init id={} driver={} model={} refresh_ms={}",
                display_config.id,
                display_config.driver,
                display_config.model,
                display_config.refresh_period_ms);
            auto backend = DisplayBackend::create(display_config);
            backend->initialize();

            auto snapshot = initialize_snapshot(display_config, *backend, *page_controller);

            displays.push_back(DisplayRuntime {
                .config = &display_config,
                .backend = std::move(backend),
                .snapshot = std::move(snapshot),
                .next_render = std::chrono::steady_clock::now(),
            });
        }

        EventPublisher publisher {config_.ipc.ui_events_endpoint, "ui.snapshot"};
        ControlClient sequencer_client {config_.ipc.sequencer_control_endpoint};
        ControlServer control_server {config_.ipc.ui_control_endpoint, config_, page_controller};

        ServiceSnapshot service_snapshot;
        service_snapshot.ui_events_endpoint = config_.ipc.ui_events_endpoint;
        service_snapshot.ui_control_endpoint = config_.ipc.ui_control_endpoint;

        auto next_sequencer_poll = std::chrono::steady_clock::now();
        auto next_project_poll = std::chrono::steady_clock::now();
        std::optional<SequencerSongSummary> cached_song_summary;
        std::optional<std::uint64_t> cached_song_revision;

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_sequencer_poll) {
                const auto status = sequencer_client.query_status("ui-service-sequencer-status");
                if (status.has_value()) {
                    const bool song_changed =
                        status->song_revision.has_value() &&
                        (!cached_song_revision.has_value() || *cached_song_revision != *status->song_revision);
                    if (song_changed || now >= next_project_poll) {
                        if (song_changed) {
                            cached_song_summary.reset();
                            cached_song_revision.reset();
                        }
                        const auto project = sequencer_client.query_project("ui-service-sequencer-project");
                        if (project.has_value()) {
                            cached_song_summary = std::move(project);
                            if (status->song_revision.has_value()) {
                                cached_song_revision = *status->song_revision;
                            }
                        }
                        next_project_poll = now + std::chrono::milliseconds(song_changed ? 100 : 1000);
                    }
                }
                for (auto& display : displays) {
                    if (status.has_value()) {
                        auto merged = *status;
                        if (cached_song_summary.has_value()) {
                            merged.song = *cached_song_summary;
                            if (!status->song.id.empty()) {
                                merged.song.id = status->song.id;
                            }
                            if (!status->song.title.empty()) {
                                merged.song.title = status->song.title;
                            }
                            if (status->song.slot > 0) {
                                merged.song.slot = status->song.slot;
                            }
                            if (!status->song.active_track_id.empty()) {
                                merged.song.active_track_id = status->song.active_track_id;
                            }
                        }
                        if (display.snapshot.sequencer.transport != status->transport) {
                            spdlog::debug(
                                "ui sequencer transport update display={} {} -> {}",
                                display.snapshot.display_id,
                                display.snapshot.sequencer.transport,
                                status->transport);
                        }
                        display.snapshot.sequencer = std::move(merged);
                    } else {
                        display.snapshot.sequencer.reachable = false;
                        display.snapshot.sequencer.service = "raptor-sequencer";
                        display.snapshot.sequencer.summary = "unreachable";
                        display.snapshot.sequencer.timestamp_ns = 0;
                    }
                }
                next_sequencer_poll = now + std::chrono::milliseconds(250);
            }

            for (auto& display : displays) {
                apply_page_assignment(display.snapshot, *page_controller);
                maybe_apply_context_page(display.snapshot, *page_controller);
                if (now >= display.next_render) {
                    ++display.snapshot.render_count;
                    spdlog::trace(
                        "ui render display={} page={} type={} variant={} count={}",
                        display.snapshot.display_id,
                        display.snapshot.page_id,
                        display.snapshot.page_type,
                        display.snapshot.page_variant,
                        display.snapshot.render_count);
                    display.backend->render(display.snapshot);
                    publisher.publish_snapshot(display.snapshot);
                    display.next_render = now + std::chrono::milliseconds(display.config->refresh_period_ms);
                }
            }

            service_snapshot.displays.clear();
            for (const auto& display : displays) {
                service_snapshot.displays.push_back(display.snapshot);
            }
            control_server.set_snapshot(service_snapshot);
            control_server.poll_once();

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const std::exception& ex) {
        spdlog::error("ui fatal error: {}", ex.what());
        return 1;
    }
}

}  // namespace raptor::ui
