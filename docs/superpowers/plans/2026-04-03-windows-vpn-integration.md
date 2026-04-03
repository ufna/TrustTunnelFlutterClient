# Windows VPN Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the mock VPN in the Windows Flutter plugin with a real tunnel by dynamically loading `vpn_easy.dll` from TrustTunnelClient.

**Architecture:** New `VpnEasyLoader` class handles `LoadLibrary`/`GetProcAddress` for `vpn_easy.dll`. `IVpnManagerImpl` uses it in `Start()`/`Stop()`, falling back to mock if DLL is absent. State callback from the native engine's background thread is forwarded to `EmitState()` which already buffers for late Dart subscribers.

**Tech Stack:** C++17, Windows API (LoadLibrary/GetProcAddress), Flutter Windows SDK, vpn_easy.dll (C API)

**Build machine:** va-kids (192.168.2.13), SSH: `ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13`

---

### Task 1: Install build prerequisites for TrustTunnelClient on va-kids

**Files:** None (environment setup)

- [ ] **Step 1: Install Python, Ninja, NASM, Perl via winget**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "powershell -Command \"winget install --id Python.Python.3.13 --accept-source-agreements --accept-package-agreements --silent; winget install --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements --silent; winget install --id NASM.NASM --accept-source-agreements --accept-package-agreements --silent; winget install --id StrawberryPerl.StrawberryPerl --accept-source-agreements --accept-package-agreements --silent\""
```

- [ ] **Step 2: Install Rust**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "powershell -Command \"Invoke-WebRequest -Uri https://win.rustup.rs/x86_64 -OutFile C:\Users\kinder\rustup-init.exe; Start-Process -Wait -FilePath C:\Users\kinder\rustup-init.exe -ArgumentList '-y'\""
```

- [ ] **Step 3: Install Conan via pip**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "powershell -Command \"pip install conan\""
```

- [ ] **Step 4: Verify all tools are available**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "set PATH=%PATH%;C:\Program Files\CMake\bin;C:\Users\kinder\.cargo\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM;C:\Ninja && python --version && cmake --version && ninja --version && nasm -v && perl -v && rustc --version && conan --version"
```

Expected: all tools report their versions.

---

### Task 2: Build vpn_easy.dll from TrustTunnelClient

**Files:** None (build on Windows machine)

- [ ] **Step 1: Clone TrustTunnelClient on va-kids (if not already present)**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "set PATH=%PATH%;C:\Program Files\Git\bin && cd /d C:\Users\kinder && git clone https://github.com/TrustTunnel/TrustTunnelClient.git 2>&1 || echo ALREADY_EXISTS"
```

- [ ] **Step 2: Bootstrap Conan dependencies**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "set PATH=%PATH%;C:\Program Files\Git\bin;C:\Users\kinder\AppData\Local\Programs\Python\Python313;C:\Users\kinder\AppData\Local\Programs\Python\Python313\Scripts && cd /d C:\Users\kinder\TrustTunnelClient && python scripts/bootstrap_conan_deps.py 2>&1"
```

- [ ] **Step 3: Configure CMake with MSVC + Ninja**

Run inside a VS2022 Developer Command Prompt environment:

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "\"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat\" -arch=amd64 && set PATH=%PATH%;C:\Ninja;C:\Users\kinder\.cargo\bin;C:\Strawberry\perl\bin;C:\Program Files\NASM && cd /d C:\Users\kinder\TrustTunnelClient && mkdir build 2>nul && cd build && cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl .. 2>&1"
```

- [ ] **Step 4: Build vpn_easy target**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "\"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat\" -arch=amd64 && cd /d C:\Users\kinder\TrustTunnelClient\build && cmake --build . --target vpn_easy 2>&1"
```

Expected: `vpn_easy.dll` produced in the build tree.

- [ ] **Step 5: Locate and verify the built DLL**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "dir /s /b C:\Users\kinder\TrustTunnelClient\build\vpn_easy.dll 2>&1"
```

- [ ] **Step 6: Download wintun.dll**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "powershell -Command \"Invoke-WebRequest -Uri https://www.wintun.net/builds/wintun-0.14.1.zip -OutFile C:\Users\kinder\wintun.zip; Expand-Archive -Path C:\Users\kinder\wintun.zip -DestinationPath C:\Users\kinder\wintun -Force; dir C:\Users\kinder\wintun\wintun\bin\amd64\wintun.dll\""
```

Expected: `wintun.dll` at `C:\Users\kinder\wintun\wintun\bin\amd64\wintun.dll`.

---

### Task 3: Create VpnEasyLoader (dynamic DLL loader)

