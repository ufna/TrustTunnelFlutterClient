# Windows VPN Integration Design

## Goal

Replace the mock VPN implementation in the Windows Flutter plugin with a real VPN tunnel by dynamically loading `vpn_easy.dll` from the TrustTunnelClient native library.

## Architecture

```
Flutter UI → Pigeon IVpnManager → IVpnManagerImpl (C++)
                                       ↓
                              VpnEasyLoader (LoadLibrary)
                                       ↓
                              vpn_easy_start(toml, callback, this)
                                       ↓
                              VPN Engine → Wintun TUN device
                                       ↓
                              callback(arg, state) → EmitState()
```

## API Contract

The `vpn_easy.dll` exports two functions:

```c
typedef void (*on_state_changed_t)(void *arg, int new_state_description);

void vpn_easy_start(const char *toml_config, on_state_changed_t cb, void *cb_arg);
void vpn_easy_stop();
```

State values (VpnSessionState) map 1:1 to the Pigeon VpnManagerState enum:
- 0 = Disconnected
- 1 = Connecting
- 2 = Connected
- 3 = WaitingForRecovery
- 4 = Recovering
- 5 = WaitingForNetwork

## Configuration

The Flutter `ConfigurationEncoder` already produces the exact TOML format expected by `vpn_easy_start()`:
- `[endpoint]` section: hostname, addresses, username, password, upstream_protocol, certificate, etc.
- `[listener.tun]` section: included_routes, excluded_routes, mtu_size
- Top-level: dns_upstreams, exclusions, vpn_mode, loglevel, killswitch_enabled

No changes to the Dart layer are needed.

## Dynamic Loading Approach

Use `LoadLibrary`/`GetProcAddress` instead of compile-time linking:

**Benefits:**
- Flutter plugin compiles independently — no TrustTunnelClient build chain dependency
- `vpn_easy.dll` and `wintun.dll` are simply bundled alongside `vpn.exe`
- Graceful fallback to mock if DLL is absent (development/testing without native lib)

**New file: `vpn_easy_loader.h`**
- `VpnEasyLoader` class wrapping LoadLibrary/GetProcAddress
- `Load(dll_path)` — loads DLL, resolves function pointers
- `Start(config, callback, arg)` — proxies to `vpn_easy_start`
- `Stop()` — proxies to `vpn_easy_stop`
- `IsLoaded()` — checks if DLL was successfully loaded

## Threading

The state callback from `vpn_easy.dll` is invoked from a **background executor thread** (not the calling thread). The `VpnEventStreamHandler::EmitState()` method already handles this via `pending_state_` buffering — state is stored when `sink_` is null and flushed when Dart subscribes via `OnListenInternal()`.

## Plugin Changes

**`IVpnManagerImpl::Start(server_name, config)`:**
1. If `VpnEasyLoader::IsLoaded()`: call `vpn_easy_start(config.c_str(), state_callback, this)`
2. Else: fall back to mock (immediate connected state)
3. The `config` parameter is the TOML string produced by Dart's `ConfigurationEncoder`

**`IVpnManagerImpl::Stop()`:**
1. If `VpnEasyLoader::IsLoaded()`: call `vpn_easy_stop()`
2. Emit `kDisconnected` via `EmitState()`

**Static callback function:**
```cpp
static void OnVpnStateChanged(void* arg, int state) {
    auto* self = static_cast<IVpnManagerImpl*>(arg);
    self->OnStateChanged(static_cast<VpnManagerState>(state));
}
```

## Bundling

Add `vpn_easy.dll` and `wintun.dll` to `vpn_plugin_bundled_libraries` in `CMakeLists.txt`. Flutter copies bundled libraries to the output directory next to `vpn.exe`.

Placeholder paths (resolved at build time):
```cmake
set(vpn_plugin_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/bin/vpn_easy.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/bin/wintun.dll"
  PARENT_SCOPE
)
```

The `bin/` directory inside the plugin's `windows/` folder contains pre-built binaries. These are not committed to git — they are placed there as part of the build/distribution process.

## Build Steps for vpn_easy.dll

On the Windows build machine (va-kids), prerequisites beyond VS2022:
- Python 3.13+, Conan 2.0.5+, Rust 1.85+, Ninja, Perl (Strawberry), NASM

Build:
```
cd TrustTunnelClient
python scripts/bootstrap_conan_deps.py
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ..
cmake --build . --target vpn_easy
```

Output: `vpn_easy.dll` in the build tree.

## Runtime Requirements

- **Administrator privileges** — Wintun TUN device requires elevation
- `vpn_easy.dll` — next to `vpn.exe`
- `wintun.dll` — next to `vpn.exe` (matching architecture: x64)

## File Summary

| Action | File |
|--------|------|
| Create | `plugins/vpn_plugin/windows/vpn_easy_loader.h` |
| Create | `plugins/vpn_plugin/windows/vpn_easy_loader.cpp` |
| Modify | `plugins/vpn_plugin/windows/vpn_plugin.h` |
| Modify | `plugins/vpn_plugin/windows/vpn_plugin.cpp` |
| Modify | `plugins/vpn_plugin/windows/CMakeLists.txt` |
| Create | `plugins/vpn_plugin/windows/bin/` (DLL staging directory, gitignored) |
