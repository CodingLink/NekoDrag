#include "settings_store.h"

#include <windows.h>

#include <array>
#include <cwchar>
#include <string>
#include <vector>

namespace nekodrag {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\NekoDrag";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NekoDrag";

// Legacy identifiers are retained only for one-way upgrade compatibility.
constexpr wchar_t kLegacySettingsKey[] = L"Software\\SuperDrag";
constexpr wchar_t kLegacyRunValue[] = L"SuperDrag";

class RegistryKey {
  public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY* receive() noexcept { return &key_; }
    HKEY get() const noexcept { return key_; }
    void close() noexcept {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

  private:
    HKEY key_ = nullptr;
};

void SetError(std::wstring* error, const wchar_t* operation,
              LSTATUS status) {
    if (error == nullptr) {
        return;
    }
    *error = operation;
    error->append(L"（错误代码 ");
    error->append(std::to_wstring(status));
    error->append(L"）");
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD* value,
               std::wstring* error) {
    DWORD size = sizeof(*value);
    const LSTATUS status = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD,
                                        nullptr, value, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        SetError(error, L"读取设置失败", status);
        return false;
    }
    return true;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value,
                std::wstring* error) {
    const LSTATUS status = RegSetValueExW(
        key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
        static_cast<DWORD>(sizeof(value)));
    if (status != ERROR_SUCCESS) {
        SetError(error, L"保存设置失败", status);
        return false;
    }
    return true;
}

struct DwordSnapshot {
    bool exists = false;
    DWORD value = 0;
};

bool SnapshotDword(HKEY key, const wchar_t* name, DwordSnapshot* snapshot,
                   std::wstring* error) {
    DWORD size = sizeof(snapshot->value);
    const LSTATUS status = RegGetValueW(
        key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &snapshot->value, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        *snapshot = DwordSnapshot{};
        return true;
    }
    if (status != ERROR_SUCCESS) {
        SetError(error, L"读取原设置失败", status);
        return false;
    }
    snapshot->exists = true;
    return true;
}

bool RestoreDword(HKEY key, const wchar_t* name,
                  const DwordSnapshot& snapshot) {
    if (snapshot.exists) {
        return RegSetValueExW(
                   key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&snapshot.value),
                   static_cast<DWORD>(sizeof(snapshot.value))) == ERROR_SUCCESS;
    }
    const LSTATUS status = RegDeleteValueW(key, name);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

struct StringSnapshot {
    bool exists = false;
    std::wstring value;
};

bool SnapshotString(HKEY key, const wchar_t* name, StringSnapshot* snapshot,
                    std::wstring* error) {
    *snapshot = StringSnapshot{};

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        SetError(error, L"读取开机启动设置失败",
                 status == ERROR_SUCCESS ? ERROR_INVALID_DATATYPE : status);
        return false;
    }

    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &size);
    if (status != ERROR_SUCCESS) {
        SetError(error, L"读取开机启动设置失败", status);
        return false;
    }
    snapshot->exists = true;
    snapshot->value.assign(buffer.data());
    return true;
}

bool ValueExists(HKEY key, const wchar_t* name, bool* exists,
                 std::wstring* error) {
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        *exists = false;
        return true;
    }
    if (status != ERROR_SUCCESS) {
        SetError(error, L"读取开机启动设置失败", status);
        return false;
    }
    *exists = true;
    return true;
}

LSTATUS WriteString(HKEY key, const wchar_t* name,
                    const std::wstring& value) {
    const DWORD size =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), size);
}

bool RestoreString(HKEY key, const wchar_t* name,
                   const StringSnapshot& snapshot) {
    if (snapshot.exists) {
        return WriteString(key, name, snapshot.value) == ERROR_SUCCESS;
    }
    const LSTATUS status = RegDeleteValueW(key, name);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool LoadSettingsFromKey(const wchar_t* keyPath, UserSettings* settings,
                         bool* keyExists, std::wstring* error) {
    *keyExists = false;

    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, key.receive());
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        SetError(error, L"打开设置失败", openStatus);
        return false;
    }
    *keyExists = true;

    DWORD enabled = settings->enabled ? 1U : 0U;
    DWORD modifierMask = settings->modifierMask;
    DWORD dragMode = static_cast<DWORD>(settings->dragEngineMode);
    DWORD firstRunCompleted = settings->firstRunCompleted ? 1U : 0U;
    DWORD privilegeHintShown = settings->privilegeHintShown ? 1U : 0U;
    if (!ReadDword(key.get(), L"Enabled", &enabled, error) ||
        !ReadDword(key.get(), L"ModifierMask", &modifierMask, error) ||
        !ReadDword(key.get(), L"DragMode", &dragMode, error) ||
        !ReadDword(key.get(), L"FirstRunCompleted", &firstRunCompleted,
                   error) ||
        !ReadDword(key.get(), L"PrivilegeHintShown", &privilegeHintShown,
                   error)) {
        return false;
    }

    settings->enabled = enabled != 0;
    settings->modifierMask =
        IsValidModifierMask(modifierMask) ? modifierMask : kDefaultModifiers;
    settings->dragEngineMode = NormalizeDragEngineMode(dragMode);
    settings->firstRunCompleted = firstRunCompleted != 0;
    settings->privilegeHintShown = privilegeHintShown != 0;
    return true;
}

