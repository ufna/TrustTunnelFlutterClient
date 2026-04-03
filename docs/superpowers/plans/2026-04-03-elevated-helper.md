# Elevated Helper Process Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move VPN engine into a separate elevated process so the main app runs without admin rights, requesting UAC only on first Connect.

**Architecture:** `vpn_service.exe` runs elevated, loads `vpn_easy.dll`, communicates with the Flutter plugin via a bidirectional named pipe. `VpnServiceManager` in the plugin manages the helper lifecycle and IPC. `IVpnManagerImpl` writes patched config to a temp file and delegates start/stop to the service manager.

**Tech Stack:** C++17, Windows API (Named Pipes, ShellExecuteEx, CreateProcess), Flutter Windows SDK

---

### Task 1: Create vpn_service.exe

Standalone C++ console app that receives a pipe name as argument, connects, loads vpn_easy.dll, and processes START/STOP commands.

**Files:**
- Create: `windows/runner/vpn_service.cpp`

- [ ] **Step 1: Create vpn_service.cpp**

```cpp
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

// ---- Minimal vpn_easy.dll loader (inline, no external deps) ----

using VpnEasyStartFn = void (*)(const char*, void (*)(void*, int), void*);
using VpnEasyStopFn = void (*)();

static VpnEasyStartFn g_start_fn = nullptr;
static VpnEasyStopFn g_stop_fn = nullptr;
static bool g_vpn_running = false;

static bool LoadVpnEasy() {
  HMODULE mod = LoadLibraryA("vpn_easy.dll");
  if (!mod) return false;
  g_start_fn = reinterpret_cast<VpnEasyStartFn>(
      GetProcAddress(mod, "vpn_easy_start"));
  g_stop_fn = reinterpret_cast<VpnEasyStopFn>(
      GetProcAddress(mod, "vpn_easy_stop"));
  return g_start_fn && g_stop_fn;
}

// ---- Pipe I/O ----

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::mutex g_pipe_mutex;

static bool SendLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_pipe_mutex);
  std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile(g_pipe, data.c_str(), static_cast<DWORD>(data.size()),
                   &written, nullptr) != 0;
}

static std::string ReadLine() {
  std::string result;
  char ch = 0;
  DWORD read = 0;
  while (ReadFile(g_pipe, &ch, 1, &read, nullptr) && read == 1) {
    if (ch == '\n') break;
    if (ch != '\r') result += ch;
  }
  return result;
}

// ---- VPN state callback (called from vpn_easy background thread) ----

static void OnStateChanged(void* /*arg*/, int state) {
  SendLine("STATE " + std::to_string(state));
}

// ---- Read config file ----

static std::string ReadFileContent(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- Main ----

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: vpn_service.exe <pipe_name>\n");
    return 1;
  }

  const std::string pipe_name = argv[1];

  // Connect to the named pipe created by the Flutter plugin.
  g_pipe = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (g_pipe == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Failed to connect to pipe: %lu\n", GetLastError());
    return 1;
  }

  // Set pipe to message mode for reading.
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(g_pipe, &mode, nullptr, nullptr);

  // Load vpn_easy.dll
  if (LoadVpnEasy()) {
    SendLine("READY");
  } else {
    SendLine("ERROR Failed to load vpn_easy.dll");
  }

  // Command loop
  while (true) {
    std::string line = ReadLine();
    if (line.empty()) {
      // Pipe broken (app closed) — stop VPN and exit.
      if (g_vpn_running && g_stop_fn) {
        g_stop_fn();
      }
      break;
    }

    if (line.rfind("START ", 0) == 0) {
      std::string config_path = line.substr(6);
      std::string config = ReadFileContent(config_path);
      if (config.empty()) {
        SendLine("ERROR Cannot read config file: " + config_path);
        SendLine("STATE 0");
        continue;
      }
      if (!g_start_fn) {
        SendLine("ERROR vpn_easy.dll not loaded");
        SendLine("STATE 0");
        continue;
      }
      // Stop previous session if running.
      if (g_vpn_running && g_stop_fn) {
        g_stop_fn();
      }
      g_vpn_running = true;
      g_start_fn(config.c_str(), &OnStateChanged, nullptr);

    } else if (line == "STOP") {
      if (g_vpn_running && g_stop_fn) {
        g_stop_fn();
        g_vpn_running = false;
      }
      SendLine("STATE 0");
    }
  }

  CloseHandle(g_pipe);
  return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add windows/runner/vpn_service.cpp
git commit -m "feat(windows): add vpn_service.exe helper for elevated VPN operations"
```

