#pragma once

#include <windows.h>

#include <vector>

namespace nekodrag::ui {

// Provides theme colors/brushes and detects Windows light/dark/high-contrast
// modes. All custom colors are bypassed when a high-contrast theme is active.
class UiTheme {
  public:
    UiTheme();
    ~UiTheme();

    UiTheme(const UiTheme&) = delete;
    UiTheme& operator=(const UiTheme&) = delete;

    void Refresh();

    bool IsDark() const noexcept { return dark_; }
    bool IsHighContrast() const noexcept { return high_contrast_; }

    COLORREF BackgroundColor() const;
    COLORREF SurfaceColor() const;
    COLORREF TextColor() const;
    COLORREF SecondaryTextColor() const;
    COLORREF AccentColor() const;
    COLORREF AccentHoverColor() const;
    COLORREF BorderColor() const;
    COLORREF SuccessColor() const;
    COLORREF ErrorColor() const;

    HBRUSH BackgroundBrush() const { return background_brush_; }
    HBRUSH SurfaceBrush() const { return surface_brush_; }

  private:
    void DeleteBrushes() noexcept;

    bool dark_ = false;
    bool high_contrast_ = false;
    HBRUSH background_brush_ = nullptr;
    HBRUSH surface_brush_ = nullptr;
    std::vector<HBRUSH> stale_brushes_;
};

// Owner-draw helpers for the settings dialog controls.
void DrawThemedPushButton(const DRAWITEMSTRUCT* dis, const UiTheme& theme);
void DrawThemedCheckbox(const DRAWITEMSTRUCT* dis, const UiTheme& theme,
                        bool onSurface);
void DrawThemedRadioButton(const DRAWITEMSTRUCT* dis, const UiTheme& theme,
                           bool onSurface);
void DrawThemedGroupBox(const DRAWITEMSTRUCT* dis, const UiTheme& theme);

}  // namespace nekodrag::ui
