#include "raptor_ui/display_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

extern "C" {
#include "GUI_Paint.h"
#include "fonts.h"
#include "raptor_ui/waveshare_shim.h"
}

namespace raptor::ui {
namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    std::uint16_t type;
    std::uint32_t size;
    std::uint16_t reserved1;
    std::uint16_t reserved2;
    std::uint32_t offset;
};

struct BmpInfoHeader {
    std::uint32_t size;
    std::int32_t width;
    std::int32_t height;
    std::uint16_t planes;
    std::uint16_t bit_count;
    std::uint32_t compression;
    std::uint32_t image_size;
    std::int32_t x_ppm;
    std::int32_t y_ppm;
    std::uint32_t clr_used;
    std::uint32_t clr_important;
};

struct BmpPaletteEntry {
    std::uint8_t blue;
    std::uint8_t green;
    std::uint8_t red;
    std::uint8_t reserved;
};
#pragma pack(pop)

std::size_t framebuffer_bytes(const waveshare_display_descriptor_t& descriptor) {
    if (descriptor.scale == 16) {
        return static_cast<std::size_t>((descriptor.width / 2) * descriptor.height);
    }
    return static_cast<std::size_t>(((descriptor.width + 7) / 8) * descriptor.height);
}

std::string choose_layout(const DisplayConfig& config, const waveshare_display_descriptor_t& descriptor) {
    if (config.layout != "auto") {
        return config.layout;
    }
    if (descriptor.height <= 32) {
        return "compact";
    }
    if (descriptor.width <= 64 && descriptor.height >= 96) {
        return "portrait_status";
    }
    if (descriptor.width == descriptor.height) {
        return "square_status";
    }
    return "wide_status";
}

std::string resolve_page_layout(const UiSnapshot& snapshot, const std::string& display_layout) {
    const auto& variant = snapshot.page_variant;
    if (variant.empty() || variant == "auto") {
        return display_layout;
    }
    if (variant == "compact" || variant == "portrait_status" || variant == "square_status" || variant == "wide_status") {
        return variant;
    }
    if (variant == "portrait") {
        return "portrait_status";
    }
    if (variant == "square") {
        return "square_status";
    }
    if (variant == "wide") {
        return "wide_status";
    }
    return display_layout;
}

bool draw_monochrome_bmp(const UiSnapshot& snapshot) {
    if (snapshot.page_image_path.empty()) {
        return false;
    }

    std::ifstream file(snapshot.page_image_path, std::ios::binary);
    if (!file) {
        spdlog::debug("bmp open failed path={}", snapshot.page_image_path);
        return false;
    }

    BmpFileHeader file_header {};
    BmpInfoHeader info_header {};
    file.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
    file.read(reinterpret_cast<char*>(&info_header), sizeof(info_header));
    if (!file || file_header.type != 0x4D42 || info_header.size < sizeof(BmpInfoHeader)) {
        return false;
    }
    if (info_header.planes != 1 || info_header.bit_count != 1 || info_header.compression != 0) {
        return false;
    }

    BmpPaletteEntry palette[2] {};
    file.read(reinterpret_cast<char*>(palette), sizeof(palette));
    if (!file) {
        return false;
    }

    const bool top_down = info_header.height < 0;
    const int width = info_header.width;
    const int height = top_down ? -info_header.height : info_header.height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>((width + 7) / 8);
    const std::size_t padded_row_bytes = ((row_bytes + 3u) / 4u) * 4u;
    std::vector<std::uint8_t> row(padded_row_bytes, 0);

    const bool palette_zero_is_white = palette[0].red == 0xFF && palette[0].green == 0xFF && palette[0].blue == 0xFF;
    const auto set_pixel_from_bit = [&](int x, int y, bool bit_set) {
        const auto color = bit_set ? (palette_zero_is_white ? BLACK : WHITE)
                                   : (palette_zero_is_white ? WHITE : BLACK);
        Paint_SetPixel(snapshot.page_image_x + static_cast<std::uint16_t>(x), snapshot.page_image_y + static_cast<std::uint16_t>(y), color);
    };

    file.seekg(static_cast<std::streamoff>(file_header.offset), std::ios::beg);
    if (!file) {
        return false;
    }

    for (int source_row = 0; source_row < height; ++source_row) {
        file.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(padded_row_bytes));
        if (!file) {
            return false;
        }
        const int y = top_down ? source_row : (height - 1 - source_row);
        for (int x = 0; x < width; ++x) {
            const auto byte = row[static_cast<std::size_t>(x / 8)];
            const bool bit_set = (byte & (0x80u >> (x % 8))) != 0;
            set_pixel_from_bit(x, y, bit_set);
        }
    }

    return true;
}