---

### Task 2: Add vpn_service.exe to CMake build and install

**Files:**
- Modify: `windows/runner/CMakeLists.txt`
- Modify: `windows/CMakeLists.txt`

- [ ] **Step 1: Add vpn_service target to runner/CMakeLists.txt**

Append at the end of `windows/runner/CMakeLists.txt`:

```cmake
# Elevated VPN helper process.
# Runs as administrator, communicates with the main app via named pipe.
add_executable(vpn_service "vpn_service.cpp")
apply_standard_settings(vpn_service)
target_compile_definitions(vpn_service PRIVATE "NOMINMAX")
```

- [ ] **Step 2: Add install rule for vpn_service in windows/CMakeLists.txt**

After the `install(TARGETS ${BINARY_NAME} ...)` line (line 75-76), add:

```cmake
install(TARGETS vpn_service RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
  COMPONENT Runtime)
```

- [ ] **Step 3: Commit**

```bash
git add windows/runner/CMakeLists.txt windows/CMakeLists.txt
git commit -m "build(windows): add vpn_service.exe CMake target and install rule"
```

---

### Task 3: Create VpnServiceManager

Named pipe server + process launcher + IPC protocol handler.

**Files:**
- Create: `plugins/vpn_plugin/windows/vpn_service_manager.h`
- Create: `plugins/vpn_plugin/windows/vpn_service_manager.cpp`

- [ ] **Step 1: Create vpn_service_manager.h**

```cpp
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

  // Sets callback for VPN state changes received from the helper.
  void SetStateCallback(StateCallback callback);

  // Launches the helper if not already running. Triggers UAC on first call.
  // Returns true if helper is ready, false if UAC was declined or launch failed.
  bool LaunchIfNeeded();

  // Sends START command with config file path.
  bool SendStart(const std::string& config_path);

  // Sends STOP command.
  bool SendStop();

  // Returns true if the helper process is running and pipe is connected.
  bool IsRunning() const;

  // Shuts down the helper and cleans up.
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
```

- [ ] **Step 2: Create vpn_service_manager.cpp**

```cpp
#include "vpn_service_manager.h"

#include <shellapi.h>
#include <shlobj.h>

#include <sstream>

namespace vpn_plugin {

VpnServiceManager::VpnServiceManager() {
  // Unique pipe name per process to avoid collisions.
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

  // Clean up any previous state.
  Shutdown();
  return CreatePipeAndLaunch();
}

bool VpnServiceManager::CreatePipeAndLaunch() {
  // Create named pipe server.
  pipe_ = CreateNamedPipeA(
      pipe_name_.c_str(),
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,      // max instances
      4096,   // out buffer
      4096,   // in buffer
      0,      // default timeout
      nullptr // default security (elevated processes CAN connect)
  );
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
    // UAC declined or launch failed.
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    return false;
  }

  // Close the process handle — we don't need it, we detect death via pipe.
  if (sei.hProcess) CloseHandle(sei.hProcess);

  // Wait for helper to connect to the pipe.
  if (!ConnectNamedPipe(pipe_, nullptr)) {
    DWORD err = GetLastError();
    if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
      return false;
    }
  }

  running_.store(true);

  // Start background reader thread.
  reader_thread_ = std::thread(&VpnServiceManager::ReaderThreadFunc, this);

  // Wait briefly for READY message (reader thread will set ready_).
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
      // Pipe broken — helper died.
      OnHelperDied();
      return;
    }

    if (ch == '\n') {
      // Parse the line.
      if (buffer == "READY") {
        ready_.store(true);
      } else if (buffer.rfind("STATE ", 0) == 0) {
        int state = std::atoi(buffer.c_str() + 6);
        if (state_callback_) {
          state_callback_(state);
        }
      }
      // ERROR messages: silently ignored (could add logging).
      buffer.clear();
    } else if (ch != '\r') {
      buffer += ch;
    }
  }
}

void VpnServiceManager::OnHelperDied() {
  running_.store(false);
  ready_.store(false);
  // Notify plugin that VPN is disconnected.
  if (state_callback_) {
    state_callback_(0); // Disconnected
  }
}

void VpnServiceManager::Shutdown() {
  running_.store(false);
  ready_.store(false);

  if (pipe_ != INVALID_HANDLE_VALUE) {
    // Closing the pipe handle causes ReadFile in the reader thread to fail,
    // which will terminate the thread. It also breaks the pipe on the helper
    // side, causing it to exit.
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
```

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/vpn_service_manager.h plugins/vpn_plugin/windows/vpn_service_manager.cpp
git commit -m "feat(windows): add VpnServiceManager for named pipe IPC with elevated helper"
```

---

### Task 4: Update IVpnManagerImpl to use VpnServiceManager

Replace direct VpnEasyLoader usage with VpnServiceManager. Keep mock fallback.

**Files:**
- Modify: `plugins/vpn_plugin/windows/vpn_plugin.h`
- Modify: `plugins/vpn_plugin/windows/vpn_plugin.cpp`

- [ ] **Step 1: Update vpn_plugin.h**

Replace the full file:

```cpp
#ifndef FLUTTER_PLUGIN_VPN_PLUGIN_H_
#define FLUTTER_PLUGIN_VPN_PLUGIN_H_