bool CurrentExecutablePath(std::wstring* path, std::wstring* error) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            SetError(error, L"无法获取程序路径", GetLastError());
            return false;
        }
        if (length < buffer.size() - 1) {
            path->assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= 32768) {
            if (error != nullptr) {
                *error = L"程序路径过长，无法设置开机启动";
            }
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool ExpectedStartupCommand(std::wstring* command, std::wstring* error) {
    std::wstring path;
    if (!CurrentExecutablePath(&path, error)) {
        return false;
    }
    *command = L"\"";
    command->append(path);
    command->append(L"\"");
    return true;
}

}  // namespace

bool LoadSettings(UserSettings* settings, SettingsLoadInfo* loadInfo,
                  std::wstring* error) {
    if (settings == nullptr || loadInfo == nullptr) {
        if (error != nullptr) {
            *error = L"设置参数无效";
        }
        return false;
    }

    *settings = UserSettings{};
    *loadInfo = SettingsLoadInfo{};

    bool currentKeyExists = false;
    if (!LoadSettingsFromKey(kSettingsKey, settings, &currentKeyExists,
                             error)) {
        return false;
    }
    if (currentKeyExists) {
        loadInfo->settingsKeyExists = true;
        return true;
    }

    bool legacyKeyExists = false;
    if (!LoadSettingsFromKey(kLegacySettingsKey, settings, &legacyKeyExists,
                             error)) {
        return false;
    }
    if (legacyKeyExists) {
        loadInfo->settingsKeyExists = true;
        loadInfo->importedLegacySettings = true;
    }
    return true;
}

bool SaveSettings(const UserSettings& settings, std::wstring* error) {
    if (!IsValidModifierMask(settings.modifierMask)) {
        if (error != nullptr) {
            *error = L"快捷键必须包含 1 至 3 个修饰键";
        }
        return false;
    }
    const DWORD dragMode = static_cast<DWORD>(settings.dragEngineMode);
    if (!IsValidDragEngineMode(dragMode)) {
        if (error != nullptr) {
            *error = L"拖动模式无效";
        }
        return false;
    }

    RegistryKey key;
    DWORD disposition = 0;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kSettingsKey, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
        key.receive(), &disposition);
    if (createStatus != ERROR_SUCCESS) {
        SetError(error, L"创建设置失败", createStatus);
        return false;
    }

    struct PendingDword {
        const wchar_t* name;
        DWORD value;
        DwordSnapshot previous;
    };
    std::array<PendingDword, 5> values{{
        {L"Enabled", settings.enabled ? 1U : 0U, {}},
        {L"ModifierMask", settings.modifierMask, {}},
        {L"DragMode", dragMode, {}},
        {L"FirstRunCompleted", settings.firstRunCompleted ? 1U : 0U, {}},
        {L"PrivilegeHintShown", settings.privilegeHintShown ? 1U : 0U, {}},
    }};

    const auto removeNewKeyAfterFailure = [&key, disposition]() {
        if (disposition != REG_CREATED_NEW_KEY) {
            return true;
        }
        key.close();
        const LSTATUS status =
            RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsKey);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    };

    for (PendingDword& value : values) {
        if (!SnapshotDword(key.get(), value.name, &value.previous, error)) {
            if (!removeNewKeyAfterFailure() && error != nullptr) {
                error->append(L"；清理未完成的新设置键时也发生错误");
            }
            return false;
        }
    }

    std::size_t writtenCount = 0;
    for (; writtenCount < values.size(); ++writtenCount) {
        const PendingDword& value = values[writtenCount];
        if (WriteDword(key.get(), value.name, value.value, error)) {
            continue;
        }

        bool rollbackSucceeded = true;
        while (writtenCount > 0) {
            --writtenCount;
            const PendingDword& written = values[writtenCount];
            rollbackSucceeded =
                RestoreDword(key.get(), written.name, written.previous) &&
                rollbackSucceeded;
        }
        if (!rollbackSucceeded && error != nullptr) {
            error->append(L"；恢复原设置时也发生错误");
        }
        if (!removeNewKeyAfterFailure() && error != nullptr) {
            error->append(L"；清理未完成的新设置键时也发生错误");
        }
        return false;
    }
    return true;
}

