#ifndef VPN_SERVICE_MANAGER_H_
#define VPN_SERVICE_MANAGER_H_

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vpn_plugin {

// Manages the elevated vpn_service.exe helper process.
// Creates a named pipe, launches the helper with UAC elevation,
// and handles bidirectional IPC for VPN start/stop/state.
class VpnServiceManager {
 public:
  using StateCallback = std::function<void(int state)>;

  VpnServiceManager();
  ~VpnServiceManager();

  VpnServiceManager(const VpnServiceManager&) = delete;
  VpnServiceManager& operator=(const VpnServiceManager&) = delete;

  void SetStateCallback(StateCallback callback);

  // Launches the helper if not already running. Triggers UAC on first call.
  bool LaunchIfNeeded();

  bool SendStart(const std::string& config_path);
  bool SendStop();
  bool IsRunning() const;
  void Shutdown();

 private:
  bool CreatePipeAndLaunch();
  bool SendLine(const std::string& line);
  void ReaderThreadFunc();
  void OnHelperDied();

  std::string pipe_name_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::thread reader_thread_;
  std::mutex write_mutex_;
  StateCallback state_callback_;
};

}  // namespace vpn_plugin

#endif  // VPN_SERVICE_MANAGER_H_
