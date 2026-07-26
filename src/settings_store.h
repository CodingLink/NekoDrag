#pragma once

#include "core.h"

#include <string>

namespace nekodrag {

struct SettingsLoadInfo {
    bool settingsKeyExists = false;
    bool importedLegacySettings = false;
};

bool LoadSettings(UserSettings* settings, SettingsLoadInfo* loadInfo,
                  std::wstring* error);
bool SaveSettings(const UserSettings& settings, std::wstring* error);

bool QueryStartupEnabled(bool* enabled, std::wstring* command,
                         std::wstring* error);
bool SetStartupEnabled(bool enabled, std::wstring* error);
bool ReconcileStartupPath(bool allowLegacyMigration, std::wstring* error);

}  // namespace nekodrag
