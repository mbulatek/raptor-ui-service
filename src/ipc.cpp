#include "raptor_ui/ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <zmq.h>

namespace raptor::ui {
namespace {

using json = nlohmann::json;

constexpr char kSchemaVersion[] = "1.0";
constexpr char kServiceName[] = "raptor-native-ui";
constexpr char kMidiTopic[] = "midi.packet";

struct MidiPortLabelMap {
    std::unordered_map<std::string, std::string> inputs;
    std::unordered_map<std::string, std::string> outputs;
};

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

json sequencer_json(const UpstreamStatus& status) {
    json j = {
        {"reachable", status.reachable},
        {"service", status.service},
        {"summary", status.summary},
        {"timestamp_ns", status.timestamp_ns},
    };
    if (status.tick.has_value()) {
        j["tick"] = *status.tick;
    }
    if (status.bpm.has_value()) {
        j["bpm"] = *status.bpm;
    }
    if (status.ppqn.has_value()) {
        j["ppqn"] = *status.ppqn;
    }
    if (!status.transport.empty()) {
        j["transport"] = status.transport;
    }
    if (!status.active_pattern.empty()) {
        j["active_pattern"] = status.active_pattern;
    }
    if (!status.input_context.empty()) {
        j["input_context"] = status.input_context;
    }
    j["ui_scroll_offset"] = status.ui_scroll_offset;
    j["ui_page_offset"] = status.ui_page_offset;
    j["ui_editing"] = status.ui_editing;
    j["ui_confirmation"] = ui_confirmation_json(status.ui_confirmation);
    if (!status.clock_source.empty()) {
        j["clock_source"] = status.clock_source;
    }
    if (!status.clock_midi_source.empty()) {
        j["clock_midi_source"] = status.clock_midi_source;
    }
    j["metronome_enabled"] = status.metronome_enabled;
    if (!status.metronome_alsa_device.empty()) {
        j["metronome_alsa_device"] = status.metronome_alsa_device;
    }
    j["chord_pad_right_hand_octave"] = status.chord_pad_right_hand_octave;
    j["chord_pad_pressed"] = status.chord_pad_pressed;
    if (status.active_step.has_value()) {
        j["active_step"] = *status.active_step;
    }
    if (status.bar.has_value()) {
        j["bar"] = *status.bar;
    }
    if (status.bars_total.has_value()) {
        j["bars_total"] = *status.bars_total;
    }
    if (status.beat.has_value()) {
        j["beat"] = *status.beat;
    }
    if (status.beats_per_bar.has_value()) {
        j["beats_per_bar"] = *status.beats_per_bar;
    }
    if (status.beat_unit.has_value()) {
        j["beat_unit"] = *status.beat_unit;
    }
    if (status.active_clip_index.has_value()) {
        j["active_clip_index"] = *status.active_clip_index;
    }
    if (status.midi_in_port.has_value()) {
        j["midi_in_port"] = *status.midi_in_port;
    }
    if (status.midi_in_channel.has_value()) {
        j["midi_in_channel"] = *status.midi_in_channel;
    }
    if (status.midi_out_port.has_value()) {
        j["midi_out_port"] = *status.midi_out_port;
    }
    if (status.midi_out_channel.has_value()) {
        j["midi_out_channel"] = *status.midi_out_channel;
    }
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
            {"slot", status.song.slot},
            {"active_track_id", status.song.active_track_id},
            {"tracks", std::move(tracks)},
        };
    }
    return j;
}

json snapshot_json(const UiSnapshot& snapshot) {
    return {
        {"schema_version", kSchemaVersion},
        {"service", kServiceName},
        {"type", "ui.snapshot"},
        {"timestamp_ns", monotonic_time_ns()},
        {"display",
         {
             {"id", snapshot.display_id},
             {"driver", snapshot.display_driver},
             {"model", snapshot.display_model},
             {"layout", snapshot.layout},
             {"width", snapshot.display_width},
             {"height", snapshot.display_height},
         }},
        {"page",
         {
             {"id", snapshot.page_id},
             {"type", snapshot.page_type},
             {"title", snapshot.page_title},
             {"variant", snapshot.page_variant},
             {"image_path", snapshot.page_image_path},
             {"image_x", snapshot.page_image_x},
             {"image_y", snapshot.page_image_y},
         }},
        {"view",
         {
             {"id", snapshot.view_id},
             {"page_index", snapshot.view_page_index},
             {"page_count", snapshot.view_page_count},
         }},
        {"render_count", snapshot.render_count},
        {"last_midi", midi_json(snapshot.last_midi)},
        {"sequencer", sequencer_json(snapshot.sequencer)},
    };
}

