#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raptor::ui {

struct MidiEventSummary {
    bool available {false};
    std::string module_id;
    int global_port {-1};
    std::string bytes_hex;
    std::uint64_t sequence {0};
    std::uint64_t timestamp_ns {0};
};

struct SequencerControllerLaneSummary {
    std::string key;
    std::string label;
    bool muted {false};
    std::uint32_t event_count {0};
};

struct SequencerTrackSummary {
    std::string id;
    std::string name;
    bool muted {false};
    std::string midi_in;
    std::string midi_out;
    std::string midi_in_label;
    std::string midi_out_label;
    int midi_channel_in {-1};   // 0..16, -1 unknown
    int midi_channel_out {-1};  // 1..16, -1 unknown
    bool send_sync_enabled {false};
    std::vector<SequencerControllerLaneSummary> controller_lanes;
};

struct SequencerSongSummary {
    bool available {false};
    std::string id;
    std::string title;
    int slot {-1};
    std::string active_track_id;
    std::vector<SequencerTrackSummary> tracks;
};

struct UiConfirmationSummary {
    bool active {false};
    std::string kind;
    std::string title;
    std::string message;
    std::string confirm_label {"Remove"};
    std::string cancel_label {"Cancel"};
    bool confirm_selected {false};
};

struct UpstreamStatus {
    bool reachable {false};
    std::string service;
    std::string summary;
    std::uint64_t timestamp_ns {0};

    // Optional snapshot details from raptor-engine sequencer control/status.
    std::optional<std::uint64_t> tick;
    std::optional<std::uint64_t> revision_epoch;
    std::optional<std::uint64_t> song_revision;
    std::optional<std::uint64_t> pattern_revision;
    std::optional<double> bpm;
    std::optional<std::uint32_t> ppqn;
    std::string transport;
    std::string active_pattern;
    std::string input_context {"song"};
    std::uint32_t ui_scroll_offset {0};
    std::uint32_t ui_page_offset {0};
    bool ui_editing {false};
    UiConfirmationSummary ui_confirmation;
    std::string clock_source;
    std::string clock_midi_source;
    bool metronome_enabled {false};
    std::string metronome_alsa_device;
    std::uint32_t chord_pad_right_hand_octave {4};
    std::vector<bool> chord_pad_pressed = std::vector<bool>(8, false);
    std::optional<std::uint32_t> active_step;

    std::optional<std::uint32_t> bar;
    std::optional<std::uint32_t> bars_total;
    std::optional<std::uint32_t> beat;
    std::optional<std::uint32_t> beats_per_bar;
    std::optional<std::uint32_t> beat_unit;
    std::optional<std::uint32_t> active_clip_index;

    std::optional<int> midi_in_port;
    std::optional<int> midi_in_channel;
    std::optional<int> midi_out_port;
    std::optional<int> midi_out_channel;
    std::string recording_quantize;
    std::string loop_quantize;
    SequencerSongSummary song;
};

struct UiSnapshot {
    std::string display_id;
    std::string display_driver;
    std::string display_model;
    std::string layout;
    std::string page_id;
    std::string page_type;
    std::string page_title;
    std::string page_variant {"auto"};
    std::string page_image_path;
    std::uint16_t page_image_x {0};
    std::uint16_t page_image_y {0};
    std::string view_id;
    std::uint32_t view_page_index {0};
    std::uint32_t view_page_count {0};
    std::uint16_t display_width {0};
    std::uint16_t display_height {0};
    std::uint64_t render_count {0};
    MidiEventSummary last_midi;
    UpstreamStatus sequencer;
};

class EventPublisher {
public:
    EventPublisher(std::string endpoint, std::string topic);
    ~EventPublisher();

    EventPublisher(const EventPublisher&) = delete;
    EventPublisher& operator=(const EventPublisher&) = delete;

    void publish_snapshot(const UiSnapshot& snapshot);

private:
    std::string endpoint_;
    std::string topic_;
    struct Impl;
    Impl* impl_ {nullptr};
};

class MidiEventSubscriber {
public:
    explicit MidiEventSubscriber(std::string endpoint);
    ~MidiEventSubscriber();

    MidiEventSubscriber(const MidiEventSubscriber&) = delete;
    MidiEventSubscriber& operator=(const MidiEventSubscriber&) = delete;

    bool poll_once(MidiEventSummary& summary);

private:
    std::string endpoint_;
    struct Impl;
    Impl* impl_ {nullptr};
};

class ControlClient {
public:
    explicit ControlClient(std::string endpoint);
    ~ControlClient();

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    std::optional<UpstreamStatus> query_status(const std::string& request_id) const;
    std::optional<SequencerSongSummary> query_project(const std::string& request_id) const;

private:
    std::string endpoint_;
};

}  // namespace raptor::ui
