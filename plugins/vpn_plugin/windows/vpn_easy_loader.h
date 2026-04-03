#ifndef VPN_EASY_LOADER_H_
#define VPN_EASY_LOADER_H_

#include <windows.h>

#include <string>

namespace vpn_plugin {

// Function pointer types matching vpn_easy.h exports.
using VpnEasyStartFn = void (*)(const char* toml_config,
                                 void (*on_state_changed)(void*, int),
                                 void* state_changed_arg);
using VpnEasyStopFn = void (*)();

// Dynamically loads vpn_easy.dll and resolves its exported functions.
// Falls back gracefully if the DLL is not present.
class VpnEasyLoader {
 public:
  VpnEasyLoader() = default;
  ~VpnEasyLoader();

  VpnEasyLoader(const VpnEasyLoader&) = delete;
  VpnEasyLoader& operator=(const VpnEasyLoader&) = delete;

  // Attempts to load vpn_easy.dll from the given path.
  // Returns true if both vpn_easy_start and vpn_easy_stop were resolved.
  bool Load(const std::string& dll_path);

  // Returns true if the DLL is loaded and function pointers are valid.
  bool IsLoaded() const;

  // Proxies to vpn_easy_start. Caller must ensure IsLoaded() is true.
  void Start(const char* toml_config,
             void (*on_state_changed)(void*, int),
             void* state_changed_arg);

  // Proxies to vpn_easy_stop. Caller must ensure IsLoaded() is true.
  void Stop();

 private:
  HMODULE module_ = nullptr;
  VpnEasyStartFn start_fn_ = nullptr;
  VpnEasyStopFn stop_fn_ = nullptr;
};

}  // namespace vpn_plugin

#endif  // VPN_EASY_LOADER_H_