void draw_status_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        char line1[48];
        char line2[48];
        std::snprintf(line1, sizeof(line1), "%s %s", snapshot.page_title.c_str(), snapshot.sequencer.reachable ? "SEQ" : "OFF");
        if (snapshot.last_midi.available) {
            std::snprintf(line2, sizeof(line2), "P%d %s", snapshot.last_midi.global_port, snapshot.last_midi.bytes_hex.c_str());
        } else {
            std::snprintf(line2, sizeof(line2), "waiting");
        }
        Paint_DrawString_EN(0, 0, line1, &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, line2, &Font8, WHITE, BLACK);
        return;
    }

    if (layout == "portrait_status") {
        char line1[48];
        char line2[48];
        char line3[48];
        std::snprintf(line1, sizeof(line1), "%s", snapshot.page_title.c_str());
        std::snprintf(line2, sizeof(line2), "SEQ %s", snapshot.sequencer.reachable ? "UP" : "DOWN");
        if (snapshot.last_midi.available) {
            std::snprintf(line3, sizeof(line3), "P%d %s", snapshot.last_midi.global_port, snapshot.last_midi.bytes_hex.c_str());
        } else {
            std::snprintf(line3, sizeof(line3), "MIDI idle");
        }
        Paint_DrawString_EN(0, 0, line1, &Font12, WHITE, BLACK);
        Paint_DrawString_EN(0, 20, line2, &Font12, WHITE, BLACK);
        Paint_DrawString_EN(0, 40, line3, &Font8, WHITE, BLACK);
        return;
    }

    if (layout == "wide_status") {
        char line1[64];
        char line2[64];
        char line3[64];
        std::snprintf(line1, sizeof(line1), "%s %s", snapshot.display_id.c_str(), snapshot.display_model.c_str());
        std::snprintf(line2, sizeof(line2), "SEQ: %s", snapshot.sequencer.reachable ? snapshot.sequencer.service.c_str() : "offline");
        if (snapshot.last_midi.available) {
            std::snprintf(line3, sizeof(line3), "MIDI P%d %s", snapshot.last_midi.global_port, snapshot.last_midi.bytes_hex.c_str());
        } else {
            std::snprintf(line3, sizeof(line3), "MIDI waiting for data");
        }
        waveshare_display_draw_status(snapshot.page_title.c_str(), line1, line2, line3);
        return;
    }

    char line1[64];
    char line2[64];
    char line3[64];
    std::snprintf(line1, sizeof(line1), "SEQ %s", snapshot.sequencer.reachable ? "UP" : "DOWN");
    if (snapshot.last_midi.available) {
        std::snprintf(line2, sizeof(line2), "P%d %s", snapshot.last_midi.global_port, snapshot.last_midi.bytes_hex.c_str());
    } else {
        std::snprintf(line2, sizeof(line2), "MIDI idle");
    }
    std::snprintf(line3, sizeof(line3), "%s #%llu", snapshot.display_id.c_str(), static_cast<unsigned long long>(snapshot.render_count));
    waveshare_display_draw_status(snapshot.page_title.c_str(), line1, line2, line3);
}

std::optional<std::uint8_t> parse_first_hex_byte(const std::string& hex) {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };

    int hi = -1;
    for (char c : hex) {
        const int v = hex_value(c);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
            continue;
        }
        const int lo = v;
        return static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return std::nullopt;
}

int midi_channel_from_status(std::uint8_t status) {
    if (status >= 0x80 && status <= 0xEF) {
        return static_cast<int>((status & 0x0F) + 1);
    }
    return -1;
}

int clip_index_from_active_pattern(const std::string& pattern) {
    // Expect patterns like A01/B12; fall back to 1.
    int value = 0;
    bool any = false;
    for (char c : pattern) {
        if (c >= '0' && c <= '9') {
            any = true;
            value = (value * 10) + (c - '0');
        }
    }
    if (!any || value <= 0) {
        return 1;
    }
    return value;
}

std::string trim_text(const std::string& value, std::size_t max_chars) {
    if (value.size() <= max_chars) {
        return value;
    }
    if (max_chars <= 3) {
        return value.substr(0, max_chars);
    }
    return value.substr(0, max_chars - 3) + "...";
}

std::size_t chars_for_width(const UWORD width_px, const sFONT& font) {
    if (font.Width == 0) {
        return 1;
    }
    return std::max<std::size_t>(1U, static_cast<std::size_t>(width_px / font.Width));
}

std::string scroll_text_window(const std::string& value, const std::size_t visible_chars, const std::uint64_t render_count) {
    if (visible_chars == 0 || value.size() <= visible_chars) {
        return value;
    }

    constexpr std::uint64_t frames_per_step = 2;
    constexpr std::uint64_t hold_steps = 4;
    const std::uint64_t range = static_cast<std::uint64_t>(value.size() - visible_chars);
    const std::uint64_t cycle = (hold_steps * 2U) + (range * 2U);
    const std::uint64_t phase = cycle == 0 ? 0 : ((render_count / frames_per_step) % cycle);

    std::uint64_t offset = 0;
    if (phase < hold_steps) {
        offset = 0;
    } else if (phase < hold_steps + range) {
        offset = phase - hold_steps;
    } else if (phase < (hold_steps * 2U) + range) {
        offset = range;
    } else {
        offset = range - (phase - ((hold_steps * 2U) + range));
    }

    return value.substr(static_cast<std::size_t>(offset), visible_chars);
}

std::string display_midi_port(const std::string& token, const std::string& label) {
    if (!label.empty()) {
        return label;
    }
    if (token.empty()) {
        return "-";
    }
    if (token == "-1" || token == "any") {
        return "ANY";
    }
    if (token == "auto") {
        return "-";
    }
    return token;
}

std::string display_midi_channel(const int channel) {
    if (channel == 0) {
        return "ANY";
    }
    if (channel < 0) {
        return "-";
    }
    return std::to_string(channel);
}

std::string display_clock_source(const std::string& value) {
    if (value == "sync_in") {
        return "Sync In";
    }
    if (value == "midi_clock_in") {
        return "MIDI Clock In";
    }
    return "Internal BPM";
}

std::string display_clock_midi_source(const std::string& value) {
    if (value.empty() || value == "any" || value == "-1") {
        return "Any";
    }
    return value;
}

std::string display_audio_device(const std::string& value) {
    if (value.empty()) {
        return "default";
    }
    return value;
}

std::string display_checkbox(const bool enabled) {
    return enabled ? "[x]" : "[ ]";
}

struct DisplayField {
    std::string label;
    std::string value;
};

