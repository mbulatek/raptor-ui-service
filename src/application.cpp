#include "raptor_ui/application.hpp"

#include "raptor_ui/control_server.hpp"
#include "raptor_ui/display_backend.hpp"
#include "raptor_ui/ipc.hpp"
#include "raptor_ui/page_controller.hpp"
#include "raptor_ui/ui_runtime.hpp"
#include "raptor_ui/view.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
        ViewRegistry view_registry;
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
        SequencerEventSubscriber sequencer_events {config_.ipc.sequencer_events_endpoint};
        ControlClient sequencer_client {config_.ipc.sequencer_control_endpoint};
        ControlServer control_server {config_.ipc.ui_control_endpoint, config_, page_controller};

        ServiceSnapshot service_snapshot;
        service_snapshot.ui_events_endpoint = config_.ipc.ui_events_endpoint;
        service_snapshot.ui_control_endpoint = config_.ipc.ui_control_endpoint;

        UpstreamStatus sequencer_status;
        sequencer_status.service = "raptor-engine";
        sequencer_status.summary = "waiting for initial state";
        bool sequencer_hydrated = false;
        auto next_hydration_attempt = std::chrono::steady_clock::now();
        auto next_project_fetch = std::chrono::steady_clock::now();
        std::optional<SequencerSongSummary> cached_song_summary;
        std::optional<std::uint64_t> cached_song_revision;

        const auto merged_sequencer_status = [&]() {
            auto merged = sequencer_status;
            if (cached_song_summary.has_value()) {
                merged.song = *cached_song_summary;
                if (!sequencer_status.song.id.empty()) {
                    merged.song.id = sequencer_status.song.id;
                }
                if (!sequencer_status.song.title.empty()) {
                    merged.song.title = sequencer_status.song.title;
                }
                if (sequencer_status.song.slot > 0) {
                    merged.song.slot = sequencer_status.song.slot;
                }
                if (!sequencer_status.song.active_track_id.empty()) {
                    merged.song.active_track_id = sequencer_status.song.active_track_id;
                }
            }
            return merged;
        };

        const auto update_display_status = [&]() {
            const auto merged = merged_sequencer_status();
            for (auto& display : displays) {
                display.snapshot.sequencer = merged;
            }
        };

        const auto render_displays = [&](const std::chrono::steady_clock::time_point now, const bool force) {
            for (auto& display : displays) {
                apply_page_assignment(display.snapshot, *page_controller);
                view_registry.apply_active_view(display.snapshot, *page_controller);
                if (!force && now < display.next_render) {
                    continue;
                }
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
                display.next_render = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(display.config->refresh_period_ms);
            }
        };

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            bool state_changed = false;
            bool project_refreshed = false;

            // Render every semantic state transition before consuming the next
            // one so short press/release pairs remain visible on the display.
            for (int event_count = 0; event_count < 32; ++event_count) {
                bool semantic_state_changed = false;
                if (!sequencer_events.poll_once(sequencer_status, semantic_state_changed)) {
                    break;
                }
                if (!semantic_state_changed) {
                    continue;
                }

                state_changed = true;
                const bool project_changed =
                    sequencer_status.song_revision.has_value() &&
                    (!cached_song_revision.has_value() ||
                     *sequencer_status.song_revision != *cached_song_revision);
                if (project_changed) {
                    // Keep the last complete project visible while fetching a
                    // newer revision. Only discard it when the song changed.
                    const bool song_changed =
                        cached_song_summary.has_value() &&
                        !sequencer_status.song.id.empty() &&
                        cached_song_summary->id != sequencer_status.song.id;
                    if (song_changed) {
                        cached_song_summary.reset();
                        cached_song_revision.reset();
                    }
                    next_project_fetch = now;
                }
                update_display_status();
                render_displays(std::chrono::steady_clock::now(), true);
            }

            if (!sequencer_hydrated && now >= next_hydration_attempt) {
                const auto hydrated = sequencer_client.query_status("ui-service-initial-state");
                if (hydrated.has_value()) {
                    sequencer_status = *hydrated;
                    sequencer_hydrated = true;
                    state_changed = true;
                } else {
                    sequencer_status.reachable = false;
                    sequencer_status.summary = "waiting for raptor-engine";
                }
                next_hydration_attempt = now + std::chrono::seconds(1);
            }

            const bool project_needs_refresh =
                !cached_song_summary.has_value() ||
                (sequencer_status.song_revision.has_value() &&
                 (!cached_song_revision.has_value() ||
                  *sequencer_status.song_revision != *cached_song_revision));
            if (sequencer_status.reachable && project_needs_refresh && now >= next_project_fetch) {
                const auto project = sequencer_client.query_project("ui-service-project-hydration");
                if (project.has_value()) {
                    cached_song_summary = *project;
                    cached_song_revision = sequencer_status.song_revision;
                    state_changed = true;
                    project_refreshed = true;
                }
                next_project_fetch = now + std::chrono::seconds(1);
            }

            if (state_changed) {
                update_display_status();
            }
            render_displays(now, project_refreshed);

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
