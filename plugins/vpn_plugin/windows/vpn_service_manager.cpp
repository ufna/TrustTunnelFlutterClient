#include "vpn_service_manager.h"

#include <shellapi.h>

namespace vpn_plugin {

VpnServiceManager::VpnServiceManager() {
  pipe_name_ = "\\\\.\\pipe\\trusttunnel_vpn_" +
                std::to_string(GetCurrentProcessId());
}

VpnServiceManager::~VpnServiceManager() {
  Shutdown();
}

void VpnServiceManager::SetStateCallback(StateCallback callback) {
  state_callback_ = std::move(callback);
}

bool VpnServiceManager::IsRunning() const {
  return running_.load() && ready_.load();
}

bool VpnServiceManager::LaunchIfNeeded() {
  if (IsRunning()) return true;
  Shutdown();
  return CreatePipeAndLaunch();
}

bool VpnServiceManager::CreatePipeAndLaunch() {
  pipe_ = CreateNamedPipeA(
      pipe_name_.c_str(),
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1, 4096, 4096, 0, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) return false;

  // Find vpn_service.exe next to vpn.exe.
  char exe_path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
  std::string dir(exe_path);
  dir = dir.substr(0, dir.find_last_of('\\') + 1);
  std::string service_exe = dir + "vpn_service.exe";

  // Launch elevated via ShellExecuteEx.
  SHELLEXECUTEINFOA sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = "runas";
  sei.lpFile = service_exe.c_str();
  sei.lpParameters = pipe_name_.c_str();
  sei.nShow = SW_HIDE;

  if (!ShellExecuteExA(&sei)) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    return false;
  }

  if (sei.hProcess) CloseHandle(sei.hProcess);

  // Wait for helper to connect.
  if (!ConnectNamedPipe(pipe_, nullptr)) {
    DWORD err = GetLastError();
    if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
      return false;
    }
  }

  running_.store(true);
  reader_thread_ = std::thread(&VpnServiceManager::ReaderThreadFunc, this);

  // Wait for READY (up to 5 seconds).
  for (int i = 0; i < 50 && !ready_.load(); ++i) {
    Sleep(100);
  }

  return ready_.load();
}

bool VpnServiceManager::SendLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (pipe_ == INVALID_HANDLE_VALUE) return false;
  std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile(pipe_, data.c_str(), static_cast<DWORD>(data.size()),
                   &written, nullptr) != 0;
}

bool VpnServiceManager::SendStart(const std::string& config_path) {
  return SendLine("START " + config_path);
}

bool VpnServiceManager::SendStop() {
  return SendLine("STOP");
}

void VpnServiceManager::ReaderThreadFunc() {
  std::string buffer;
  char ch = 0;
  DWORD read_count = 0;

  while (running_.load()) {
    if (!ReadFile(pipe_, &ch, 1, &read_count, nullptr) || read_count == 0) {
      OnHelperDied();
      return;
    }

    if (ch == '\n') {
      if (buffer == "READY") {
        ready_.store(true);
      } else if (buffer.rfind("STATE ", 0) == 0) {
        int state = std::atoi(buffer.c_str() + 6);
        if (state_callback_) state_callback_(state);
      }
      buffer.clear();
    } else if (ch != '\r') {
      buffer += ch;
    }
  }
}

void VpnServiceManager::OnHelperDied() {
  running_.store(false);
  ready_.store(false);
  if (state_callback_) state_callback_(0);
}

void VpnServiceManager::Shutdown() {
  running_.store(false);
  ready_.store(false);

  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }

  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
}

}  // namespace vpn_plugin