void draw_editable_field_rows(const std::vector<DisplayField>& fields,
                              const std::size_t selected,
                              const bool editing,
                              const bool compact,
                              const UWORD label_width,
                              const std::uint64_t render_count) {
    sFONT* font = compact ? &Font8 : &Font12;
    const UWORD row_h = compact ? 11 : 18;
    const UWORD y0 = compact ? 20 : 24;
    const UWORD text_pad_y = compact ? 1 : 2;
    const std::size_t max_chars = compact ? 20U : 21U;
    const UWORD value_x = static_cast<UWORD>(label_width + 3U);
    const std::size_t label_chars = chars_for_width(label_width > 4U ? static_cast<UWORD>(label_width - 4U) : label_width, *font);
    const std::size_t value_chars = chars_for_width(value_x < 125U ? static_cast<UWORD>(125U - value_x) : 1U, *font);

    for (std::size_t i = 0; i < fields.size(); ++i) {
        const UWORD y = static_cast<UWORD>(y0 + static_cast<UWORD>(i) * row_h);
        const bool active = i == selected;
        const std::string line = fields[i].label + ": " + fields[i].value;

        if (active && editing) {
            Paint_DrawRectangle(0, y, label_width, static_cast<UWORD>(y + row_h - 1), WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            Paint_DrawString_EN(
                2,
                static_cast<UWORD>(y + text_pad_y),
                trim_text(fields[i].label, 8).c_str(),
                font,
                BLACK,
                WHITE);
            Paint_DrawRectangle(value_x, y, 127, static_cast<UWORD>(y + row_h - 1), WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            Paint_DrawString_EN(
                static_cast<UWORD>(value_x + 2U),
                static_cast<UWORD>(y + text_pad_y),
                trim_text(fields[i].value, compact ? 12U : 13U).c_str(),
                font,
                WHITE,
                BLACK);
            continue;
        }

        if (active) {
            Paint_DrawRectangle(0, y, 127, static_cast<UWORD>(y + row_h - 1), WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            Paint_DrawString_EN(
                2,
                static_cast<UWORD>(y + text_pad_y),
                trim_text(fields[i].label + ":", label_chars).c_str(),
                font,
                BLACK,
                WHITE);
            Paint_DrawString_EN(
                value_x,
                static_cast<UWORD>(y + text_pad_y),
                scroll_text_window(fields[i].value, value_chars, render_count).c_str(),
                font,
                BLACK,
                WHITE);
            continue;
        }

        Paint_DrawString_EN(
            2,
            static_cast<UWORD>(y + text_pad_y),
            trim_text(line, max_chars).c_str(),
            font,
            active ? BLACK : WHITE,
            active ? WHITE : BLACK);
    }
}

std::size_t scroll_start_index(const std::uint32_t offset, const std::size_t total, const std::size_t visible) {
    if (total <= visible) {
        return 0;
    }
    return std::min<std::size_t>(static_cast<std::size_t>(offset), total - visible);
}

int parse_midi_port_token(const std::string& value) {
    if (value.empty()) {
        return -1;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed, 10);
        if (consumed == value.size()) {
            return parsed;
        }
    } catch (...) {
    }

    int parsed = -1;
    for (char c : value) {
        if (c >= '0' && c <= '9') {
            if (parsed < 0) {
                parsed = c - '0';
            } else {
                parsed = (parsed * 10) + (c - '0');
            }
        } else if (parsed >= 0) {
            break;
        }
    }
    return parsed;
}

const SequencerTrackSummary* find_focused_track(const UiSnapshot& snapshot) {
    if (!snapshot.sequencer.song.available || snapshot.sequencer.song.tracks.empty()) {
        return nullptr;
    }

    if (!snapshot.sequencer.song.active_track_id.empty()) {
        for (const auto& track : snapshot.sequencer.song.tracks) {
            if (track.id == snapshot.sequencer.song.active_track_id) {
                return &track;
            }
        }
    }

    const auto out_port = snapshot.sequencer.midi_out_port.value_or(-1);
    const auto out_ch = snapshot.sequencer.midi_out_channel.value_or(-1);
    if (out_port >= 0) {
        for (const auto& track : snapshot.sequencer.song.tracks) {
            const int track_out_port = parse_midi_port_token(track.midi_out);
            if (track_out_port == out_port && (out_ch <= 0 || track.midi_channel_out == out_ch)) {
                return &track;
            }
        }
    }

    const auto in_port = snapshot.sequencer.midi_in_port.value_or(-1);
    const auto in_ch = snapshot.sequencer.midi_in_channel.value_or(-1);
    if (in_port >= 0) {
        for (const auto& track : snapshot.sequencer.song.tracks) {
            const int track_in_port = parse_midi_port_token(track.midi_in);
            if (track_in_port == in_port && (in_ch <= 0 || track.midi_channel_in == in_ch)) {
                return &track;
            }
        }
    }

    return &snapshot.sequencer.song.tracks.front();
}

void draw_song_page(const UiSnapshot& snapshot, const std::string& /*layout*/) {
    const bool tracks_variant =
        snapshot.page_variant == "tracks" ||
        snapshot.page_variant == "song_tracks";

    const std::string title = snapshot.sequencer.song.available && !snapshot.sequencer.song.title.empty()
        ? snapshot.sequencer.song.title
        : snapshot.page_title;
    Paint_DrawString_EN(0, 0, trim_text(title, 18).c_str(), &Font12, WHITE, BLACK);
    Paint_DrawLine(0, 16, 127, 16, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    if (!tracks_variant) {
        char bpm_value[24];
        char length_value[24];
        char signature_value[24];
        std::snprintf(bpm_value, sizeof(bpm_value), "%.0f BPM", snapshot.sequencer.bpm.value_or(120.0));
        std::snprintf(length_value, sizeof(length_value), "%u bars", snapshot.sequencer.bars_total.value_or(8U));
        std::snprintf(
            signature_value,
            sizeof(signature_value),
            "%u/%u",
            snapshot.sequencer.beats_per_bar.value_or(4U),
            snapshot.sequencer.beat_unit.value_or(4U));

        std::vector<DisplayField> fields {
            DisplayField {"Tempo", bpm_value},
            DisplayField {"Length", length_value},
            DisplayField {"Signature", signature_value},
        };
        const std::size_t selected = fields.empty()
            ? 0U
            : static_cast<std::size_t>(snapshot.sequencer.presentation.focus_index % fields.size());
        draw_editable_field_rows(
            fields,
            selected,
            snapshot.sequencer.presentation.editing,
            false,
            58U,
            snapshot.render_count);
        return;
    }

    if (!snapshot.sequencer.song.available || snapshot.sequencer.song.tracks.empty()) {
        Paint_DrawString_EN(0, 28, "No tracks", &Font12, WHITE, BLACK);
        return;
    }

    const std::size_t max_rows = 7;
    std::size_t selected = snapshot.sequencer.song.tracks.empty()
        ? 0U
        : static_cast<std::size_t>(snapshot.sequencer.presentation.focus_index % snapshot.sequencer.song.tracks.size());
    if (!snapshot.sequencer.song.active_track_id.empty()) {
        for (std::size_t i = 0; i < snapshot.sequencer.song.tracks.size(); ++i) {
            if (snapshot.sequencer.song.tracks[i].id == snapshot.sequencer.song.active_track_id) {
                selected = i;
                break;
            }
        }
    }
    const std::size_t first = scroll_start_index(static_cast<std::uint32_t>(selected), snapshot.sequencer.song.tracks.size(), max_rows);
    if (snapshot.sequencer.song.tracks.size() > max_rows) {
        char pos[32];
        std::snprintf(pos, sizeof(pos), "%zu/%zu", selected + 1U, snapshot.sequencer.song.tracks.size());
        Paint_DrawString_EN(94, 3, pos, &Font8, WHITE, BLACK);
    }
    for (std::size_t row = 0; row < max_rows && first + row < snapshot.sequencer.song.tracks.size(); ++row) {
        const auto& track = snapshot.sequencer.song.tracks[first + row];
        const bool active = first + row == selected;
        const int y = 20 + static_cast<int>(row * 15);
        if (active) {
            Paint_DrawRectangle(0, static_cast<UWORD>(y - 1), 127, static_cast<UWORD>(y + 11), WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }

        const std::string name = trim_text(track.name.empty() ? track.id : track.name, 10);
        Paint_DrawString_EN(2, static_cast<UWORD>(y), active ? ">" : " ", &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
        Paint_DrawString_EN(12, static_cast<UWORD>(y), name.c_str(), &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
        Paint_DrawString_EN(104, static_cast<UWORD>(y), track.muted ? "[M]" : "[ ]", &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
    }
}

void draw_track_page(const UiSnapshot& snapshot, const std::string& layout) {
    const auto* track = find_focused_track(snapshot);
    const std::string fallback_title = snapshot.page_title.empty() ? std::string {"Track"} : snapshot.page_title;
    const std::string track_title = track != nullptr
        ? (track->name.empty() ? track->id : track->name)
        : fallback_title;

    Paint_DrawString_EN(0, 0, trim_text(track_title, 18).c_str(), &Font12, WHITE, BLACK);
    Paint_DrawLine(0, 16, 127, 16, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    const bool controller_lanes_variant =
        snapshot.page_variant == "controller_lanes" ||
        snapshot.page_variant == "track_controller_lanes";
    if (controller_lanes_variant) {
        if (track == nullptr || track->controller_lanes.empty()) {
            Paint_DrawString_EN(0, 28, "No CC lanes", &Font12, WHITE, BLACK);
            return;
        }

        const auto& lanes = track->controller_lanes;
        const std::size_t max_rows = layout == "compact" || snapshot.display_height <= 64 ? 3U : 7U;
        const std::size_t selected = static_cast<std::size_t>(snapshot.sequencer.presentation.focus_index % lanes.size());
        const std::size_t first = scroll_start_index(static_cast<std::uint32_t>(selected), lanes.size(), max_rows);
        if (lanes.size() > max_rows) {
            char pos[32];
            std::snprintf(pos, sizeof(pos), "%zu/%zu", selected + 1U, lanes.size());
            Paint_DrawString_EN(94, 3, pos, &Font8, WHITE, BLACK);
        }

        for (std::size_t row = 0; row < max_rows && first + row < lanes.size(); ++row) {
            const auto& lane = lanes[first + row];
            const bool active = first + row == selected;
            const int y = 20 + static_cast<int>(row * 15);
            if (active) {
                Paint_DrawRectangle(0, static_cast<UWORD>(y - 1), 127, static_cast<UWORD>(y + 11), WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            }

            const std::string label = trim_text(lane.label.empty() ? lane.key : lane.label, 9);
            char count[12];
            std::snprintf(count, sizeof(count), "%u", lane.event_count);
            Paint_DrawString_EN(2, static_cast<UWORD>(y), active ? ">" : " ", &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
            Paint_DrawString_EN(12, static_cast<UWORD>(y), label.c_str(), &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
            Paint_DrawString_EN(78, static_cast<UWORD>(y), count, &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
            Paint_DrawString_EN(104, static_cast<UWORD>(y), lane.muted ? "[M]" : "[ ]", &Font8, active ? BLACK : WHITE, active ? WHITE : BLACK);
        }
        return;
    }

    std::string midi_in = "-";
    std::string midi_out = "-";
    std::string midi_in_label;
    std::string midi_out_label;
    int ch_in = -1;
    int ch_out = -1;
    bool send_sync_enabled = false;
    if (track != nullptr) {
        midi_in = track->midi_in.empty() ? "-" : track->midi_in;
        midi_out = track->midi_out.empty() ? "-" : track->midi_out;
        midi_in_label = track->midi_in_label;
        midi_out_label = track->midi_out_label;
        ch_in = track->midi_channel_in;
        ch_out = track->midi_channel_out;
        send_sync_enabled = track->send_sync_enabled;
    } else {
        if (snapshot.sequencer.midi_in_port.has_value()) midi_in = std::to_string(*snapshot.sequencer.midi_in_port);
        if (snapshot.sequencer.midi_out_port.has_value()) midi_out = std::to_string(*snapshot.sequencer.midi_out_port);
        ch_in = snapshot.sequencer.midi_in_channel.value_or(-1);
        ch_out = snapshot.sequencer.midi_out_channel.value_or(-1);
    }

    std::vector<DisplayField> fields {
        DisplayField {"In", display_midi_port(midi_in, midi_in_label)},
        DisplayField {"Out", display_midi_port(midi_out, midi_out_label)},
        DisplayField {"Ch In", display_midi_channel(ch_in)},
        DisplayField {"Ch Out", display_midi_channel(ch_out)},
        DisplayField {"Send sync", display_checkbox(send_sync_enabled)},
    };

    const bool compact = layout == "compact" || snapshot.display_height <= 64;
    const std::size_t selected = fields.empty()
        ? 0U
        : static_cast<std::size_t>(snapshot.sequencer.presentation.focus_index % fields.size());
    draw_editable_field_rows(
        fields,
        selected,
        snapshot.sequencer.presentation.editing,
        compact,
        compact ? 34U : 45U,
        snapshot.render_count);
}

void draw_settings_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        Paint_DrawString_EN(0, 0, "Settings", &Font8, WHITE, BLACK);
        const std::string clock = snapshot.sequencer.clock_source.empty() ? "internal" : snapshot.sequencer.clock_source;
        Paint_DrawString_EN(0, 12, trim_text(display_clock_source(clock), 16).c_str(), &Font8, WHITE, BLACK);
        return;
    }

    Paint_DrawString_EN(0, 0, "Settings", &Font12, WHITE, BLACK);
    Paint_DrawLine(0, 16, 127, 16, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    const std::string clock = snapshot.sequencer.clock_source.empty() ? "internal" : snapshot.sequencer.clock_source;
    const std::string midi_source = snapshot.sequencer.clock_midi_source.empty() ? "any" : snapshot.sequencer.clock_midi_source;
    char ppqn_value[16];
    std::snprintf(ppqn_value, sizeof(ppqn_value), "%u", snapshot.sequencer.ppqn.value_or(24U));
    const std::string loop_quantize = snapshot.sequencer.loop_quantize.empty() ? "1/16" : snapshot.sequencer.loop_quantize;

    std::vector<DisplayField> fields {
        DisplayField {"Clock", display_clock_source(clock)},
        DisplayField {"MIDI", display_clock_midi_source(midi_source)},
        DisplayField {"PPQN", ppqn_value},
        DisplayField {"Loop Q", loop_quantize},
        DisplayField {"Sound", display_audio_device(snapshot.sequencer.metronome_alsa_device)},
    };

    const bool compact = snapshot.display_height <= 64;
    const std::size_t selected = fields.empty()
        ? 0U
        : static_cast<std::size_t>(snapshot.sequencer.presentation.focus_index % fields.size());
    draw_editable_field_rows(
        fields,
        selected,
        snapshot.sequencer.presentation.editing,
        compact,
        compact ? 40U : 50U,
        snapshot.render_count);
}

void draw_recording_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        Paint_DrawString_EN(0, 0, "REC", &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, snapshot.last_midi.available ? snapshot.last_midi.bytes_hex.c_str() : "NO MIDI", &Font8, WHITE, BLACK);
        return;
    }

    const int clip_index = snapshot.sequencer.active_clip_index.has_value() && *snapshot.sequencer.active_clip_index > 0
                               ? static_cast<int>(*snapshot.sequencer.active_clip_index)
                               : clip_index_from_active_pattern(snapshot.sequencer.active_pattern);

    const auto status_byte = snapshot.last_midi.available ? parse_first_hex_byte(snapshot.last_midi.bytes_hex) : std::nullopt;
    const int midi_ch = status_byte.has_value() ? midi_channel_from_status(*status_byte) : -1;

    // Prefer sequencer-derived musical position when available; otherwise fall back to prototype mapping.
    const std::uint64_t tick = snapshot.sequencer.tick.value_or(0);
    const std::uint32_t step = snapshot.sequencer.active_step.value_or(static_cast<std::uint32_t>(tick % 16ULL));

    const std::uint32_t bar = snapshot.sequencer.bar.value_or(static_cast<std::uint32_t>((tick / 16ULL) + 1ULL));
    const std::uint32_t bars_total = snapshot.sequencer.bars_total.value_or(8U);
    const std::uint32_t beat = snapshot.sequencer.beat.value_or(static_cast<std::uint32_t>((step / 4U) + 1U));
    const std::uint32_t beats_per_bar = snapshot.sequencer.beats_per_bar.value_or(4U);

    char clip_line[32];
    char in_line[48];
    char out_line[48];
    char bar_line[32];
    char beat_line[32];

    std::snprintf(clip_line, sizeof(clip_line), "Clip %d", clip_index);

    const int in_port = snapshot.sequencer.midi_in_port.value_or(-1);
    const int in_ch = snapshot.sequencer.midi_in_channel.value_or(-1);
    if (in_port >= 0) {
        if (in_ch > 0) {
            std::snprintf(in_line, sizeof(in_line), "IN  P%d  CH%d", in_port, in_ch);
        } else {
            std::snprintf(in_line, sizeof(in_line), "IN  P%d", in_port);
        }
    } else if (snapshot.last_midi.available) {
        if (midi_ch > 0) {
            std::snprintf(in_line, sizeof(in_line), "IN  P%d  CH%d", snapshot.last_midi.global_port, midi_ch);
        } else {
            std::snprintf(in_line, sizeof(in_line), "IN  P%d", snapshot.last_midi.global_port);
        }
    } else {
        std::snprintf(in_line, sizeof(in_line), "IN  ---");
    }

    const int out_port = snapshot.sequencer.midi_out_port.value_or(-1);
    const int out_ch = snapshot.sequencer.midi_out_channel.value_or(-1);
    if (out_port >= 0) {
        if (out_ch > 0) {
            std::snprintf(out_line, sizeof(out_line), "OUT P%d  CH%d", out_port, out_ch);
        } else {
            std::snprintf(out_line, sizeof(out_line), "OUT P%d", out_port);
        }
    } else {
        std::snprintf(out_line, sizeof(out_line), "OUT ---");
    }

    std::snprintf(bar_line, sizeof(bar_line), "Bar: %u/%u", bar, bars_total);
    std::snprintf(beat_line, sizeof(beat_line), "Beat: %u/%u", beat, beats_per_bar);

    Paint_DrawString_EN(0, 0, snapshot.page_title.c_str(), &Font12, WHITE, BLACK);
    Paint_DrawString_EN(0, 18, clip_line, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(0, 44, in_line, &Font12, WHITE, BLACK);
    Paint_DrawString_EN(0, 60, out_line, &Font12, WHITE, BLACK);
    Paint_DrawString_EN(0, 84, bar_line, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(0, 108, beat_line, &Font16, WHITE, BLACK);
}

void draw_playing_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        Paint_DrawString_EN(0, 0, "PLAY", &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, snapshot.last_midi.available ? snapshot.last_midi.bytes_hex.c_str() : "NO MIDI", &Font8, WHITE, BLACK);
        return;
    }
    draw_recording_page(snapshot, layout);
}

void draw_midi_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        char line1[48];
        char line2[48];
        std::snprintf(line1, sizeof(line1), "%s", snapshot.page_title.c_str());
        std::snprintf(line2, sizeof(line2), "%s", snapshot.last_midi.available ? snapshot.last_midi.bytes_hex.c_str() : "NO MIDI");
        Paint_DrawString_EN(0, 0, line1, &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, line2, &Font8, WHITE, BLACK);
        return;
    }

    char line1[64];
    char line2[64];
    std::snprintf(line1, sizeof(line1), "PORT %d", snapshot.last_midi.global_port);
    std::snprintf(line2, sizeof(line2), "%s", snapshot.last_midi.available ? snapshot.last_midi.bytes_hex.c_str() : "NO MIDI");
    Paint_DrawString_EN(0, 0, snapshot.page_title.c_str(), &Font12, WHITE, BLACK);
    Paint_DrawString_EN(0, 20, line1, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(0, 44, line2, &Font12, WHITE, BLACK);
}

void draw_transport_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (layout == "compact") {
        char line1[48];
        char line2[48];
        std::snprintf(line1, sizeof(line1), "%s", snapshot.page_title.c_str());
        std::snprintf(line2, sizeof(line2), "%s", snapshot.sequencer.reachable ? snapshot.sequencer.summary.c_str() : "SEQ OFF");
        Paint_DrawString_EN(0, 0, line1, &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, line2, &Font8, WHITE, BLACK);
        return;
    }

    char line1[64];
    char line2[64];
    char line3[64];
    std::snprintf(line1, sizeof(line1), "%s", snapshot.sequencer.reachable ? "Sequencer online" : "Sequencer offline");
    std::snprintf(line2, sizeof(line2), "%s", snapshot.sequencer.summary.c_str());
    std::snprintf(line3, sizeof(line3), "Last MIDI: %s", snapshot.last_midi.available ? snapshot.last_midi.bytes_hex.c_str() : "idle");
    waveshare_display_draw_status(snapshot.page_title.c_str(), line1, line2, line3);
}

void draw_boot_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (draw_monochrome_bmp(snapshot)) {
        spdlog::debug("boot bmp drawn display={} path={}", snapshot.display_id, snapshot.page_image_path);
        return;
    }

    if (layout == "compact") {
        Paint_DrawString_EN(0, 0, "Raptor UI", &Font8, WHITE, BLACK);
        Paint_DrawString_EN(0, 12, snapshot.display_id.c_str(), &Font8, WHITE, BLACK);
        return;
    }

    char line1[64];
    char line2[64];
    char line3[64];
    std::snprintf(line1, sizeof(line1), "Display %s", snapshot.display_id.c_str());
    std::snprintf(line2, sizeof(line2), "%s", snapshot.display_model.c_str());
    std::snprintf(line3, sizeof(line3), "%s", snapshot.sequencer.reachable ? "ready" : "waiting for sequencer");
    waveshare_display_draw_status(snapshot.page_title.c_str(), line1, line2, line3);
}

void draw_chords_page(const UiSnapshot& snapshot, const std::string& layout) {
    const int width = static_cast<int>(snapshot.display_width == 0 ? 128 : snapshot.display_width);
    const int height = static_cast<int>(snapshot.display_height == 0 ? 128 : snapshot.display_height);
    const bool compact = layout == "compact" || height <= 64;
    const std::string title = "Chords pad";
    char octave_label[16];
    std::snprintf(
        octave_label,
        sizeof(octave_label),
        "Oct %u",
        static_cast<unsigned int>(snapshot.sequencer.chord_pad_right_hand_octave));

    if (compact) {
        Paint_DrawString_EN(0, 0, title.c_str(), &Font8, WHITE, BLACK);
        Paint_DrawString_EN(static_cast<UWORD>(std::max(0, width - 32)), 0, octave_label, &Font8, WHITE, BLACK);
    } else {
        Paint_DrawString_EN(0, 0, title.c_str(), &Font12, WHITE, BLACK);
        Paint_DrawString_EN(static_cast<UWORD>(std::max(0, width - 32)), 3, octave_label, &Font8, WHITE, BLACK);
        Paint_DrawLine(0, 16, static_cast<UWORD>(std::max(0, width - 1)), 16, WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    }

    const int margin = compact ? 2 : 7;
    const int gap = compact ? 2 : 5;
    const int grid_top_min = compact ? 12 : 25;
    const int available_width = std::max(1, width - (margin * 2));
    const int available_height = std::max(1, height - grid_top_min - margin);
    const int cell_by_width = (available_width - (gap * 3)) / 4;
    const int cell_by_height = (available_height - gap) / 2;
    const int cell = std::max(4, std::min(cell_by_width, cell_by_height));
    const int grid_width = (cell * 4) + (gap * 3);
    const int grid_height = (cell * 2) + gap;
    const int grid_x = std::max(0, (width - grid_width) / 2);
    const int grid_y = std::max(grid_top_min, grid_top_min + ((available_height - grid_height) / 2));

    for (int index = 0; index < 8; ++index) {
        const int row = index / 4;
        const int col = index % 4;
        const int x = grid_x + (col * (cell + gap));
        const int y = grid_y + (row * (cell + gap));
        const bool pressed =
            static_cast<std::size_t>(index) < snapshot.sequencer.chord_pad_pressed.size() &&
            snapshot.sequencer.chord_pad_pressed[static_cast<std::size_t>(index)];

        Paint_DrawRectangle(
            static_cast<UWORD>(x),
            static_cast<UWORD>(y),
            static_cast<UWORD>(std::min(width - 1, x + cell - 1)),
            static_cast<UWORD>(std::min(height - 1, y + cell - 1)),
            WHITE,
            DOT_PIXEL_1X1,
            pressed ? DRAW_FILL_FULL : DRAW_FILL_EMPTY);
    }
}

void draw_confirmation_button(const int x,
                              const int y,
                              const int w,
                              const int h,
                              const std::string& label,
                              const bool selected) {
    const UWORD fg = selected ? BLACK : WHITE;
    const UWORD bg = selected ? WHITE : BLACK;
    Paint_DrawRectangle(
        static_cast<UWORD>(x),
        static_cast<UWORD>(y),
        static_cast<UWORD>(x + w),
        static_cast<UWORD>(y + h),
        WHITE,
        DOT_PIXEL_1X1,
        selected ? DRAW_FILL_FULL : DRAW_FILL_EMPTY);
    Paint_DrawString_EN(
        static_cast<UWORD>(x + 4),
        static_cast<UWORD>(y + 5),
        trim_text(label, static_cast<std::size_t>(std::max(1, (w - 8) / 7))).c_str(),
        &Font8,
        fg,
        bg);
}

void draw_confirmation_popup(const UiSnapshot& snapshot) {
    const auto& confirmation = snapshot.sequencer.presentation.confirmation;
    if (!confirmation.active) {
        return;
    }

    const int x0 = snapshot.display_width <= 96 ? 2 : 6;
    const int display_height = static_cast<int>(snapshot.display_height == 0 ? 128 : snapshot.display_height);
    const bool compact = display_height <= 64;
    const int y0 = compact ? 4 : 16;
    const int x1 = static_cast<int>(snapshot.display_width == 0 ? 128 : snapshot.display_width) - x0 - 1;
    const int y1 = display_height - y0 - 1;

    Paint_DrawRectangle(
        static_cast<UWORD>(x0),
        static_cast<UWORD>(y0),
        static_cast<UWORD>(x1),
        static_cast<UWORD>(y1),
        BLACK,
        DOT_PIXEL_1X1,
        DRAW_FILL_FULL);
    Paint_DrawRectangle(
        static_cast<UWORD>(x0),
        static_cast<UWORD>(y0),
        static_cast<UWORD>(x1),
        static_cast<UWORD>(y1),
        WHITE,
        DOT_PIXEL_1X1,
        DRAW_FILL_EMPTY);

    const std::string title = confirmation.title.empty() ? std::string {"Confirm"} : confirmation.title;
    const std::string message = confirmation.message.empty() ? std::string {"Remove selected item"} : confirmation.message;
    Paint_DrawString_EN(static_cast<UWORD>(x0 + 6), static_cast<UWORD>(y0 + 8), trim_text(title, 16).c_str(), &Font12, WHITE, BLACK);
    Paint_DrawLine(
        static_cast<UWORD>(x0 + 4),
        static_cast<UWORD>(y0 + 24),
        static_cast<UWORD>(x1 - 4),
        static_cast<UWORD>(y0 + 24),
        WHITE,
        DOT_PIXEL_1X1,
        LINE_STYLE_SOLID);
    if (!compact) {
        Paint_DrawString_EN(static_cast<UWORD>(x0 + 6), static_cast<UWORD>(y0 + 34), trim_text(message, 18).c_str(), &Font8, WHITE, BLACK);
    }

    const int button_h = compact ? 14 : 18;
    const int button_gap = compact ? 2 : 4;
    const int button_w = std::max(44, x1 - x0 - 12);
    const int first_button_y = y1 - (button_h * 2) - button_gap - (compact ? 4 : 6);
    const int button_x = x0 + 6;
    draw_confirmation_button(
        button_x,
        first_button_y,
        button_w,
        button_h,
        confirmation.cancel_label.empty() ? "Cancel" : confirmation.cancel_label,
        !confirmation.confirm_selected);
    draw_confirmation_button(
        button_x,
        first_button_y + button_h + button_gap,
        button_w,
        button_h,
        confirmation.confirm_label.empty() ? "Remove" : confirmation.confirm_label,
        confirmation.confirm_selected);
}

void draw_page(const UiSnapshot& snapshot, const std::string& layout) {
    if (snapshot.page_type == "boot") {
        draw_boot_page(snapshot, layout);
    } else if (snapshot.page_type == "song") {
        draw_song_page(snapshot, layout);
    } else if (snapshot.page_type == "track") {
        draw_track_page(snapshot, layout);
    } else if (snapshot.page_type == "settings") {
        draw_settings_page(snapshot, layout);
    } else if (snapshot.page_type == "chords") {
        draw_chords_page(snapshot, layout);
    } else if (snapshot.page_type == "transport") {
        draw_transport_page(snapshot, layout);
    } else if (snapshot.page_type == "midi_monitor") {
        draw_midi_page(snapshot, layout);
    } else if (snapshot.page_type == "playing") {
        draw_playing_page(snapshot, layout);
    } else if (snapshot.page_type == "recording") {
        draw_recording_page(snapshot, layout);
    } else {
        draw_status_page(snapshot, layout);
    }
}

class WaveshareDisplayBackend final : public DisplayBackend {
public:
    explicit WaveshareDisplayBackend(DisplayConfig config)
        : config_(std::move(config)) {
        descriptor_ = waveshare_display_get_descriptor(config_.model.c_str());
        if (descriptor_ == nullptr) {
            throw std::runtime_error("Unsupported Waveshare display model: " + config_.model);
        }
        effective_layout_ = choose_layout(config_, *descriptor_);
        framebuffer_.resize(framebuffer_bytes(*descriptor_));
    }

    void initialize() override {
        spdlog::debug("display init id={} model={} spi={} speed_hz={}", config_.id, config_.model, config_.hardware.spi_device, config_.hardware.spi_speed_hz);
        spdlog::debug("display init step=apply_hardware id={}", config_.id);
        apply_hardware();
        spdlog::debug("display init step=waveshare_display_initialize begin id={}", config_.id);
        const int rc = waveshare_display_initialize(config_.model.c_str());
        spdlog::debug("display init step=waveshare_display_initialize end id={} rc={}", config_.id, rc);
        if (rc != 0) {
            throw std::runtime_error("Failed to initialize Waveshare display backend");
        }
        spdlog::debug("display init step=clear begin id={}", config_.id);
        clear();
        spdlog::debug("display init step=clear end id={}", config_.id);
    }

    void render(const UiSnapshot& snapshot) override {
        spdlog::trace("display render id={} page_type={}", config_.id, snapshot.page_type);
        apply_hardware();
        if (config_.force_clear_each_frame) {
            spdlog::trace("display render id={} force_clear_each_frame=1", config_.id);
            waveshare_display_clear_panel(config_.model.c_str());
        }
        waveshare_display_prepare_frame(config_.model.c_str(), framebuffer_.data(), config_.rotation);
        const auto resolved_layout = resolve_page_layout(snapshot, effective_layout_);
        draw_page(snapshot, resolved_layout);
        draw_confirmation_popup(snapshot);
        waveshare_display_present(config_.model.c_str(), framebuffer_.data());
    }

    void clear() override {
        apply_hardware();
        std::fill(framebuffer_.begin(), framebuffer_.end(), 0x00);
        waveshare_display_clear_panel(config_.model.c_str());
    }

    std::uint16_t width() const override { return descriptor_->width; }
    std::uint16_t height() const override { return descriptor_->height; }
    const std::string& id() const override { return config_.id; }
    const std::string& model() const override { return config_.model; }
    const std::string& layout() const override { return effective_layout_; }

private:
    void apply_hardware() const {
        const waveshare_hardware_config_t hw = {
            .gpiochip = config_.hardware.gpiochip,
            .spi_device = config_.hardware.spi_device,
            .spi_channel = config_.hardware.spi_channel,
            .spi_speed_hz = config_.hardware.spi_speed_hz,
            .spi_flags = config_.hardware.spi_flags,
            .cs_gpio = config_.hardware.cs_gpio,
            .reset_gpio = config_.hardware.reset_gpio,
            .dc_gpio = config_.hardware.dc_gpio,
        };
        waveshare_display_set_hardware_config(&hw);
    }

    DisplayConfig config_;
    const waveshare_display_descriptor_t* descriptor_ {nullptr};
    std::string effective_layout_;
    std::vector<std::uint8_t> framebuffer_;
};

}  // namespace

std::unique_ptr<DisplayBackend> DisplayBackend::create(const DisplayConfig& config) {
    if (config.driver == "waveshare") {
        return std::make_unique<WaveshareDisplayBackend>(config);
    }
    throw std::runtime_error("Unsupported display driver: " + config.driver);
}

}  // namespace raptor::ui