bool QueryStartupEnabled(bool* enabled, std::wstring* command,
                         std::wstring* error) {
    if (enabled == nullptr) {
        if (error != nullptr) {
            *error = L"开机启动参数无效";
        }
        return false;
    }
    *enabled = false;
    if (command != nullptr) {
        command->clear();
    }

    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, key.receive());
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        SetError(error, L"读取开机启动设置失败", openStatus);
        return false;
    }

    StringSnapshot current;
    if (!SnapshotString(key.get(), kRunValue, &current, error)) {
        return false;
    }
    if (current.exists) {
        *enabled = true;
        if (command != nullptr) {
            *command = current.value;
        }
        return true;
    }

    StringSnapshot legacy;
    if (!SnapshotString(key.get(), kLegacyRunValue, &legacy, error)) {
        return false;
    }
    if (!legacy.exists) {
        return true;
    }
    *enabled = true;
    if (command != nullptr) {
        *command = legacy.value;
    }
    return true;
}

bool SetStartupEnabled(bool enabled, std::wstring* error) {
    RegistryKey key;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, key.receive(), nullptr);
    if (createStatus != ERROR_SUCCESS) {
        SetError(error, L"打开开机启动设置失败", createStatus);
        return false;
    }

    if (!enabled) {
        const LSTATUS currentStatus = RegDeleteValueW(key.get(), kRunValue);
        const LSTATUS legacyStatus =
            RegDeleteValueW(key.get(), kLegacyRunValue);
        if (currentStatus != ERROR_SUCCESS &&
            currentStatus != ERROR_FILE_NOT_FOUND) {
            SetError(error, L"关闭开机启动失败", currentStatus);
            return false;
        }
        if (legacyStatus != ERROR_SUCCESS &&
            legacyStatus != ERROR_FILE_NOT_FOUND) {
            SetError(error, L"清理旧开机启动项失败", legacyStatus);
            return false;
        }
        return true;
    }

    std::wstring command;
    if (!ExpectedStartupCommand(&command, error)) {
        return false;
    }

    StringSnapshot previousCurrent;
    if (!SnapshotString(key.get(), kRunValue, &previousCurrent, error)) {
        return false;
    }

    const LSTATUS writeStatus = WriteString(key.get(), kRunValue, command);
    if (writeStatus != ERROR_SUCCESS) {
        SetError(error, L"启用开机启动失败", writeStatus);
        return false;
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key.get(), kLegacyRunValue);
    if (deleteStatus != ERROR_SUCCESS &&
        deleteStatus != ERROR_FILE_NOT_FOUND) {
        const bool rollbackSucceeded =
            RestoreString(key.get(), kRunValue, previousCurrent);
        SetError(error, L"迁移旧开机启动项失败", deleteStatus);
        if (!rollbackSucceeded && error != nullptr) {
            error->append(L"；恢复 NekoDrag 启动项时也发生错误");
        }
        return false;
    }
    return true;
}

bool ReconcileStartupPath(std::wstring* error) {
    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, key.receive());
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        SetError(error, L"读取开机启动设置失败", openStatus);
        return false;
    }

    StringSnapshot current;
    if (!SnapshotString(key.get(), kRunValue, &current, error)) {
        return false;
    }

    bool legacyExists = false;
    if (!ValueExists(key.get(), kLegacyRunValue, &legacyExists, error)) {
        return false;
    }
    if (!current.exists && !legacyExists) {
        return true;
    }

    std::wstring expectedCommand;
    if (!ExpectedStartupCommand(&expectedCommand, error)) {
        return false;
    }
    if (current.exists && !legacyExists &&
        _wcsicmp(current.value.c_str(), expectedCommand.c_str()) == 0) {
        return true;
    }
    return SetStartupEnabled(true, error);
}

}  // namespace nekodrag
