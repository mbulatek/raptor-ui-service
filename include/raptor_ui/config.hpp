#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace raptor::ui {

struct DisplayConfig {
    struct HardwareConfig {
        int gpiochip {0};
        int spi_device {0};
        int spi_channel {0};
        int spi_speed_hz {10'000'000};
        int spi_flags {3};  // OLEDs typically use SPI mode 3
        int cs_gpio {8};
        int reset_gpio {27};
        int dc_gpio {25};
    };

    std::string id {"main"};
    std::string driver {"waveshare"};
    std::string model {"oled_1in5"};
    std::string layout {"auto"};
    std::uint16_t rotation {0};
    std::uint32_t refresh_period_ms {250};
    bool force_clear_each_frame {false};
    HardwareConfig hardware;
};

struct PageConfig {
    struct ImageConfig {
        std::string path;
        std::uint16_t x {0};
        std::uint16_t y {0};
    };

    std::string id {"status"};
    std::string type {"status"};
    std::string title {"Status"};
    std::vector<std::string> allowed_models;
    std::string default_variant {"auto"};
    ImageConfig image;
};

struct DisplayAssignmentConfig {
    std::string display_id;
    std::string page_id;
};

struct SceneConfig {
    std::string id;
    std::string title;
    std::vector<DisplayAssignmentConfig> assignments;
};

struct IpcConfig {
    std::string ui_events_endpoint {"ipc:///run/raptor-ui/events.zmq"};
    std::string ui_control_endpoint {"ipc:///run/raptor-ui/control.zmq"};
    std::string midi_events_endpoint {"ipc:///run/raptor-engine/midi-events.zmq"};
    std::string midi_control_endpoint {"ipc:///run/raptor-engine/midi-control.zmq"};
    std::string sequencer_events_endpoint {"ipc:///run/raptor-engine/seq-events.zmq"};
    std::string sequencer_control_endpoint {"ipc:///run/raptor-engine/seq-control.zmq"};
};

struct LoggingConfig {
    std::string level {"info"};
};

struct ServiceConfig {
    std::string config_path;
    std::string ipc_config_path {"/etc/raptor/raptor-ipc.yaml"};
    std::vector<DisplayConfig> displays;
    std::vector<PageConfig> pages;
    std::vector<DisplayAssignmentConfig> assignments;
    std::vector<SceneConfig> scenes;
    std::string initial_scene;
    IpcConfig ipc;
    LoggingConfig logging;
};

ServiceConfig load_config(const std::string& path);

}  // namespace raptor::ui
