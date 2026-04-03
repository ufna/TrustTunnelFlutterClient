#include "vpn_easy_loader.h"

namespace vpn_plugin {

VpnEasyLoader::~VpnEasyLoader() {
  if (module_) {
    FreeLibrary(module_);
    module_ = nullptr;
  }
}

bool VpnEasyLoader::Load(const std::string& dll_path) {
  module_ = LoadLibraryA(dll_path.c_str());
  if (!module_) return false;

  start_fn_ = reinterpret_cast<VpnEasyStartFn>(
      GetProcAddress(module_, "vpn_easy_start"));
  stop_fn_ = reinterpret_cast<VpnEasyStopFn>(
      GetProcAddress(module_, "vpn_easy_stop"));

  if (!start_fn_ || !stop_fn_) {
    FreeLibrary(module_);
    module_ = nullptr;
    start_fn_ = nullptr;
    stop_fn_ = nullptr;
    return false;
  }
  return true;
}

bool VpnEasyLoader::IsLoaded() const {
  return module_ != nullptr && start_fn_ != nullptr && stop_fn_ != nullptr;
}

void VpnEasyLoader::Start(const char* toml_config,
                           void (*on_state_changed)(void*, int),
                           void* state_changed_arg) {
  if (start_fn_) {
    start_fn_(toml_config, on_state_changed, state_changed_arg);
  }
}

void VpnEasyLoader::Stop() {
  if (stop_fn_) {
    stop_fn_();
  }
}

}  // namespace vpn_plugin
