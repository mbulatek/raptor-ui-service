#include "raptor_ui/view.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace raptor::ui {
namespace {

bool page_supported_by_display(const PageConfig& page, const DisplayConfig& display) {
    return page.allowed_models.empty() ||
           std::find(page.allowed_models.begin(), page.allowed_models.end(), display.model) != page.allowed_models.end();
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

void apply_fallback_page(UiSnapshot& snapshot, const std::string& view_id, const std::string& title) {
    snapshot.page_id = "view_" + view_id + "_fallback";
    snapshot.page_type = view_id;
    snapshot.page_title = title;
    snapshot.page_variant = "square";
    snapshot.page_image_path.clear();
    snapshot.page_image_x = 0;
    snapshot.page_image_y = 0;
}

std::vector<const PageConfig*> pages_for_view(const PageController& page_controller,
                                              const DisplayConfig& display,
                                              const std::string& view_id) {
    std::vector<const PageConfig*> pages;
    for (const auto& page : page_controller.pages()) {
        if (page.type == view_id && page_supported_by_display(page, display)) {
            pages.push_back(&page);
        }
    }
    return pages;
}

}  // namespace

View::View(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {}

std::string_view View::id() const {
    return id_;
}

bool View::apply(UiSnapshot& snapshot, const PageController& page_controller) const {
    const auto* display = page_controller.display(snapshot.display_id);
    if (display == nullptr) {
        return false;
    }

    const auto pages = pages_for_view(page_controller, *display, id_);
    snapshot.view_id = id_;
    snapshot.view_page_count = static_cast<std::uint32_t>(pages.size());
    snapshot.view_page_index = 0;

    if (pages.empty()) {
        static std::vector<std::string> reported_missing_pages;
        const std::string key = snapshot.display_id + "|" + id_;
        if (std::find(reported_missing_pages.begin(), reported_missing_pages.end(), key) == reported_missing_pages.end()) {
            reported_missing_pages.push_back(key);
            spdlog::warn(
                "ui view page missing display={} model={} view={}",
                snapshot.display_id,
                snapshot.display_model,
                id_);
        }
        apply_fallback_page(snapshot, id_, title_);
        return true;
    }

    const auto page_index = static_cast<std::uint32_t>(snapshot.sequencer.ui_page_offset % pages.size());
    snapshot.view_page_index = page_index;
    apply_page(snapshot, *pages[page_index]);
    return true;
}

ViewRegistry::ViewRegistry()
    : views_ {
          View {"song", "Song"},
          View {"track", "Track"},
          View {"chords", "Chords"},
          View {"settings", "Settings"},
      } {}

const View* ViewRegistry::find(const std::string_view id) const {
    const auto it = std::find_if(views_.begin(), views_.end(), [&](const View& view) {
        return view.id() == id;
    });
    return it == views_.end() ? nullptr : &*it;
}

bool ViewRegistry::apply_active_view(UiSnapshot& snapshot, const PageController& page_controller) const {
    const auto* view = find(snapshot.sequencer.input_context);
    if (view == nullptr) {
        snapshot.view_id.clear();
        snapshot.view_page_index = 0;
        snapshot.view_page_count = 0;
        return false;
    }
    return view->apply(snapshot, page_controller);
}

}  // namespace raptor::ui
