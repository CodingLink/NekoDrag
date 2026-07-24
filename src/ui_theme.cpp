#include "ui_theme.h"

#include <algorithm>
#include <windows.h>
#include <windowsx.h>

#include <string>

namespace superdrag::ui {
namespace {

// Windows 10 1809+ attribute for dark title bar.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif

constexpr COLORREF kLightBackground = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kLightSurface = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kLightText = RGB(0x1A, 0x1A, 0x1A);
constexpr COLORREF kLightSecondaryText = RGB(0x5C, 0x5C, 0x5C);
constexpr COLORREF kLightAccent = RGB(0x00, 0x78, 0xD4);
constexpr COLORREF kLightAccentHover = RGB(0x10, 0x6E, 0xBE);
constexpr COLORREF kLightBorder = RGB(0xE0, 0xE0, 0xE0);
constexpr COLORREF kLightSuccess = RGB(0x10, 0x7C, 0x10);
constexpr COLORREF kLightError = RGB(0xD1, 0x34, 0x38);

constexpr COLORREF kDarkBackground = RGB(0x20, 0x20, 0x20);
constexpr COLORREF kDarkSurface = RGB(0x2D, 0x2D, 0x2D);
constexpr COLORREF kDarkText = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kDarkSecondaryText = RGB(0xA0, 0xA0, 0xA0);
constexpr COLORREF kDarkAccent = RGB(0x4C, 0xC2, 0xFF);
constexpr COLORREF kDarkAccentHover = RGB(0x60, 0xCD, 0xFF);
constexpr COLORREF kDarkBorder = RGB(0x3C, 0x3C, 0x3C);
constexpr COLORREF kDarkSuccess = RGB(0x54, 0xB0, 0x54);
constexpr COLORREF kDarkError = RGB(0xFF, 0x7B, 0x7B);

int ScaleDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

bool QueryAppsUseLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return value != 0;
}

bool QueryHighContrast() {
    HIGHCONTRASTW info{};
    info.cbSize = sizeof(info);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(info), &info, 0)) {
        return false;
    }
    return (info.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

void DrawCheckmark(HDC hdc, const RECT& box, UINT dpi) {
    const int pen_width = std::max(1, ScaleDpi(2, dpi));
    HPEN pen = CreatePen(PS_SOLID, pen_width, RGB(0xFF, 0xFF, 0xFF));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));

    const int pad_x = (box.right - box.left) / 5;
    const int pad_y = (box.bottom - box.top) / 5;
    POINT pts[3] = {
        {box.left + pad_x, box.top + (box.bottom - box.top) / 2},
        {box.left + (box.right - box.left) / 2,
         box.bottom - pad_y - pen_width / 2},
        {box.right - pad_x, box.top + pad_y}};
    Polyline(hdc, pts, 3);

    SelectObject(hdc, old_pen);
    DeleteObject(pen);
}

}  // namespace

UiTheme::UiTheme() {
    Refresh();
}

UiTheme::~UiTheme() {
    DeleteBrushes();
}

void UiTheme::Refresh() {
    dark_ = !QueryAppsUseLightTheme();
    high_contrast_ = QueryHighContrast();

    // Move the current brushes to the stale list so they remain valid if the
    // window is still painting with them; they are deleted in the destructor.
    if (background_brush_ != nullptr) {
        stale_brushes_.push_back(background_brush_);
    }
    if (surface_brush_ != nullptr) {
        stale_brushes_.push_back(surface_brush_);
    }

    background_brush_ = CreateSolidBrush(BackgroundColor());
    surface_brush_ = CreateSolidBrush(SurfaceColor());
}

void UiTheme::DeleteBrushes() noexcept {
    if (background_brush_ != nullptr) {
        DeleteObject(background_brush_);
        background_brush_ = nullptr;
    }
    if (surface_brush_ != nullptr) {
        DeleteObject(surface_brush_);
        surface_brush_ = nullptr;
    }
    for (HBRUSH brush : stale_brushes_) {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
    }
    stale_brushes_.clear();
}

COLORREF UiTheme::BackgroundColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_WINDOW);
    }
    return dark_ ? kDarkBackground : kLightBackground;
}

COLORREF UiTheme::SurfaceColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_BTNFACE);
    }
    return dark_ ? kDarkSurface : kLightSurface;
}

COLORREF UiTheme::TextColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_WINDOWTEXT);
    }
    return dark_ ? kDarkText : kLightText;
}

COLORREF UiTheme::SecondaryTextColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_GRAYTEXT);
    }
    return dark_ ? kDarkSecondaryText : kLightSecondaryText;
}