#include <flutter/event_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "platform_api.g.h"
#include "vpn_service_manager.h"

namespace vpn_plugin {

// Streams VPN state changes to Dart via EventChannel.
class VpnEventStreamHandler
    : public flutter::StreamHandler<flutter::EncodableValue> {
 public:
  void EmitState(VpnManagerState state);

 protected:
  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnListenInternal(
      const flutter::EncodableValue* arguments,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
      override;

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnCancelInternal(const flutter::EncodableValue* arguments) override;

 private:
  std::mutex mutex_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
  std::optional<VpnManagerState> pending_state_;
};

// VPN manager using elevated vpn_service.exe helper when available,
// mock fallback otherwise.
class IVpnManagerImpl : public IVpnManager {
 public:
  IVpnManagerImpl(VpnEventStreamHandler* handler,
                  VpnServiceManager* service_manager);

  std::optional<FlutterError> Start(const std::string& server_name,
                                    const std::string& config) override;
  std::optional<FlutterError> Stop() override;
  std::optional<FlutterError> UpdateConfiguration(
      const std::string* server_name, const std::string* config) override;
  ErrorOr<VpnManagerState> GetCurrentState() override;

  void OnStateChanged(int state);

 private:
  VpnEventStreamHandler* handler_;
  VpnServiceManager* service_manager_;
  VpnManagerState state_ = VpnManagerState::kDisconnected;
};

// Stub implementation of the Pigeon-generated IDeepLink host API.
class IDeepLinkImpl : public IDeepLink {
 public:
  ErrorOr<std::string> Decode(const std::string& uri) override;
};

// Main plugin class — owns all components and registers with Flutter.
class VpnPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  VpnPlugin(
      std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
          event_channel,
      std::unique_ptr<VpnEventStreamHandler> handler,
      std::unique_ptr<VpnServiceManager> service_manager,
      std::unique_ptr<IVpnManagerImpl> vpn_manager,
      std::unique_ptr<IDeepLinkImpl> deep_link);

  ~VpnPlugin() override;

  VpnPlugin(const VpnPlugin&) = delete;
  VpnPlugin& operator=(const VpnPlugin&) = delete;

 private:
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<VpnEventStreamHandler> handler_;
  std::unique_ptr<VpnServiceManager> service_manager_;
  std::unique_ptr<IVpnManagerImpl> vpn_manager_;
  std::unique_ptr<IDeepLinkImpl> deep_link_;
};

}  // namespace vpn_plugin

#endif  // FLUTTER_PLUGIN_VPN_PLUGIN_H_
```

- [ ] **Step 2: Update vpn_plugin.cpp**

Replace the full file:

```cpp
#include "vpn_plugin.h"

#include <fstream>
#include <regex>