std::optional<json> request_control_json(const std::string& endpoint, const json& request, const int timeout_ms) {
    void* context = zmq_ctx_new();
    if (context == nullptr) {
        return std::nullopt;
    }

    void* socket = zmq_socket(context, ZMQ_REQ);
    if (socket == nullptr) {
        zmq_ctx_term(context);
        return std::nullopt;
    }

    constexpr int linger_ms = 0;
    (void)zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    (void)zmq_setsockopt(socket, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    (void)zmq_setsockopt(socket, ZMQ_LINGER, &linger_ms, sizeof(linger_ms));

    if (zmq_connect(socket, endpoint.c_str()) != 0) {
        zmq_close(socket);
        zmq_ctx_term(context);
        return std::nullopt;
    }

    const auto payload = request.dump();
    if (zmq_send(socket, payload.data(), payload.size(), 0) < 0) {
        zmq_close(socket);
        zmq_ctx_term(context);
        return std::nullopt;
    }

    zmq_msg_t reply_msg;
    zmq_msg_init(&reply_msg);
    const auto size = zmq_msg_recv(&reply_msg, socket, 0);
    std::string reply_text;
    if (size > 0) {
        const auto* data = static_cast<const char*>(zmq_msg_data(&reply_msg));
        reply_text.assign(data, data + zmq_msg_size(&reply_msg));
    }
    zmq_msg_close(&reply_msg);
    zmq_close(socket);
    zmq_ctx_term(context);
    if (size <= 0) {
        return std::nullopt;
    }

    try {
        return json::parse(reply_text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string json_port_token(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int>());
    }
    return {};
}

void add_label(std::unordered_map<std::string, std::string>& labels, const std::string& key, const std::string& label) {
    if (!key.empty() && !label.empty()) {
        labels[key] = label;
    }
}

MidiPortLabelMap midi_port_labels_from_project(const json& project_json) {
    MidiPortLabelMap labels;
    const auto ports_it = project_json.find("midi_ports");
    if (ports_it == project_json.end() || !ports_it->is_array()) {
        return labels;
    }

    for (const auto& p : *ports_it) {
        if (!p.is_object()) {
            continue;
        }
        const std::string global_port = p.contains("global_port") ? json_port_token(p.at("global_port")) : std::string {};
        const std::string device_id = p.value("device_id", std::string {});
        const std::string input_label = p.value("input_label", std::string {});
        const std::string output_label = p.value("output_label", std::string {});

        add_label(labels.inputs, global_port, input_label.empty() ? output_label : input_label);
        add_label(labels.outputs, global_port, output_label.empty() ? input_label : output_label);
        add_label(labels.inputs, device_id, input_label.empty() ? output_label : input_label);
        add_label(labels.outputs, device_id, output_label.empty() ? input_label : output_label);
    }

    return labels;
}

std::string midi_label_for(
    const MidiPortLabelMap& labels,
    const std::string& token,
    const std::string& device_id,
    const bool input) {
    const auto& map = input ? labels.inputs : labels.outputs;
    if (!device_id.empty()) {
        if (const auto it = map.find(device_id); it != map.end()) {
            return it->second;
        }
    }
    if (!token.empty()) {
        if (const auto it = map.find(token); it != map.end()) {
            return it->second;
        }
    }
    if (token == "-1" || token == "any") {
        return "ANY";
    }
    if (token == "auto") {
        return "Auto";
    }
    return {};
}

bool is_channel_midi_status(const int status) {
    return status >= 0x80 && status <= 0xEF;
}

bool is_controller_lane_event(const json& event) {
    if (!event.is_object()) {
        return false;
    }
    const int size = event.value("size", 0);
    const int status = event.value("status", 0);
    if (size == 0 || !is_channel_midi_status(status)) {
        return false;
    }
    const int kind = status & 0xF0;
    return kind == 0xA0 || kind == 0xB0 || kind == 0xC0 || kind == 0xD0 || kind == 0xE0;
}

std::string controller_lane_key(const json& event) {
    const int status = event.value("status", 0);
    const int kind = status & 0xF0;
    const int data1 = std::clamp(event.value("data1", 0), 0, 127);
    switch (kind) {
    case 0xA0:
        return "polyaftertouch:" + std::to_string(data1);
    case 0xB0:
        return "cc:" + std::to_string(data1);
    case 0xC0:
        return "program";
    case 0xD0:
        return "aftertouch";
    case 0xE0:
        return "pitchbend";
    default:
        return {};
    }
}

std::optional<int> parse_lane_number_suffix(const std::string& key, const std::string_view prefix) {
    if (!key.starts_with(prefix)) {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        const int value = std::stoi(key.substr(prefix.size()), &pos, 10);
        if (pos == key.size() - prefix.size()) {
            return value;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::string controller_lane_label(const std::string& key) {
    if (const auto number = parse_lane_number_suffix(key, "cc:")) {
        return "CC " + std::to_string(*number);
    }
    if (const auto number = parse_lane_number_suffix(key, "polyaftertouch:")) {
        return "Poly AT " + std::to_string(*number);
    }
    if (key == "pitchbend") {
        return "Pitch Bend";
    }
    if (key == "aftertouch") {
        return "Aftertouch";
    }
    if (key == "program") {
        return "Program";
    }
    return key.empty() ? std::string {"Unknown"} : key;
}

int controller_lane_sort_rank(const std::string& key) {
    if (key.starts_with("cc:")) return 0;
    if (key.starts_with("polyaftertouch:")) return 1;
    if (key == "pitchbend") return 2;
    if (key == "aftertouch") return 3;
    if (key == "program") return 4;
    return 5;
}

bool controller_lane_less(const SequencerControllerLaneSummary& left, const SequencerControllerLaneSummary& right) {
    const int left_rank = controller_lane_sort_rank(left.key);
    const int right_rank = controller_lane_sort_rank(right.key);
    if (left_rank != right_rank) {
        return left_rank < right_rank;
    }
    const auto left_cc = parse_lane_number_suffix(left.key, "cc:");
    const auto right_cc = parse_lane_number_suffix(right.key, "cc:");
    if (left_cc.has_value() && right_cc.has_value() && *left_cc != *right_cc) {
        return *left_cc < *right_cc;
    }
    const auto left_poly = parse_lane_number_suffix(left.key, "polyaftertouch:");
    const auto right_poly = parse_lane_number_suffix(right.key, "polyaftertouch:");
    if (left_poly.has_value() && right_poly.has_value() && *left_poly != *right_poly) {
        return *left_poly < *right_poly;
    }
    return left.key < right.key;
}

void add_controller_lane(std::vector<SequencerControllerLaneSummary>& lanes,
                         const std::string& key,
                         const bool muted,
                         const std::uint32_t events) {
    if (key.empty()) {
        return;
    }
    auto it = std::find_if(lanes.begin(), lanes.end(), [&](const SequencerControllerLaneSummary& lane) {
        return lane.key == key;
    });
    if (it == lanes.end()) {
        lanes.push_back(SequencerControllerLaneSummary {
            .key = key,
            .label = controller_lane_label(key),
            .muted = muted,
            .event_count = events,
        });
        return;
    }
    it->muted = it->muted || muted;
    it->event_count += events;
}

const json* find_pattern_json(const json& project_json, const std::string& pattern_id) {
    if (pattern_id.empty() || !project_json.contains("patterns") || !project_json["patterns"].is_array()) {
        return nullptr;
    }
    for (const auto& pattern : project_json["patterns"]) {
        if (pattern.is_object() && pattern.value("id", std::string{}) == pattern_id) {
            return &pattern;
        }
    }
    return nullptr;
}

std::vector<SequencerControllerLaneSummary> parse_controller_lanes_from_track(const json& project_json, const json& track_json) {
    std::vector<SequencerControllerLaneSummary> lanes;
    std::unordered_set<std::string> muted_lanes;
    if (track_json.contains("automation_muted_lanes") && track_json["automation_muted_lanes"].is_array()) {
        for (const auto& lane : track_json["automation_muted_lanes"]) {
            if (!lane.is_string()) {
                continue;
            }
            const std::string key = lane.get<std::string>();
            muted_lanes.insert(key);
            add_controller_lane(lanes, key, true, 0U);
        }
    }

    std::unordered_set<std::string> pattern_ids;
    if (track_json.contains("clips") && track_json["clips"].is_array()) {
        for (const auto& clip : track_json["clips"]) {
            if (clip.is_object()) {
                const std::string pattern_id = clip.value("pattern_id", std::string{});
                if (!pattern_id.empty()) {
                    pattern_ids.insert(pattern_id);
                }
            }
        }
    }
    for (const std::string& pattern_id : pattern_ids) {
        const json* pattern = find_pattern_json(project_json, pattern_id);
        if (pattern == nullptr || !pattern->contains("events") || !(*pattern)["events"].is_array()) {
            continue;
        }
        for (const auto& event : (*pattern)["events"]) {
            if (!is_controller_lane_event(event)) {
                continue;
            }
            const std::string key = controller_lane_key(event);
            add_controller_lane(lanes, key, muted_lanes.contains(key), 1U);
        }
    }
    std::sort(lanes.begin(), lanes.end(), controller_lane_less);
    return lanes;
}

SequencerSongSummary parse_song_summary_from_project(const json& project_json) {
    SequencerSongSummary song;
    if (!project_json.is_object()) {
        return song;
    }

    if (!project_json.contains("song") || !project_json["song"].is_object()) {
        return song;
    }
    const auto& s = project_json["song"];
    song.id = s.value("id", std::string{});
    song.title = s.value("title", std::string{});
    song.slot = s.value("slot_index", -1);
    song.active_track_id = project_json.value("currentTrackId", std::string{});
    const auto midi_labels = midi_port_labels_from_project(project_json);

    if (s.contains("tracks") && s["tracks"].is_array()) {
        for (const auto& t : s["tracks"]) {
            if (!t.is_object()) {
                continue;
            }
            SequencerTrackSummary track;
            track.id = t.value("id", std::string{});
            track.name = t.value("name", std::string{});
            track.muted = t.value("muted", false);
            track.midi_in = t.value("midi_in", std::string{});
            track.midi_out = t.value("midi_out", std::string{});
            track.send_sync_enabled = t.value("send_sync_enabled", false);
            track.midi_in_label = midi_label_for(
                midi_labels,
                track.midi_in,
                t.value("midi_in_device_id", std::string{}),
                true);
            track.midi_out_label = midi_label_for(
                midi_labels,
                track.midi_out,
                t.value("midi_out_device_id", std::string{}),
                false);
            if (t.contains("midi_channel_in")) {
                track.midi_channel_in = t.value("midi_channel_in", -1);
            } else if (t.contains("midi_channel")) {
                track.midi_channel_in = t.value("midi_channel", -1);
            }
            if (t.contains("midi_channel_out")) {
                track.midi_channel_out = t.value("midi_channel_out", -1);
            } else if (t.contains("midi_channel")) {
                track.midi_channel_out = t.value("midi_channel", -1);
            }
            track.controller_lanes = parse_controller_lanes_from_track(project_json, t);
            song.tracks.push_back(std::move(track));
        }
    }

    song.available = true;
    return song;
}

}  // namespace

struct EventPublisher::Impl {
    void* context {nullptr};
    void* socket {nullptr};
};

EventPublisher::EventPublisher(std::string endpoint, std::string topic)
    : endpoint_(std::move(endpoint)), topic_(std::move(topic)), impl_(new Impl()) {
    spdlog::debug("ui pub init endpoint={} topic_prefix={}", endpoint_, topic_);
    const auto directory = endpoint_directory(endpoint_);
    if (!directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
    }
    impl_->context = zmq_ctx_new();
    if (impl_->context == nullptr) {
        spdlog::error("zmq_ctx_new failed for ui publisher {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        return;
    }
    impl_->socket = zmq_socket(impl_->context, ZMQ_PUB);
    if (impl_->socket == nullptr) {
        spdlog::error("zmq_socket(ZMQ_PUB) failed for ui publisher {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_ctx_term(impl_->context);
        impl_->context = nullptr;
        return;
    }
    constexpr int linger_ms = 0;
    (void)zmq_setsockopt(impl_->socket, ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
    // Bind failures are non-fatal for early boot; caller can still run in degraded mode.
    if (zmq_bind(impl_->socket, endpoint_.c_str()) != 0) {
        spdlog::error("zmq_bind failed for ui publisher {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_close(impl_->socket);
        zmq_ctx_term(impl_->context);
        impl_->socket = nullptr;
        impl_->context = nullptr;
    }
}

EventPublisher::~EventPublisher() {
// If the socket is alive here, bind succeeded.

    if (impl_ != nullptr) {
        if (impl_->socket != nullptr) {
            zmq_close(impl_->socket);
        }
        if (impl_->context != nullptr) {
            zmq_ctx_term(impl_->context);
        }
    }
    delete impl_;
}

void EventPublisher::publish_snapshot(const UiSnapshot& snapshot) {
    if (!(impl_ != nullptr && impl_->socket != nullptr)) {
        return;
    }

    const auto payload = snapshot_json(snapshot).dump();
    const auto topic = topic_ + "." + snapshot.display_id;
    const auto rc1 = zmq_send(impl_->socket, topic.data(), topic.size(), ZMQ_SNDMORE);
    const auto rc2 = zmq_send(impl_->socket, payload.data(), payload.size(), 0);
    if (rc1 >= 0 && rc2 >= 0) {
        spdlog::trace("ui snapshot pub endpoint={} topic={} bytes={}", endpoint_, topic, payload.size());
    } else {
        static std::uint64_t publish_failures = 0;
        ++publish_failures;
        if (publish_failures == 1 || (publish_failures % 100) == 0) {
            spdlog::error(
                "ui snapshot publish failed endpoint={} err={} failures={}",
                endpoint_,
                zmq_strerror(zmq_errno()),
                publish_failures);
        }
    }
    (void)rc1;
    (void)rc2;
}

struct MidiEventSubscriber::Impl {
    void* context {nullptr};
    void* socket {nullptr};
};

MidiEventSubscriber::MidiEventSubscriber(std::string endpoint)
    : endpoint_(std::move(endpoint)), impl_(new Impl()) {
    spdlog::debug("midi sub connect endpoint={}", endpoint_);
    impl_->context = zmq_ctx_new();
    if (impl_->context == nullptr) {
        spdlog::error("zmq_ctx_new failed for midi subscriber {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        return;
    }
    impl_->socket = zmq_socket(impl_->context, ZMQ_SUB);
    if (impl_->socket == nullptr) {
        spdlog::error("zmq_socket(ZMQ_SUB) failed for midi subscriber {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_ctx_term(impl_->context);
        impl_->context = nullptr;
        return;
    }
    constexpr int recv_timeout_ms = 0;
    (void)zmq_setsockopt(impl_->socket, ZMQ_RCVTIMEO, &recv_timeout_ms, sizeof(recv_timeout_ms));
    (void)zmq_setsockopt(impl_->socket, ZMQ_SUBSCRIBE, kMidiTopic, sizeof(kMidiTopic) - 1);
    if (zmq_connect(impl_->socket, endpoint_.c_str()) != 0) {
        spdlog::error("zmq_connect failed for midi subscriber {}: {}", endpoint_, zmq_strerror(zmq_errno()));
        zmq_close(impl_->socket);
        zmq_ctx_term(impl_->context);
        impl_->socket = nullptr;
        impl_->context = nullptr;
    }
}

MidiEventSubscriber::~MidiEventSubscriber() {
    if (impl_ != nullptr) {
        if (impl_->socket != nullptr) {
            zmq_close(impl_->socket);
        }
        if (impl_->context != nullptr) {
            zmq_ctx_term(impl_->context);
        }
    }
    delete impl_;
}

bool MidiEventSubscriber::poll_once(MidiEventSummary& summary) {
    if (!(impl_ != nullptr && impl_->socket != nullptr)) {
        return false;
    }

    zmq_pollitem_t items[] = {{impl_->socket, 0, ZMQ_POLLIN, 0}};
    if (zmq_poll(items, 1, 0) <= 0 || (items[0].revents & ZMQ_POLLIN) == 0) {
        return false;
    }

    zmq_msg_t topic;
    zmq_msg_t payload;
    zmq_msg_init(&topic);
    zmq_msg_init(&payload);
    if (zmq_msg_recv(&topic, impl_->socket, 0) < 0 || zmq_msg_recv(&payload, impl_->socket, 0) < 0) {
        static std::uint64_t recv_failures = 0;
        ++recv_failures;
        if (recv_failures == 1 || (recv_failures % 100) == 0) {
            spdlog::error(
                "midi sub recv failed endpoint={} err={} failures={}",
                endpoint_,
                zmq_strerror(zmq_errno()),
                recv_failures);
        }
        zmq_msg_close(&topic);
        zmq_msg_close(&payload);
        return false;
    }

    const auto* raw = static_cast<const char*>(zmq_msg_data(&payload));
    const std::string text {raw, raw + zmq_msg_size(&payload)};
    zmq_msg_close(&topic);
    zmq_msg_close(&payload);

    try {
        const auto root = nlohmann::json::parse(text);
        summary.available = true;
        summary.module_id = root["source"].value("module_id", "");
        summary.global_port = root["source"].value("global_port", -1);
        summary.bytes_hex = root["midi"].value("bytes_hex", "");
        summary.sequence = root.value("sequence", static_cast<std::uint64_t>(0));
        summary.timestamp_ns = root.value("timestamp_ns", static_cast<std::uint64_t>(0));
        spdlog::debug("midi event seq={} module={} port={} bytes=""{}""", summary.sequence, summary.module_id, summary.global_port, summary.bytes_hex);
        return true;
    } catch (const std::exception& ex) {
        static std::uint64_t parse_failures = 0;
        ++parse_failures;
        if (parse_failures == 1 || (parse_failures % 100) == 0) {
            spdlog::error(
                "midi sub json parse failed endpoint={} err={} failures={}",
                endpoint_,
                ex.what(),
                parse_failures);
        }
        return false;
    }
}

ControlClient::ControlClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}
ControlClient::~ControlClient() = default;

std::optional<UpstreamStatus> ControlClient::query_status(const std::string& request_id) const {
    spdlog::trace("sequencer status query endpoint={} request_id={}", endpoint_, request_id);
    constexpr int timeout_ms = 100;
    const auto reply_opt = request_control_json(endpoint_, json{{"command", "status"}, {"request_id", request_id}}, timeout_ms);
    if (!reply_opt.has_value()) {
        static std::uint64_t failures = 0;
        ++failures;
        if (failures == 1 || (failures % 30) == 0) {
            spdlog::warn("sequencer status query failed endpoint={} failures={}", endpoint_, failures);
        }
        return std::nullopt;
    }

    try {
        const auto& reply = *reply_opt;
        UpstreamStatus status;
        status.reachable = reply.value("ok", false);
        status.service = reply.value("service", "unknown");
        status.timestamp_ns = reply.value("timestamp_ns", static_cast<std::uint64_t>(0));
        if (reply.contains("snapshot") && reply["snapshot"].is_object()) {
            const auto& snap = reply["snapshot"];
            if (snap.contains("tick")) {
                status.tick = snap.value("tick", static_cast<std::uint64_t>(0));
            }
            if (snap.contains("revision_epoch")) {
                status.revision_epoch = snap.value("revision_epoch", static_cast<std::uint64_t>(0));
            }
            if (snap.contains("song_revision")) {
                status.song_revision = snap.value("song_revision", static_cast<std::uint64_t>(0));
            }
            if (snap.contains("pattern_revision")) {
                status.pattern_revision = snap.value("pattern_revision", static_cast<std::uint64_t>(0));
            }
            if (snap.contains("bpm")) {
                status.bpm = snap.value("bpm", 0.0);
            }
            if (snap.contains("ppqn")) {
                status.ppqn = snap.value("ppqn", static_cast<std::uint32_t>(0));
            }
            status.transport = snap.value("transport", "");
            status.active_pattern = snap.value("active_pattern", "");
            status.input_context = snap.value("input_context", std::string{"song"});
            status.ui_scroll_offset = snap.value("ui_scroll_offset", static_cast<std::uint32_t>(0));
            status.ui_page_offset = snap.value("ui_page_offset", static_cast<std::uint32_t>(0));
            status.ui_editing = snap.value("ui_editing", false);
            if (snap.contains("ui_confirmation") && snap["ui_confirmation"].is_object()) {
                const auto& confirmation = snap["ui_confirmation"];
                status.ui_confirmation.active = confirmation.value("active", false);
                status.ui_confirmation.kind = confirmation.value("kind", std::string{});
                status.ui_confirmation.title = confirmation.value("title", std::string{});
                status.ui_confirmation.message = confirmation.value("message", std::string{});
                status.ui_confirmation.confirm_label = confirmation.value("confirm_label", std::string{"Remove"});
                status.ui_confirmation.cancel_label = confirmation.value("cancel_label", std::string{"Cancel"});
                status.ui_confirmation.confirm_selected = confirmation.value("confirm_selected", false);
            }
            status.clock_source = snap.value("clock_source", std::string{});
            status.clock_midi_source = snap.value("clock_midi_source", std::string{});
            status.metronome_enabled = snap.value("metronome_enabled", false);
            status.metronome_alsa_device = snap.value("metronome_alsa_device", std::string{});
            if (snap.contains("chords_pad") && snap["chords_pad"].is_object()) {
                const auto& chords = snap["chords_pad"];
                status.chord_pad_right_hand_octave = std::clamp<std::uint32_t>(
                    chords.value("right_hand_octave", static_cast<std::uint32_t>(4)),
                    0U,
                    8U);
            }
            if (snap.contains("chord_pad_pressed") && snap["chord_pad_pressed"].is_array()) {
                status.chord_pad_pressed.assign(8, false);
                const auto& pads = snap["chord_pad_pressed"];
                for (std::size_t index = 0; index < pads.size() && index < status.chord_pad_pressed.size(); ++index) {
                    if (pads[index].is_boolean()) {
                        status.chord_pad_pressed[index] = pads[index].get<bool>();
                    }
                }
            }
            status.song.id = snap.value("current_song_id", std::string{});
            status.song.title = snap.value("current_song_title", std::string{});
            status.song.slot = snap.value("current_song_slot", -1);
            status.song.active_track_id = snap.value("active_track_id", std::string{});
            status.song.available = !status.song.id.empty() || !status.song.title.empty();
            if (snap.contains("active_step")) {
                status.active_step = snap.value("active_step", static_cast<std::uint32_t>(0));
            }

            if (snap.contains("bar")) {
                status.bar = snap.value("bar", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("bars_total")) {
                status.bars_total = snap.value("bars_total", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("beat")) {
                status.beat = snap.value("beat", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("beats_per_bar")) {
                status.beats_per_bar = snap.value("beats_per_bar", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("beat_unit")) {
                status.beat_unit = snap.value("beat_unit", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("active_clip_index")) {
                status.active_clip_index = snap.value("active_clip_index", static_cast<std::uint32_t>(0));
            }
            if (snap.contains("midi_in_port")) {
                status.midi_in_port = snap.value("midi_in_port", -1);
            }
            if (snap.contains("midi_in_channel")) {
                status.midi_in_channel = snap.value("midi_in_channel", -1);
            }
            if (snap.contains("midi_out_port")) {
                status.midi_out_port = snap.value("midi_out_port", -1);
            }
            if (snap.contains("midi_out_channel")) {
                status.midi_out_channel = snap.value("midi_out_channel", -1);
            }
            if (snap.contains("recording_quantize")) {
                status.recording_quantize = snap.value("recording_quantize", std::string{});
            }
            if (snap.contains("loop_quantize")) {
                status.loop_quantize = snap.value("loop_quantize", std::string{});
            }
        }

        if (status.reachable && reply.contains("data")) {
            status.summary = "status ok";
        } else if (reply.contains("error")) {
            status.summary = reply["error"].value("message", "upstream error");
        }
        return status;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<SequencerSongSummary> ControlClient::query_project(const std::string& request_id) const {
    spdlog::debug("sequencer project query endpoint={} request_id={}", endpoint_, request_id);
    constexpr int timeout_ms = 750;
    const auto reply_opt = request_control_json(endpoint_, json{{"command", "get-project"}, {"request_id", request_id}}, timeout_ms);
    if (!reply_opt.has_value()) {
        return std::nullopt;
    }

    try {
        const auto& reply = *reply_opt;
        if (!reply.value("ok", false)) {
            return std::nullopt;
        }
        if (!reply.contains("data") || !reply["data"].is_object()) {
            return std::nullopt;
        }
        const auto& data = reply["data"];
        if (!data.contains("project")) {
            return std::nullopt;
        }
        return parse_song_summary_from_project(data["project"]);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace raptor::ui