COLORREF UiTheme::AccentColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_HIGHLIGHT);
    }
    return dark_ ? kDarkAccent : kLightAccent;
}

COLORREF UiTheme::AccentHoverColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_HIGHLIGHT);
    }
    return dark_ ? kDarkAccentHover : kLightAccentHover;
}

COLORREF UiTheme::BorderColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_BTNSHADOW);
    }
    return dark_ ? kDarkBorder : kLightBorder;
}

COLORREF UiTheme::SuccessColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_WINDOWTEXT);
    }
    return dark_ ? kDarkSuccess : kLightSuccess;
}

COLORREF UiTheme::ErrorColor() const {
    if (high_contrast_) {
        return GetSysColor(COLOR_WINDOWTEXT);
    }
    return dark_ ? kDarkError : kLightError;
}

void DrawThemedPushButton(const DRAWITEMSTRUCT* dis, const UiTheme& theme) {
    HWND hwnd = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool is_default = (dis->itemState & ODS_DEFAULT) != 0;
    const bool is_disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool is_pressed = (dis->itemState & ODS_SELECTED) != 0;
    const UINT dpi = GetDpiForWindow(hwnd);

    FillRect(hdc, &rc, theme.BackgroundBrush());

    wchar_t text[256]{};
    const int text_len = GetWindowTextW(hwnd, text, 256);

    if (theme.IsHighContrast()) {
        if (is_default) {
            FrameRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOWTEXT));
            InflateRect(&rc, -1, -1);
        }
        UINT dfcs = DFCS_BUTTONPUSH;
        if (is_pressed) {
            dfcs |= DFCS_PUSHED;
        }
        if (is_disabled) {
            dfcs |= DFCS_INACTIVE;
        }
        DrawFrameControl(hdc, &rc, DFC_BUTTON, dfcs);

        InflateRect(&rc, -ScaleDpi(2, dpi), -ScaleDpi(2, dpi));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        HFONT font =
            reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        HFONT old_font =
            font != nullptr ? static_cast<HFONT>(SelectObject(hdc, font))
                            : nullptr;
        DrawTextW(hdc, text, text_len, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font != nullptr) {
            SelectObject(hdc, old_font);
        }
        if (dis->itemState & ODS_FOCUS) {
            DrawFocusRect(hdc, &rc);
        }
        return;
    }

    const COLORREF fill = is_default
                              ? (is_pressed ? theme.AccentHoverColor()
                                            : theme.AccentColor())
                              : (is_pressed ? theme.BorderColor()
                                            : theme.SurfaceColor());
    const COLORREF border =
        is_default ? theme.AccentHoverColor() : theme.BorderColor();
    const COLORREF text_color =
        is_default ? RGB(0xFF, 0xFF, 0xFF)
                   : (is_disabled ? theme.SecondaryTextColor()
                                  : theme.TextColor());

    const int corner = ScaleDpi(3, dpi) * 2;
    const int border_width = is_default ? 2 : 1;

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, border_width, border);
    HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, corner, corner);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    HFONT font =
        reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT old_font =
        font != nullptr ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text_color);
    RECT text_rc = rc;
    InflateRect(&text_rc, -ScaleDpi(4, dpi), 0);
    DrawTextW(hdc, text, text_len, &text_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (old_font != nullptr) {
        SelectObject(hdc, old_font);
    }

    if (dis->itemState & ODS_FOCUS) {
        RECT focus_rc = rc;
        InflateRect(&focus_rc, -ScaleDpi(3, dpi), -ScaleDpi(3, dpi));
        DrawFocusRect(hdc, &focus_rc);
    }
}