namespace vpn_plugin {

// Patch the config TOML to fix known incompatibilities between
// the Flutter ConfigurationEncoder output and vpn_easy expectations.
static std::string PatchConfig(const std::string& config) {
  std::string out = config;
  {
    std::regex re(R"(\n\[listener\]\s*\n)");
    out = std::regex_replace(out, re, "\n");
  }
  {
    std::regex re(R"(custom_sni\s*=\s*"[^"]*"\n?)");
    out = std::regex_replace(out, re, "");
  }
  {
    std::regex re(R"(upstream_fallback_protocol\s*=\s*"[^"]*"\n?)");
    out = std::regex_replace(out, re, "");
  }
  {
    std::regex re("\"http://([^\"]+)\"");
    out = std::regex_replace(out, re, "\"$1\"");
  }
  return out;
}

// Write string to a temp file, return the path.
static std::string WriteTempConfig(const std::string& config) {
  char temp_dir[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp_dir);
  char temp_file[MAX_PATH] = {};
  GetTempFileNameA(temp_dir, "tt_", 0, temp_file);
  std::ofstream f(temp_file);
  f << config;
  f.close();
  return std::string(temp_file);
}

// ---- VpnEventStreamHandler ----

void VpnEventStreamHandler::EmitState(VpnManagerState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_) {
    sink_->Success(
        flutter::EncodableValue(static_cast<int64_t>(state)));
    pending_state_.reset();
  } else {
    pending_state_ = state;
  }
}

std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
VpnEventStreamHandler::OnListenInternal(
    const flutter::EncodableValue* /*arguments*/,
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = std::move(events);
  if (pending_state_.has_value()) {
    sink_->Success(
        flutter::EncodableValue(static_cast<int64_t>(*pending_state_)));
    pending_state_.reset();
  }
  return nullptr;
}

std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
VpnEventStreamHandler::OnCancelInternal(
    const flutter::EncodableValue* /*arguments*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_.reset();
  return nullptr;
}

// ---- IVpnManagerImpl ----

IVpnManagerImpl::IVpnManagerImpl(VpnEventStreamHandler* handler,
                                 VpnServiceManager* service_manager)
    : handler_(handler), service_manager_(service_manager) {}

void IVpnManagerImpl::OnStateChanged(int state) {
  if (state < 0 || state > 5) state = 0;
  state_ = static_cast<VpnManagerState>(state);
  handler_->EmitState(state_);
}

std::optional<FlutterError> IVpnManagerImpl::Start(
    const std::string& /*server_name*/, const std::string& config) {
  if (service_manager_) {
    state_ = VpnManagerState::kConnecting;
    handler_->EmitState(state_);

    if (!service_manager_->LaunchIfNeeded()) {
      // UAC declined or launch failed.
      state_ = VpnManagerState::kDisconnected;
      handler_->EmitState(state_);
      return std::nullopt;
    }

    auto patched = PatchConfig(config);
    auto temp_path = WriteTempConfig(patched);
    service_manager_->SendStart(temp_path);
  } else {
    // Mock fallback.
    state_ = VpnManagerState::kConnected;
    handler_->EmitState(state_);
  }
  return std::nullopt;
}

std::optional<FlutterError> IVpnManagerImpl::Stop() {
  if (service_manager_ && service_manager_->IsRunning()) {
    service_manager_->SendStop();
  }
  state_ = VpnManagerState::kDisconnected;
  handler_->EmitState(state_);
  return std::nullopt;
}

std::optional<FlutterError> IVpnManagerImpl::UpdateConfiguration(
    const std::string* /*server_name*/, const std::string* /*config*/) {
  return std::nullopt;
}

ErrorOr<VpnManagerState> IVpnManagerImpl::GetCurrentState() {
  return state_;
}

// ---- IDeepLinkImpl ----

ErrorOr<std::string> IDeepLinkImpl::Decode(const std::string& /*uri*/) {
  return FlutterError("unimplemented",
                      "Deep link decoding is not supported on Windows");
}

// ---- VpnPlugin ----

void VpnPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto messenger = registrar->messenger();

  auto handler = std::make_unique<VpnEventStreamHandler>();

  auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          messenger, "vpn_plugin_event_channel",
          &flutter::StandardMethodCodec::GetInstance());
  event_channel->SetStreamHandler(
      std::unique_ptr<VpnEventStreamHandler>(handler.get()));

  // Create service manager for elevated helper IPC.
  auto service_manager = std::make_unique<VpnServiceManager>();

  auto vpn_manager = std::make_unique<IVpnManagerImpl>(
      handler.get(), service_manager.get());

