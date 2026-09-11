#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRegistryKey[] = L"SOFTWARE\\Zultys\\NCallBridge";
constexpr wchar_t kProviderIdName[] = L"ProviderId";
constexpr wchar_t kTspFileName[] = L"ZultysNCallTsp.tsp";

bool ReadProviderId(DWORD* provider_id) {
  if (provider_id == nullptr) {
    return false;
  }

  HKEY key = nullptr;
  const LSTATUS open_status = ::RegOpenKeyExW(
      HKEY_LOCAL_MACHINE, kRegistryKey, 0, KEY_QUERY_VALUE, &key);
  if (open_status == ERROR_FILE_NOT_FOUND) {
    *provider_id = 0;
    return true;
  }
  if (open_status != ERROR_SUCCESS) {
    std::fwprintf(stderr, L"Could not read provider registration state (0x%08lX).\n",
                  static_cast<unsigned long>(open_status));
    return false;
  }

  DWORD value = 0;
  DWORD value_size = sizeof(value);
  const LSTATUS read_status = ::RegGetValueW(
      key, nullptr, kProviderIdName, RRF_RT_REG_DWORD, nullptr, &value,
      &value_size);
  ::RegCloseKey(key);
  if (read_status == ERROR_FILE_NOT_FOUND) {
    *provider_id = 0;
    return true;
  }
  if (read_status != ERROR_SUCCESS || value_size != sizeof(value)) {
    std::fwprintf(stderr, L"Could not read ProviderId (0x%08lX).\n",
                  static_cast<unsigned long>(read_status));
    return false;
  }

  *provider_id = value;
  return true;
}

bool WriteProviderId(const DWORD provider_id) {
  HKEY key = nullptr;
  DWORD disposition = 0;
  const LSTATUS create_status = ::RegCreateKeyExW(
      HKEY_LOCAL_MACHINE, kRegistryKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_SET_VALUE, nullptr, &key, &disposition);
  if (create_status != ERROR_SUCCESS) {
    std::fwprintf(stderr, L"Could not write provider registration state (0x%08lX).\n",
                  static_cast<unsigned long>(create_status));
    return false;
  }

  const LSTATUS write_status = ::RegSetValueExW(
      key, kProviderIdName, 0, REG_DWORD,
      reinterpret_cast<const BYTE*>(&provider_id), sizeof(provider_id));
  ::RegCloseKey(key);
  if (write_status != ERROR_SUCCESS) {
    std::fwprintf(stderr, L"Could not save ProviderId (0x%08lX).\n",
                  static_cast<unsigned long>(write_status));
    return false;
  }
  return true;
}

bool ClearProviderId() {
  HKEY key = nullptr;
  const LSTATUS open_status = ::RegOpenKeyExW(
      HKEY_LOCAL_MACHINE, kRegistryKey, 0, KEY_SET_VALUE, &key);
  if (open_status == ERROR_FILE_NOT_FOUND) {
    return true;
  }
  if (open_status != ERROR_SUCCESS) {
    std::fwprintf(stderr, L"Could not clear provider registration state (0x%08lX).\n",
                  static_cast<unsigned long>(open_status));
    return false;
  }

  const LSTATUS delete_status = ::RegDeleteValueW(key, kProviderIdName);
  ::RegCloseKey(key);
  if (delete_status != ERROR_SUCCESS && delete_status != ERROR_FILE_NOT_FOUND) {
    std::fwprintf(stderr, L"Could not clear ProviderId (0x%08lX).\n",
                  static_cast<unsigned long>(delete_status));
    return false;
  }
  return true;
}

bool GetTspPath(std::wstring* path) {
  if (path == nullptr) {
    return false;
  }

  std::vector<wchar_t> executable_path(MAX_PATH);
  DWORD length = ::GetModuleFileNameW(nullptr, executable_path.data(),
                                      static_cast<DWORD>(executable_path.size()));
  if (length == 0) {
    std::fwprintf(stderr, L"Could not determine the registration helper path (0x%08lX).\n",
                  static_cast<unsigned long>(::GetLastError()));
    return false;
  }
  if (length >= executable_path.size() - 1) {
    executable_path.resize(static_cast<size_t>(length) + 2);
    length = ::GetModuleFileNameW(nullptr, executable_path.data(),
                                  static_cast<DWORD>(executable_path.size()));
    if (length == 0 || length >= executable_path.size() - 1) {
      std::fwprintf(stderr, L"Could not determine the registration helper path.\n");
      return false;
    }
  }

  const std::wstring helper_path(executable_path.data(), length);
  const size_t separator = helper_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    std::fwprintf(stderr, L"Could not determine the registration helper directory.\n");
    return false;
  }

  *path = helper_path.substr(0, separator + 1) + kTspFileName;
  const DWORD attributes = ::GetFileAttributesW(path->c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    std::fwprintf(stderr, L"TSP file is missing: %ls\n", path->c_str());
    return false;
  }
  return true;
}

int RemoveRegisteredProvider() {
  DWORD provider_id = 0;
  if (!ReadProviderId(&provider_id)) {
    return 1;
  }
  if (provider_id == 0) {
    return 0;
  }

  const LONG result = ::lineRemoveProvider(provider_id, nullptr);
  if (result != 0) {
    std::fwprintf(stderr, L"lineRemoveProvider failed for ProviderId %lu (0x%08lX).\n",
                  static_cast<unsigned long>(provider_id),
                  static_cast<unsigned long>(result));
    return 1;
  }
  return ClearProviderId() ? 0 : 1;
}

int InstallProvider() {
  DWORD existing_provider_id = 0;
  if (!ReadProviderId(&existing_provider_id)) {
    return 1;
  }
  if (existing_provider_id != 0) {
    std::fwprintf(stderr,
                  L"ProviderId %lu already exists; refusing to replace a registered provider.\n",
                  static_cast<unsigned long>(existing_provider_id));
    return 1;
  }

  std::wstring tsp_path;
  if (!GetTspPath(&tsp_path)) {
    return 1;
  }

  DWORD provider_id = 0;
  const LONG result = ::lineAddProviderW(tsp_path.c_str(), nullptr, &provider_id);
  if (result != 0 || provider_id == 0) {
    std::fwprintf(stderr, L"lineAddProviderW failed (0x%08lX).\n",
                  static_cast<unsigned long>(result));
    return 1;
  }

  if (!WriteProviderId(provider_id)) {
    const LONG rollback_result = ::lineRemoveProvider(provider_id, nullptr);
    if (rollback_result != 0) {
      std::fwprintf(stderr,
                    L"Could not roll back ProviderId %lu after registry write failure (0x%08lX).\n",
                    static_cast<unsigned long>(provider_id),
                    static_cast<unsigned long>(rollback_result));
    }
    return 1;
  }

  std::wprintf(L"Registered Zultys nCall TSP with ProviderId %lu.\n",
               static_cast<unsigned long>(provider_id));
  return 0;
}

int Usage() {
  std::fwprintf(stderr,
                L"Usage: ZultysNCallProviderReg.exe install|uninstall|rollback-install\n");
  return 2;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  if (argc != 2 || argv[1] == nullptr) {
    return Usage();
  }

  const std::wstring command(argv[1]);
  if (command == L"install") {
    return InstallProvider();
  }
  if (command == L"uninstall" || command == L"rollback-install") {
    return RemoveRegisteredProvider();
  }
  return Usage();
}
