#include "settings_store.h"

#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace superdrag {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\SuperDrag";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"SuperDrag";

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

bool LoadSettings(UserSettings* settings, bool* settingsKeyExists,
                  std::wstring* error) {
    if (settings == nullptr || settingsKeyExists == nullptr) {
        if (error != nullptr) {
            *error = L"设置参数无效";
        }
        return false;
    }

    *settings = UserSettings{};
    *settingsKeyExists = false;

    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, key.receive());
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        SetError(error, L"打开设置失败", openStatus);
        return false;
    }
    *settingsKeyExists = true;

    DWORD enabled = settings->enabled ? 1U : 0U;
    DWORD modifierMask = settings->modifierMask;
    DWORD firstRunCompleted = settings->firstRunCompleted ? 1U : 0U;
    DWORD privilegeHintShown = settings->privilegeHintShown ? 1U : 0U;
    if (!ReadDword(key.get(), L"Enabled", &enabled, error) ||
        !ReadDword(key.get(), L"ModifierMask", &modifierMask, error) ||
        !ReadDword(key.get(), L"FirstRunCompleted", &firstRunCompleted,
                   error) ||
        !ReadDword(key.get(), L"PrivilegeHintShown", &privilegeHintShown,
                   error)) {
        return false;
    }

    settings->enabled = enabled != 0;
    settings->modifierMask =
        IsValidModifierMask(modifierMask) ? modifierMask : kDefaultModifiers;
    settings->firstRunCompleted = firstRunCompleted != 0;
    settings->privilegeHintShown = privilegeHintShown != 0;
    return true;
}

bool SaveSettings(const UserSettings& settings, std::wstring* error) {
    if (!IsValidModifierMask(settings.modifierMask)) {
        if (error != nullptr) {
            *error = L"快捷键必须包含 1 至 3 个修饰键";
        }
        return false;
    }

    RegistryKey key;
    DWORD disposition = 0;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kSettingsKey, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, key.receive(),
        &disposition);
    if (createStatus != ERROR_SUCCESS) {
        SetError(error, L"创建设置失败", createStatus);
        return false;
    }

    return WriteDword(key.get(), L"Enabled", settings.enabled ? 1U : 0U,
                      error) &&
           WriteDword(key.get(), L"ModifierMask", settings.modifierMask,
                      error) &&
           WriteDword(key.get(), L"FirstRunCompleted",
                      settings.firstRunCompleted ? 1U : 0U, error) &&
           WriteDword(key.get(), L"PrivilegeHintShown",
                      settings.privilegeHintShown ? 1U : 0U, error);
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

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = RegQueryValueExW(key.get(), kRunValue, nullptr, &type,
                                      nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        SetError(error, L"读取开机启动设置失败",
                 status == ERROR_SUCCESS ? ERROR_INVALID_DATATYPE : status);
        return false;
    }

    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key.get(), kRunValue, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &size);
    if (status != ERROR_SUCCESS) {
        SetError(error, L"读取开机启动设置失败", status);
        return false;
    }
    *enabled = true;
    if (command != nullptr) {
        command->assign(buffer.data());
    }
    return true;
}

bool SetStartupEnabled(bool enabled, std::wstring* error) {
    RegistryKey key;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, key.receive(), nullptr);
    if (createStatus != ERROR_SUCCESS) {
        SetError(error, L"打开开机启动设置失败", createStatus);
        return false;
    }

    if (!enabled) {
        const LSTATUS deleteStatus = RegDeleteValueW(key.get(), kRunValue);
        if (deleteStatus != ERROR_SUCCESS &&
            deleteStatus != ERROR_FILE_NOT_FOUND) {
            SetError(error, L"关闭开机启动失败", deleteStatus);
            return false;
        }
        return true;
    }

    std::wstring command;
    if (!ExpectedStartupCommand(&command, error)) {
        return false;
    }
    const DWORD size = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key.get(), kRunValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), size);
    if (status != ERROR_SUCCESS) {
        SetError(error, L"启用开机启动失败", status);
        return false;
    }
    return true;
}

bool ReconcileStartupPath(std::wstring* error) {
    bool enabled = false;
    std::wstring currentCommand;
    if (!QueryStartupEnabled(&enabled, &currentCommand, error)) {
        return false;
    }
    if (!enabled) {
        return true;
    }

    std::wstring expectedCommand;
    if (!ExpectedStartupCommand(&expectedCommand, error)) {
        return false;
    }
    if (_wcsicmp(currentCommand.c_str(), expectedCommand.c_str()) == 0) {
        return true;
    }
    return SetStartupEnabled(true, error);
}

}  // namespace superdrag
