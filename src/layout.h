#pragma once

#include <windows.h>

#include <array>

namespace nekodrag::ui {

// Logical layout constants for the settings dialog (96-DPI units).
// All runtime coordinates are produced by Scale(value, dpi).
struct SettingsLayout {
    static constexpr int kBaseDpi = 96;

    static constexpr int kMinClientWidth = 480;
    static constexpr int kMinClientHeight = 500;

    static constexpr int kMargin = 24;
    static constexpr int kTopMargin = 20;
    static constexpr int kRowSpacing = 16;
    static constexpr int kSectionSpacing = 24;

    static constexpr int kCheckboxHeight = 24;
    static constexpr int kCheckboxWidth = 72;

    static constexpr int kGroupHeight = 88;
    static constexpr int kGroupCheckboxTop = 34;
    static constexpr int kGroupInsetX = 16;
    static constexpr std::array<int, 4> kModifierCheckboxX = {16, 102, 188,
                                                                 274};

    static constexpr int kDragModeGroupHeight = 88;
    static constexpr int kDragModeOptionTop = 34;
    static constexpr std::array<int, 3> kDragModeOptionX = {16, 150, 284};
    static constexpr std::array<int, 3> kDragModeOptionWidth = {120, 120,
                                                                132};

    static constexpr int kHelpHeight = 38;
    static constexpr int kStatusHeight = 32;
    static constexpr int kStatusTopSpacing = 8;

    static constexpr int kButtonWidth = 76;
    static constexpr int kButtonHeight = 28;
    static constexpr int kButtonSpacing = 12;

    static int Scale(int value, UINT dpi) {
        return MulDiv(value, static_cast<int>(dpi), kBaseDpi);
    }

    static RECT EnabledCheckbox(UINT dpi) {
        const int x = Scale(kMargin, dpi);
        const int y = Scale(kTopMargin, dpi);
        const int w = Scale(220, dpi);
        const int h = Scale(kCheckboxHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT ModifierGroup(UINT dpi, int client_width) {
        const RECT enabled = EnabledCheckbox(dpi);
        const int x = Scale(kMargin, dpi);
        const int y = enabled.bottom + Scale(kRowSpacing, dpi);
        const int w = client_width - 2 * Scale(kMargin, dpi);
        const int h = Scale(kGroupHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT ModifierCheckbox(UINT dpi, int group_left, int group_top,
                                 int index) {
        const int x = group_left + Scale(kModifierCheckboxX[index], dpi);
        const int y = group_top + Scale(kGroupCheckboxTop, dpi);
        const int w = Scale(kCheckboxWidth, dpi);
        const int h = Scale(kCheckboxHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT DragModeGroup(UINT dpi, int client_width,
                              int previous_bottom) {
        const int x = Scale(kMargin, dpi);
        const int y = previous_bottom + Scale(kSectionSpacing, dpi);
        const int w = client_width - 2 * Scale(kMargin, dpi);
        const int h = Scale(kDragModeGroupHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT DragModeOption(UINT dpi, int group_left, int group_top,
                               int index) {
        const int x = group_left + Scale(kDragModeOptionX[index], dpi);
        const int y = group_top + Scale(kDragModeOptionTop, dpi);
        const int w = Scale(kDragModeOptionWidth[index], dpi);
        const int h = Scale(kCheckboxHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT StartupCheckbox(UINT dpi, int group_bottom) {
        const int x = Scale(kMargin, dpi);
        const int y = group_bottom + Scale(kSectionSpacing, dpi);
        const int w = Scale(280, dpi);
        const int h = Scale(kCheckboxHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT HelpLabel(UINT dpi, int client_width, int previous_bottom) {
        const int x = Scale(kMargin, dpi);
        const int y = previous_bottom + Scale(kRowSpacing, dpi);
        const int w = client_width - 2 * Scale(kMargin, dpi);
        const int h = Scale(kHelpHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT StatusLabel(UINT dpi, int client_width, int help_bottom) {
        const int x = Scale(kMargin, dpi);
        const int y = help_bottom + Scale(kStatusTopSpacing, dpi);
        const int w = client_width - 2 * Scale(kMargin, dpi);
        const int h = Scale(kStatusHeight, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT SaveButton(UINT dpi, int client_width, int client_height) {
        const int h = Scale(kButtonHeight, dpi);
        const int w = Scale(kButtonWidth, dpi);
        const int y = client_height - Scale(kMargin, dpi) - h;
        const int x = client_width -
                      Scale(kMargin + 2 * kButtonWidth + kButtonSpacing, dpi);
        return {x, y, x + w, y + h};
    }

    static RECT CancelButton(UINT dpi, int client_width, int client_height) {
        const int h = Scale(kButtonHeight, dpi);
        const int w = Scale(kButtonWidth, dpi);
        const int y = client_height - Scale(kMargin, dpi) - h;
        const int x = client_width - Scale(kMargin + kButtonWidth, dpi);
        return {x, y, x + w, y + h};
    }
};

}  // namespace nekodrag::ui