**Files:**
- Create: `plugins/vpn_plugin/windows/vpn_easy_loader.h`
- Create: `plugins/vpn_plugin/windows/vpn_easy_loader.cpp`

- [ ] **Step 1: Create vpn_easy_loader.h**

```cpp
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
```

- [ ] **Step 2: Create vpn_easy_loader.cpp**

```cpp
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
```

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/vpn_easy_loader.h plugins/vpn_plugin/windows/vpn_easy_loader.cpp
git commit -m "feat(windows): add VpnEasyLoader for dynamic vpn_easy.dll loading"
```

---

### Task 4: Update IVpnManagerImpl to use VpnEasyLoader

**Files:**
- Modify: `plugins/vpn_plugin/windows/vpn_plugin.h`
- Modify: `plugins/vpn_plugin/windows/vpn_plugin.cpp`

- [ ] **Step 1: Update vpn_plugin.h**

Add `#include "vpn_easy_loader.h"` and update `IVpnManagerImpl`:

Replace the current `IVpnManagerImpl` class (lines 39-55) with:

```cpp
// VPN manager using vpn_easy.dll when available, mock fallback otherwise.
class IVpnManagerImpl : public IVpnManager {
 public:
  IVpnManagerImpl(VpnEventStreamHandler* handler, VpnEasyLoader* loader);

  std::optional<FlutterError> Start(const std::string& server_name,
                                    const std::string& config) override;
  std::optional<FlutterError> Stop() override;
  std::optional<FlutterError> UpdateConfiguration(
      const std::string* server_name, const std::string* config) override;
  ErrorOr<VpnManagerState> GetCurrentState() override;

  // Called from vpn_easy state callback (background thread).
  void OnStateChanged(VpnManagerState new_state);

 private:
  static void VpnStateCallback(void* arg, int state);

  VpnEventStreamHandler* handler_;
  VpnEasyLoader* loader_;
  VpnManagerState state_ = VpnManagerState::kDisconnected;
};
```

Update `VpnPlugin` to own the loader — add `std::unique_ptr<VpnEasyLoader> loader_;` to private members and update the constructor:

```cpp
class VpnPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  VpnPlugin(
      std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
          event_channel,
      std::unique_ptr<VpnEventStreamHandler> handler,
      std::unique_ptr<VpnEasyLoader> loader,
      std::unique_ptr<IVpnManagerImpl> vpn_manager,
      std::unique_ptr<IDeepLinkImpl> deep_link);

  ~VpnPlugin() override;

  VpnPlugin(const VpnPlugin&) = delete;
  VpnPlugin& operator=(const VpnPlugin&) = delete;

 private:
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<VpnEventStreamHandler> handler_;
  std::unique_ptr<VpnEasyLoader> loader_;
  std::unique_ptr<IVpnManagerImpl> vpn_manager_;
  std::unique_ptr<IDeepLinkImpl> deep_link_;
};
```

- [ ] **Step 2: Update vpn_plugin.cpp**

Replace `IVpnManagerImpl` implementation:

```cpp
// ---- IVpnManagerImpl ----

IVpnManagerImpl::IVpnManagerImpl(VpnEventStreamHandler* handler,
                                 VpnEasyLoader* loader)
    : handler_(handler), loader_(loader) {}

void IVpnManagerImpl::VpnStateCallback(void* arg, int state) {
  auto* self = static_cast<IVpnManagerImpl*>(arg);
  // Clamp to valid range, default to disconnected.
  if (state < 0 || state > 5) state = 0;
  self->OnStateChanged(static_cast<VpnManagerState>(state));
}

void IVpnManagerImpl::OnStateChanged(VpnManagerState new_state) {
  state_ = new_state;
  handler_->EmitState(state_);
}

std::optional<FlutterError> IVpnManagerImpl::Start(
    const std::string& /*server_name*/, const std::string& config) {
  if (loader_ && loader_->IsLoaded()) {
    // Real VPN: vpn_easy_start is async — state changes arrive via callback.
    state_ = VpnManagerState::kConnecting;
    handler_->EmitState(state_);
    loader_->Start(config.c_str(), &VpnStateCallback, this);
  } else {
    // Mock fallback when DLL is not available.
    state_ = VpnManagerState::kConnected;
    handler_->EmitState(state_);
  }
  return std::nullopt;
}

std::optional<FlutterError> IVpnManagerImpl::Stop() {
  if (loader_ && loader_->IsLoaded()) {
    loader_->Stop();
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
```

Update `RegisterWithRegistrar` to create and use the loader:

```cpp
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

  // Try to load vpn_easy.dll from the executable's directory.
  auto loader = std::make_unique<VpnEasyLoader>();
  loader->Load("vpn_easy.dll");

  auto vpn_manager =
      std::make_unique<IVpnManagerImpl>(handler.get(), loader.get());
  auto deep_link = std::make_unique<IDeepLinkImpl>();

  IVpnManager::SetUp(messenger, vpn_manager.get());
  IDeepLink::SetUp(messenger, deep_link.get());

  registrar->AddPlugin(std::make_unique<VpnPlugin>(
      std::move(event_channel), std::move(handler), std::move(loader),
      std::move(vpn_manager), std::move(deep_link)));
}

VpnPlugin::VpnPlugin(
    std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
        event_channel,
    std::unique_ptr<VpnEventStreamHandler> handler,
    std::unique_ptr<VpnEasyLoader> loader,
    std::unique_ptr<IVpnManagerImpl> vpn_manager,
    std::unique_ptr<IDeepLinkImpl> deep_link)
    : event_channel_(std::move(event_channel)),
      handler_(std::move(handler)),
      loader_(std::move(loader)),
      vpn_manager_(std::move(vpn_manager)),
      deep_link_(std::move(deep_link)) {}
```

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/vpn_plugin.h plugins/vpn_plugin/windows/vpn_plugin.cpp
git commit -m "feat(windows): integrate vpn_easy.dll via VpnEasyLoader in IVpnManagerImpl

Start() calls vpn_easy_start() with TOML config when DLL is loaded,
falls back to mock otherwise. State callback from background thread
forwarded to EmitState(). Stop() calls vpn_easy_stop()."
```

---

### Task 5: Update CMakeLists.txt — add loader sources and bundled DLLs

**Files:**
- Modify: `plugins/vpn_plugin/windows/CMakeLists.txt`

- [ ] **Step 1: Add loader sources to PLUGIN_SOURCES**

Replace the `PLUGIN_SOURCES` block:

```cmake
list(APPEND PLUGIN_SOURCES
  "vpn_plugin.cpp"
  "vpn_plugin.h"
  "vpn_easy_loader.cpp"
  "vpn_easy_loader.h"
  "platform_api.g.cpp"
  "platform_api.g.h"
)
```

- [ ] **Step 2: Update bundled libraries**

Replace the `vpn_plugin_bundled_libraries` block:

```cmake
# Pre-built native VPN library and Wintun driver.
# Place vpn_easy.dll and wintun.dll in the bin/ subdirectory.
# If absent, the plugin falls back to mock VPN behaviour.
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

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/CMakeLists.txt
git commit -m "build(windows): add VpnEasyLoader sources and conditional DLL bundling"
```

---

### Task 6: Update test file

**Files:**
- Modify: `plugins/vpn_plugin/windows/test/vpn_plugin_test.cpp`

- [ ] **Step 1: Rewrite tests for new constructor**

```cpp
#include <gtest/gtest.h>

#include "vpn_plugin.h"

namespace vpn_plugin {
namespace test {

TEST(VpnPlugin, InitialStateIsDisconnected) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  // nullptr loader = mock mode.
  IVpnManagerImpl manager(handler.get(), nullptr);
  auto result = manager.GetCurrentState();
  ASSERT_FALSE(result.has_error());
  EXPECT_EQ(result.value(), VpnManagerState::kDisconnected);
}

TEST(VpnPlugin, StartWithoutDllFallsBackToMock) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  IVpnManagerImpl manager(handler.get(), nullptr);
  auto err = manager.Start("test-server", "[endpoint]\nhostname=\"test\"");
  EXPECT_FALSE(err.has_value());
  // Mock goes straight to connected.
  auto result = manager.GetCurrentState();
  EXPECT_EQ(result.value(), VpnManagerState::kConnected);
}

TEST(VpnPlugin, StopReturnsDisconnected) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  IVpnManagerImpl manager(handler.get(), nullptr);
  manager.Start("test-server", "");
  auto err = manager.Stop();
  EXPECT_FALSE(err.has_value());
  auto result = manager.GetCurrentState();
  EXPECT_EQ(result.value(), VpnManagerState::kDisconnected);
}

TEST(VpnPlugin, OnStateChangedUpdatesState) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  IVpnManagerImpl manager(handler.get(), nullptr);
  manager.OnStateChanged(VpnManagerState::kConnected);
  auto result = manager.GetCurrentState();
  EXPECT_EQ(result.value(), VpnManagerState::kConnected);
}

TEST(VpnPlugin, DeepLinkReturnsUnimplemented) {
  IDeepLinkImpl deep_link;
  auto result = deep_link.Decode("trusttunnel://example");
  EXPECT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unimplemented");
}

