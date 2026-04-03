# Elevated Helper Process Design

## Goal

Request administrator privileges only when the user connects to VPN, not at app launch. A separate elevated helper process (`vpn_service.exe`) manages the VPN engine, communicating with the main Flutter app via named pipe.

## Architecture

```
vpn.exe (normal user)                    vpn_service.exe (administrator)
    │                                         │
    │── Creates named pipe ──────────────────>│
    │── ShellExecuteEx("runas") ────────────>│ [UAC prompt, once per session]
    │                                         │── Connects to pipe
    │                                         │── LoadLibrary("vpn_easy.dll")
    │                                         │
    │── "START C:\tmp\config1.toml" ────────>│── vpn_easy_start(config)
    │<── "STATE 1" ──────────────────────────│── (Connecting)
    │<── "STATE 2" ──────────────────────────│── (Connected)
    │                                         │
    │── "STOP" ─────────────────────────────>│── vpn_easy_stop()
    │<── "STATE 0" ──────────────────────────│── (Disconnected)
    │                                         │── Waits for next command...
    │                                         │
    │── "START C:\tmp\config2.toml" ────────>│── vpn_easy_start(new config)
    │<── "STATE 1" ──────────────────────────│   (no UAC prompt!)
    │<── "STATE 2" ──────────────────────────│
    │                                         │
    │── [app closes, pipe breaks] ──────────>│── Detects broken pipe, exits
```

## IPC Protocol

Text-based, line-delimited, over a single bidirectional named pipe (`\\.\pipe\trusttunnel_vpn_<pid>`).

### Plugin → Helper

| Command | Description |
|---------|-------------|
| `START <config_path>` | Start VPN with TOML config at given file path |
| `STOP` | Stop VPN |

### Helper → Plugin

| Message | Description |
|---------|-------------|
| `STATE <0-5>` | VPN state changed (maps to VpnManagerState enum) |
| `ERROR <text>` | Error message (informational, logged) |
| `READY` | Helper connected to pipe and loaded DLL, ready for commands |

## Components

### vpn_service.exe (new standalone executable)

Minimal C++ console application (~150 lines). Lifecycle:
1. Receives pipe name as command-line argument
2. Connects to the named pipe
3. Loads `vpn_easy.dll` via the same `VpnEasyLoader` mechanism
4. Sends `READY` (or `ERROR` if DLL load failed)
5. Reads commands in a loop:
   - `START <path>`: reads TOML from file, calls `vpn_easy_start()`, state changes forwarded as `STATE <n>`
   - `STOP`: calls `vpn_easy_stop()`
6. On pipe disconnect (app closed): calls `vpn_easy_stop()` if running, exits

Built as a separate CMake target in the Windows runner directory, bundled next to `vpn.exe`.

### VpnServiceManager (new class in plugin)

Manages the helper process lifecycle and pipe communication. Replaces direct `VpnEasyLoader` usage in `IVpnManagerImpl`.

**Interface:**
- `LaunchIfNeeded()` — creates pipe, launches helper via ShellExecuteEx+runas if not running. Returns false if UAC declined.
- `SendStart(config_path)` — sends START command
- `SendStop()` — sends STOP command
- `IsRunning()` — checks if helper is alive
- `SetStateCallback(fn)` — sets callback for STATE messages from helper
- `Shutdown()` — closes pipe, helper exits

**Pipe reader:** Background thread reads lines from the named pipe, parses `STATE <n>` messages, invokes state callback.

### Changes to IVpnManagerImpl

- Constructor takes `VpnServiceManager*` instead of `VpnEasyLoader*`
- `Start()`:
  1. Writes patched TOML config to temp file
  2. Calls `manager->LaunchIfNeeded()` (UAC prompt on first call)
  3. Calls `manager->SendStart(temp_path)`
  4. State changes arrive via pipe → callback → EmitState()
- `Stop()`: calls `manager->SendStop()`
- Mock fallback remains when vpn_easy.dll is absent (helper won't launch)

## Error Handling

| Scenario | Behavior |
|----------|----------|
| UAC declined | `LaunchIfNeeded()` returns false, plugin emits Disconnected |
| Helper crashes | Pipe reader detects broken pipe, emits Disconnected |
| App closes | Pipe closes, helper detects and exits cleanly |
| vpn_easy.dll missing | Helper sends `ERROR`, plugin falls back to mock |
| Config file unreadable | Helper sends `ERROR` + `STATE 0` |

## Security

- Named pipe name includes PID (`\\.\pipe\trusttunnel_vpn_<pid>`) to avoid collisions
- Pipe created with default security descriptor — elevated processes can connect to pipes created by standard users
- Config written to user's temp directory (standard permissions)
- Helper only accepts commands from the pipe it was given at launch

## File Summary

| Action | File |
|--------|------|
| Create | `windows/runner/vpn_service.cpp` — helper exe entry point |
| Create | `plugins/vpn_plugin/windows/vpn_service_manager.h` — IPC + process management |
| Create | `plugins/vpn_plugin/windows/vpn_service_manager.cpp` — implementation |
| Modify | `plugins/vpn_plugin/windows/vpn_plugin.h` — use VpnServiceManager |
| Modify | `plugins/vpn_plugin/windows/vpn_plugin.cpp` — use VpnServiceManager in Start/Stop |
| Modify | `windows/runner/CMakeLists.txt` — add vpn_service.exe target |
| Modify | `plugins/vpn_plugin/windows/CMakeLists.txt` — add manager sources, bundle vpn_service.exe |