  // Wire state callback: helper → OnStateChanged → EmitState → Dart.
  auto* mgr_ptr = vpn_manager.get();
  service_manager->SetStateCallback(
      [mgr_ptr](int state) { mgr_ptr->OnStateChanged(state); });

  auto deep_link = std::make_unique<IDeepLinkImpl>();

  IVpnManager::SetUp(messenger, vpn_manager.get());
  IDeepLink::SetUp(messenger, deep_link.get());

  registrar->AddPlugin(std::make_unique<VpnPlugin>(
      std::move(event_channel), std::move(handler),
      std::move(service_manager), std::move(vpn_manager),
      std::move(deep_link)));
}

VpnPlugin::VpnPlugin(
    std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
        event_channel,
    std::unique_ptr<VpnEventStreamHandler> handler,
    std::unique_ptr<VpnServiceManager> service_manager,
    std::unique_ptr<IVpnManagerImpl> vpn_manager,
    std::unique_ptr<IDeepLinkImpl> deep_link)
    : event_channel_(std::move(event_channel)),
      handler_(std::move(handler)),
      service_manager_(std::move(service_manager)),
      vpn_manager_(std::move(vpn_manager)),
      deep_link_(std::move(deep_link)) {}

VpnPlugin::~VpnPlugin() = default;

}  // namespace vpn_plugin
```

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/vpn_plugin.h plugins/vpn_plugin/windows/vpn_plugin.cpp
git commit -m "feat(windows): use VpnServiceManager in IVpnManagerImpl

Start() launches elevated helper on first call (UAC prompt), writes
patched config to temp file, sends START via named pipe. State
changes arrive via pipe reader thread. Subsequent connect/disconnect
cycles reuse the running helper without UAC."
```

---

### Task 5: Update plugin CMakeLists.txt

Add VpnServiceManager sources. Remove VpnEasyLoader (no longer used by plugin). Bundle vpn_service.exe.

**Files:**
- Modify: `plugins/vpn_plugin/windows/CMakeLists.txt`

- [ ] **Step 1: Update PLUGIN_SOURCES and bundled libraries**

Replace PLUGIN_SOURCES block:

```cmake
list(APPEND PLUGIN_SOURCES
  "vpn_plugin.cpp"
  "vpn_plugin.h"
  "vpn_service_manager.cpp"
  "vpn_service_manager.h"
  "platform_api.g.cpp"
  "platform_api.g.h"
)
```

Update bundled libraries to also include vpn_service.exe:

```cmake
# Pre-built native VPN library, Wintun driver, and elevated helper.
set(vpn_plugin_bundled_libraries "" PARENT_SCOPE)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bin/vpn_easy.dll")
  list(APPEND _bundled "${CMAKE_CURRENT_SOURCE_DIR}/bin/vpn_easy.dll")
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bin/wintun.dll")
  list(APPEND _bundled "${CMAKE_CURRENT_SOURCE_DIR}/bin/wintun.dll")
endif()
if (_bundled)
  set(vpn_plugin_bundled_libraries ${_bundled} PARENT_SCOPE)
endif()
```

Note: vpn_service.exe is installed via the runner CMakeLists (Task 2), not bundled via the plugin.

- [ ] **Step 2: Commit**

```bash
git add plugins/vpn_plugin/windows/CMakeLists.txt
git commit -m "build(windows): update plugin sources for VpnServiceManager"
```

---

### Task 6: Build and test on Windows

**Files:** None (build and integration test)

- [ ] **Step 1: Push all changes**

```bash
git push fork feature/windows-client
```

- [ ] **Step 2: Pull on Windows, regenerate Pigeon, and build**

```bash
ssh kinder@192.168.2.13 "... git pull && pigeon gen && flutter build windows --release"
```

- [ ] **Step 3: Verify vpn_service.exe is in Release/ next to vpn.exe**

- [ ] **Step 4: Test — launch vpn.exe (NOT as admin), press Connect**

Expected:
1. UAC prompt appears
2. User approves
3. VPN connects (state: Connecting → Connected)
4. Disconnect works
5. Re-connect works WITHOUT another UAC prompt
6. Close app → helper exits

- [ ] **Step 5: Test — decline UAC**

Expected: state returns to Disconnected, no crash

- [ ] **Step 6: Commit any fixes**
