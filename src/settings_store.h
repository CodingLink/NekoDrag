#pragma once

#include "core.h"

#include <string>

namespace superdrag {

bool LoadSettings(UserSettings* settings, bool* settingsKeyExists,
                  std::wstring* error);
bool SaveSettings(const UserSettings& settings, std::wstring* error);

bool QueryStartupEnabled(bool* enabled, std::wstring* command,
                         std::wstring* error);
bool SetStartupEnabled(bool enabled, std::wstring* error);
bool ReconcileStartupPath(std::wstring* error);

}  // namespace superdrag