TEST(VpnEasyLoader, LoadNonExistentDllReturnsFalse) {
  VpnEasyLoader loader;
  EXPECT_FALSE(loader.Load("nonexistent_vpn_easy.dll"));
  EXPECT_FALSE(loader.IsLoaded());
}

}  // namespace test
}  // namespace vpn_plugin
```

- [ ] **Step 2: Commit**

```bash
git add plugins/vpn_plugin/windows/test/vpn_plugin_test.cpp
git commit -m "test(windows): update tests for VpnEasyLoader integration"
```

---

### Task 7: Add bin/ directory and gitignore

**Files:**
- Create: `plugins/vpn_plugin/windows/bin/.gitkeep`
- Modify: `plugins/vpn_plugin/windows/.gitignore`

- [ ] **Step 1: Create bin/ staging directory**

```bash
mkdir -p plugins/vpn_plugin/windows/bin
touch plugins/vpn_plugin/windows/bin/.gitkeep
```

- [ ] **Step 2: Add gitignore for DLLs in bin/**

Append to `plugins/vpn_plugin/windows/.gitignore`:

```
# Pre-built native DLLs (not committed — placed during build/distribution)
bin/*.dll
```

- [ ] **Step 3: Commit**

```bash
git add plugins/vpn_plugin/windows/bin/.gitkeep plugins/vpn_plugin/windows/.gitignore
git commit -m "build(windows): add bin/ staging directory for native DLLs"
```

---

### Task 8: Build, deploy DLLs, and test

**Files:** None (integration testing on Windows)

- [ ] **Step 1: Push all changes**

```bash
git push fork feature/windows-client
```

- [ ] **Step 2: Pull on Windows and regenerate Pigeon**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "set PATH=%PATH%;C:\flutter\bin;C:\Program Files\Git\bin && cd /d C:\Users\kinder\TrustTunnelFlutterClient && git pull origin feature/windows-client 2>&1 && cd plugins\vpn_plugin && dart run pigeon --input pigeons/platform_api.dart --cpp_header_out windows/platform_api.g.h --cpp_source_out windows/platform_api.g.cpp --cpp_namespace vpn_plugin 2>&1"
```

- [ ] **Step 3: Copy vpn_easy.dll and wintun.dll into bin/**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "copy C:\Users\kinder\TrustTunnelClient\build\platform\windows\vpn_easy.dll C:\Users\kinder\TrustTunnelFlutterClient\plugins\vpn_plugin\windows\bin\vpn_easy.dll && copy C:\Users\kinder\wintun\wintun\bin\amd64\wintun.dll C:\Users\kinder\TrustTunnelFlutterClient\plugins\vpn_plugin\windows\bin\wintun.dll && dir C:\Users\kinder\TrustTunnelFlutterClient\plugins\vpn_plugin\windows\bin\*.dll"
```

Note: the exact path to `vpn_easy.dll` in the build tree may vary. Use `dir /s /b C:\Users\kinder\TrustTunnelClient\build\vpn_easy.dll` to find it.

- [ ] **Step 4: Build Flutter Windows release**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "set PATH=%PATH%;C:\flutter\bin;C:\Program Files\Git\bin && cd /d C:\Users\kinder\TrustTunnelFlutterClient && flutter build windows --release 2>&1"
```

Expected: `vpn.exe` built with `vpn_easy.dll` and `wintun.dll` in the Release directory.

- [ ] **Step 5: Verify DLLs are bundled next to vpn.exe**

```bash
ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13 "dir C:\Users\kinder\TrustTunnelFlutterClient\build\windows\x64\runner\Release\*.dll"
```

Expected: `vpn_easy.dll` and `wintun.dll` listed alongside `flutter_windows.dll` etc.

- [ ] **Step 6: Test real VPN connection**

Run `vpn.exe` **as Administrator** on va-kids. Configure a server with:
- Server Name: vpn_tt_47
- IP: 212.34.136.215:443
- Domain: t1.srv01.trusttunnel.me
- Username: ListnAafpY
- Password: Nke1jjP1Cg1R
- Protocol: HTTP/2
- DNS: 9.9.9.9

Tap Connect. Expected: state transitions Disconnected → Connecting → Connected with actual VPN tunnel.

- [ ] **Step 7: If build/runtime issues, diagnose and fix**

Common issues:
- `vpn_easy.dll` not found → verify it's in Release/ directory
- State callback not received → check that `VpnStateCallback` is being called (add OutputDebugString for debugging)
- Wintun error → verify `wintun.dll` is in the same directory and process runs as Administrator
- Config parsing error → the TOML from ConfigurationEncoder may need adjustments (check vpn_easy stderr output)