void DrawThemedCheckbox(const DRAWITEMSTRUCT* dis, const UiTheme& theme,
                        bool onSurface) {
    HWND hwnd = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool checked = (dis->itemState & ODS_CHECKED) != 0;
    const bool is_disabled = (dis->itemState & ODS_DISABLED) != 0;
    const UINT dpi = GetDpiForWindow(hwnd);
    const HBRUSH background_brush =
        onSurface ? theme.SurfaceBrush() : theme.BackgroundBrush();
    const COLORREF background_color =
        onSurface ? theme.SurfaceColor() : theme.BackgroundColor();

    FillRect(hdc, &rc, background_brush);

    wchar_t text[256]{};
    const int text_len = GetWindowTextW(hwnd, text, 256);

    const int box_size = ScaleDpi(14, dpi);
    RECT box_rc{};
    box_rc.left = rc.left;
    box_rc.top = rc.top + (rc.bottom - rc.top - box_size) / 2;
    box_rc.right = box_rc.left + box_size;
    box_rc.bottom = box_rc.top + box_size;

    if (theme.IsHighContrast()) {
        UINT dfcs = DFCS_BUTTONCHECK;
        if (checked) {
            dfcs |= DFCS_CHECKED;
        }
        if (is_disabled) {
            dfcs |= DFCS_INACTIVE;
        }
        DrawFrameControl(hdc, &box_rc, DFC_BUTTON, dfcs);

        RECT text_rc = rc;
        text_rc.left = box_rc.right + ScaleDpi(6, dpi);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        HFONT font =
            reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        HFONT old_font =
            font != nullptr ? static_cast<HFONT>(SelectObject(hdc, font))
                            : nullptr;
        DrawTextW(hdc, text, text_len, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (old_font != nullptr) {
            SelectObject(hdc, old_font);
        }
        return;
    }

    const int corner = ScaleDpi(2, dpi) * 2;
    HBRUSH brush = CreateSolidBrush(
        checked ? theme.AccentColor() : background_color);
    HPEN pen = CreatePen(PS_SOLID, 1,
                         is_disabled ? theme.SecondaryTextColor()
                                     : theme.BorderColor());
    HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
    RoundRect(hdc, box_rc.left, box_rc.top, box_rc.right, box_rc.bottom, corner,
              corner);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (checked) {
        DrawCheckmark(hdc, box_rc, dpi);
    }

    HFONT font =
        reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT old_font =
        font != nullptr ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, is_disabled ? theme.SecondaryTextColor()
                                  : theme.TextColor());
    RECT text_rc = rc;
    text_rc.left = box_rc.right + ScaleDpi(6, dpi);
    DrawTextW(hdc, text, text_len, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (old_font != nullptr) {
        SelectObject(hdc, old_font);
    }

    if (dis->itemState & ODS_FOCUS) {
        RECT focus_rc = rc;
        InflateRect(&focus_rc, -ScaleDpi(2, dpi), -ScaleDpi(2, dpi));
        DrawFocusRect(hdc, &focus_rc);
    }
}

void DrawThemedGroupBox(const DRAWITEMSTRUCT* dis, const UiTheme& theme) {
    HWND hwnd = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const UINT dpi = GetDpiForWindow(hwnd);

    wchar_t text[256]{};
    const int text_len = GetWindowTextW(hwnd, text, 256);

    // WS_CLIPCHILDREN leaves the entire child rectangle to this control.
    // Clear it first so rounded corners and high-contrast rendering are valid.
    FillRect(hdc, &rc, theme.BackgroundBrush());

    HFONT font =
        reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HFONT old_font =
        font != nullptr ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;

    if (theme.IsHighContrast()) {
        FillRect(hdc, &rc, theme.SurfaceBrush());
        HBRUSH old_brush =
            static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
        Rectangle(hdc, rc.left, rc.top + ScaleDpi(8, dpi), rc.right - 1,
                  rc.bottom - 1);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        SelectObject(hdc, old_brush);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        TextOutW(hdc, rc.left + ScaleDpi(8, dpi), rc.top, text, text_len);

        if (old_font != nullptr) {
            SelectObject(hdc, old_font);
        }
        return;
    }

    const int corner = ScaleDpi(4, dpi) * 2;

    // Fill surface.
    HBRUSH brush = CreateSolidBrush(theme.SurfaceColor());
    HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, corner, corner);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);

    // Border.
    HPEN border_pen = CreatePen(PS_SOLID, 1, theme.BorderColor());
    old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
    old_brush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, corner, corner);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);

    // Label text and a small surface patch behind it to hide the top border.
    SIZE text_size{};
    GetTextExtentPoint32W(hdc, text, text_len, &text_size);

    RECT label_bg_rc{};
    label_bg_rc.left = rc.left + ScaleDpi(12, dpi) - ScaleDpi(2, dpi);
    label_bg_rc.top = rc.top;
    label_bg_rc.right = label_bg_rc.left + text_size.cx + ScaleDpi(4, dpi);
    label_bg_rc.bottom = rc.top + text_size.cy;
    HBRUSH surface_brush = CreateSolidBrush(theme.SurfaceColor());
    FillRect(hdc, &label_bg_rc, surface_brush);
    DeleteObject(surface_brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme.TextColor());
    TextOutW(hdc, rc.left + ScaleDpi(12, dpi), rc.top, text, text_len);

    if (old_font != nullptr) {
        SelectObject(hdc, old_font);
    }
}

}  // namespace superdrag::ui
