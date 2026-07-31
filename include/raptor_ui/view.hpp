#pragma once

#include "raptor_ui/ipc.hpp"
#include "raptor_ui/page_controller.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace raptor::ui {

class View {
public:
    View(std::string id, std::string title);

    std::string_view id() const;
    bool apply(UiSnapshot& snapshot, const PageController& page_controller) const;

private:
    std::string id_;
    std::string title_;
};

class ViewRegistry {
public:
    ViewRegistry();

    const View* find(std::string_view id) const;
    bool apply_active_view(UiSnapshot& snapshot, const PageController& page_controller) const;

private:
    std::vector<View> views_;
};

}  // namespace raptor::ui
