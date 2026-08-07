# Raptor Native UI

`raptor-native-ui` is a background UI daemon for Raspberry Pi that renders sequencer and MIDI state onto one or more small OLED displays.

## Architecture

The service is built around four concepts:

- `display`: a physical screen with its own model, GPIO and SPI wiring
- `page`: a renderer for a semantic page selected by `raptor-engine/session`
- `layout`: a geometry-aware rendering variant chosen for the physical display
- `assignment`: the current runtime binding between a display and a page

Application navigation belongs to `raptor-engine`. It publishes a
`presentation` containing stable identifiers such as `song.overview`,
`song.tracks` or `track.controllers`. Native UI maps that page identifier to a
compatible local renderer and never derives the active page from a numeric
offset. Boot, diagnostics and auxiliary-display pages can still be assigned or
swapped through the Native UI control API.

## Hardware backend

The rendering path uses the Waveshare vendor library, but the project replaces the original hard-coded `DEV_Config` with a configurable GPIO/SPI shim. Each display instance may use its own:

- GPIO chip
- SPI device and channel
- SPI speed and flags
- CS, RESET and DC GPIO lines

The current backend uses `libgpiod` for GPIO control and Linux `spidev` for SPI transfers.

Multiple displays are rendered sequentially. This is deliberate because the vendor stack keeps global drawing state.

## Configuration

The runtime YAML defines:

- `displays`: physical screens and hardware wiring
- `pages`: logical UI pages and compatibility limits
- `assignments`: current page-to-display mapping
- `scenes`: named assignment sets such as `performance`, `maintenance`, `boot`, `standby`
- `initial_scene`: scene activated during startup
- `ipc`: ZeroMQ endpoints

Supported page types in the current implementation:

- `boot`
- `transport`
- `playing`
- `recording`
- `song`
- `track`
- `settings`
- `chords`
- `midi_monitor`
- `status`

If `pages` is omitted, the service creates a default `status` page. If `assignments` is omitted, the service derives them from `initial_scene`, or from the first scene, or from the first compatible page per display.

### Page variants

Each page may set `default_variant`.

Supported values:

- `auto`
- `compact`
- `portrait_status`
- `square_status`
- `wide_status`
- aliases: `portrait`, `square`, `wide`

`default_variant` may override the display's geometry-derived base layout for that page. This lets the same physical display show different pages in different visual arrangements.

### Page images

Pages may also declare an optional monochrome BMP image:

```yaml
image:
  path: /usr/share/raptor-native-ui/images/boot-main.bmp
  x: 0
  y: 0
```

Current support is intentionally simple:

- uncompressed `1-bit BMP`
- drawn directly into the page framebuffer
- best suited for boot splash screens and logos
- if loading fails, the service falls back to the normal text rendering path

At the moment the most useful target is the `boot` page type, which will prefer the image over the textual fallback.

## Scene profiles

The example YAML now contains named scenes. Each scene is a full set of display-to-page assignments.

Built-in examples:

- `performance`
- `maintenance`
- `boot`
- `standby`

This lets you switch the whole UI personality with one control command instead of issuing several `assign-page` calls.

### Recommended scene usage

`performance`
- main screen focuses on transport
- sidecar focuses on MIDI activity
- mini shows compact stage status

`maintenance`
- all screens emphasize diagnostics and service health

`boot`
- startup-oriented pages for bring-up and recovery
- optional monochrome BMP splash on the configured boot pages

`standby`
- calm informational layout for idle operation

## IPC

The service mirrors the JSON + ZeroMQ style used by `raptor-engine`.

At startup Native UI hydrates once from the sequencer control endpoint. Runtime
state then arrives through `sequencer.state` and `sequencer.clock` on the
sequencer event endpoint. Project details are fetched only when the published
song revision changes.

### Published topics

- `ui.snapshot.<display_id>`

Example event payload fields:

- `display.id`, `display.model`, `display.layout`
- `page.id`, `page.type`, `page.title`, `page.variant`
- `page.image_path`, `page.image_x`, `page.image_y`
- `last_midi`
- `sequencer`

### Control commands

Requests are JSON objects.

`ping`
```json
{ "command": "ping", "request_id": "req-1" }
```

`status`
```json
{ "command": "status", "request_id": "req-2" }
```

`list-pages`
```json
{ "command": "list-pages", "request_id": "req-3" }
```

`list-assignments`
```json
{ "command": "list-assignments", "request_id": "req-4" }
```

`list-scenes`
```json
{ "command": "list-scenes", "request_id": "req-5" }
```

`assign-page`
```json
{
  "command": "assign-page",
  "request_id": "req-6",
  "data": {
    "display_id": "main",
    "page_id": "perf_midi_main"
  }
}
```

`swap-pages`
```json
{
  "command": "swap-pages",
  "request_id": "req-7",
  "data": {
    "display_a": "main",
    "display_b": "sidecar"
  }
}
```

`activate-scene`
```json
{
  "command": "activate-scene",
  "request_id": "req-8",
  "data": {
    "scene_id": "maintenance"
  }
}
```

`status` and `list-scenes` report the current `active_scene`. Manual page reassignment clears the active scene and moves the service into a custom assignment state until another scene is activated.

## Build

The service uses:

- `yaml-cpp`
- `nlohmann_json`
- `libzmq`
- `libgpiod`

The Windows host build is not treated as the primary target. The intended build targets are Yocto and Linux environments with the required packages installed.
