#pragma once

#include "dender_document.h"
#include "dender_view.h"

#include <affineui/affineui.h>

#include <string_view>

namespace dender {

class DenderApp {
public:
    DenderApp();

    int run();
    void reload();

private:
    [[nodiscard]] static affineui::App::Config make_config();
    [[nodiscard]] DenderActions make_actions();

    affineui::App app_;
    DenderDocument document_;
};

}  // namespace dender
