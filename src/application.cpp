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
                view_registry.apply_active_view(display.snapshot, *page_controller);
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
