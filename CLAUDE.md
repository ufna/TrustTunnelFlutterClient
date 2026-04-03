# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TrustTunnel Flutter Client — cross-platform VPN client (Android, iOS, macOS, Linux, Windows) for self-hosted TrustTunnel VPN servers. Built with Flutter 3.38.3 / Dart 3.10.1.

## Build Commands

```shell
make init                    # Full project init: clean, pub get, build_runner, intl_utils, plugin init
make gen                     # Code generation: build_runner + Pigeon (Swift/Kotlin/C++ bindings)
make ln                      # Generate localizations only
flutter analyze              # Lint (strict-casts, strict-raw-types enabled)
flutter build windows        # Windows desktop build
flutter build appbundle      # Android AAB release
flutter run                  # Dev run on connected device
```

**Plugin code generation** (from repo root `make gen` runs this too):
```shell
cd plugins/vpn_plugin && make gen   # Pigeon: generates Swift (iOS/macOS) + C++ (Windows) bindings
```

**Code generation tools:**
- `dart run build_runner build --delete-conflicting-outputs` — Drift ORM schema
- `dart run intl_utils:generate` — Localization from ARB files
- `dart run pigeon --input pigeons/platform_api.dart` — Platform channel bindings

**Environment:** `GPR_KEY` env var (GitHub PAT with `read:packages`) required for Android/iOS native dependency resolution.

## Architecture

### Two-Layer Design

**Flutter App** (`lib/`) — UI, navigation, business logic, configuration management.
**VPN Plugin** (`plugins/vpn_plugin/`) — Platform-native VPN integration via Pigeon-generated APIs. Fully decoupled from the app; reusable in other Flutter apps.

### App Structure (`lib/`)

- **`feature/`** — Feature modules: `server/`, `routing/`, `vpn/`, `settings/`, `deep_link/`, `navigation/`, `app/`
- **`data/`** — Data layer: `database/` (Drift/SQLite), `model/`, `repository/`, `datasources/` (local + native)
- **`common/`** — Shared: `controller/` (state management), `error/`, `localization/`, `router/`, `theme/`, `extensions/`
- **`di/`** — Dependency injection via `InheritedWidget` (`DependencyScope`), lazy singleton factories
- **`widgets/`** — Shared UI components

### State Management — Custom Controller Pattern

No BLoC/Provider/Riverpod. Custom pattern built on `ChangeNotifier`:

- **`BaseController`** → `ChangeNotifier` with `handle()` for async work scheduling
- **Concurrency handlers** (mixins): `SequentialControllerHandler`, `DroppableControllerHandler`, `RestartableControllerHandler`
- **`StateController<T>`** — generic state holder with `state`/`setState()`
- **Scope widgets** — `InheritedModel`-based (e.g., `ServersScope`, `VpnScope`, `RoutingScope`) with aspects for granular rebuilds
- **`StateConsumer`** — simplified `ListenableBuilder` for controllers

### Plugin / Platform Channel Flow

1. UI → Repository → DataSource (native) → `VpnPlugin` → Pigeon EventChannel → Native platform code
2. Native responses stream back through `VpnPlugin.states` / `VpnPlugin.queryLog`
3. **Pigeon schema**: `plugins/vpn_plugin/pigeons/platform_api.dart`
4. **Generated bindings**: Dart (`lib/platform_api.g.dart`), C++ (`windows/platform_api.g.*`), Kotlin, Swift

### Database (Drift/SQLite)

Schema version 3. Tables defined in `lib/data/database/tables/*.drift`. Key tables: servers, routing_profiles, profile_rules, excluded_routes, protocols, dns_servers, certificates, vpn_requests.

### DI Pattern

`DependencyFactory` and `RepositoryFactory` as lazy singleton containers, provided through `DependencyScope` (InheritedWidget). Access via `context.dependencyFactory` / `context.repositoryFactory`.

### Localization

ARB files in `lib/common/localization/arb/`. Generated class: `AppLocalizations`. Access: `context.ln.keyName` or `Localization.ln.keyName`.

## Code Style

- **Formatter**: line width 120, trailing commas preserved
- **Strict analysis**: `strict-casts: true`, `strict-raw-types: true`, `dead_code: error`
- **Single quotes** preferred
- **Member ordering** enforced by `dart_code_metrics`: static fields → final fields → constructors → static methods → late fields → getters/setters → methods (init → overridden → public → dispose → private)
- **Widget ordering**: initState → didChangeDependencies → didUpdateWidget → build → public → private → dispose
- **Naming**: State classes end with `State`, Event with `Event`, Bloc with `Bloc`
- **No curly braces** in single-statement flow control (`curly_braces_in_flow_control_structures: false`)
- **Pinned dependency versions** in pubspec.yaml

## Windows Desktop

### Architecture

The Windows plugin dynamically loads `vpn_easy.dll` (from TrustTunnelClient native library) at startup via `VpnEasyLoader` (LoadLibrary/GetProcAddress). If the DLL is absent, it falls back to a mock that immediately reports Connected state.

```
Flutter UI → Pigeon IVpnManager → IVpnManagerImpl (C++)
                                       ↓
                              VpnEasyLoader (LoadLibrary)
                                       ↓
                              vpn_easy_start(toml_config, state_callback, this)
                                       ↓
                              VPN Engine → Wintun TUN device
                                       ↓
                              state_callback → EmitState() → Dart EventChannel
```

### Plugin Files (`plugins/vpn_plugin/windows/`)

| File | Purpose |
|------|---------|
| `vpn_plugin.h/cpp` | IVpnManager + IDeepLink implementations, VpnEventStreamHandler, VpnPlugin registration |
| `vpn_easy_loader.h/cpp` | Dynamic DLL loading wrapper for vpn_easy.dll |
| `platform_api.g.h/cpp` | Pigeon-generated C++ bindings (generated, gitignored) |
| `vpn_plugin_c_api.cpp` | C export for Flutter plugin registration |
| `include/vpn_plugin/vpn_plugin.h` | Public header matching Flutter's generated_plugin_registrant expectations |
| `bin/` | Staging directory for pre-built DLLs (vpn_easy.dll, wintun.dll — gitignored) |

### vpn_easy.dll C API

```c
typedef void (*on_state_changed_t)(void *arg, int new_state_description);
void vpn_easy_start(const char *toml_config, on_state_changed_t cb, void *cb_arg);
void vpn_easy_stop();
```

State values map 1:1 to Pigeon `VpnManagerState` enum (0=Disconnected, 1=Connecting, 2=Connected, 3=WaitingForRecovery, 4=Recovering, 5=WaitingForNetwork).

### TOML Config

The Flutter `ConfigurationEncoder` produces the TOML string passed to `vpn_easy_start()`. The C++ plugin applies `PatchConfig()` to fix known incompatibilities:
- Removes empty `[listener]` section (encoder emits it before `[listener.tun]`)
- Removes `custom_sni` and `upstream_fallback_protocol` keys (not in vpn_easy schema)
- Strips `http://` prefix from DNS upstreams (not a valid DNS protocol)

### EventChannel Threading

The `vpn_easy.dll` state callback fires from a **background executor thread**. `VpnEventStreamHandler::EmitState()` handles this with `pending_state_` buffering: if the Dart EventChannel sink isn't attached yet (Dart subscribes AFTER calling `start()`), the state is buffered and flushed in `OnListenInternal()` when the subscription arrives.

### Building for Windows

**Prerequisites on build machine:**
- Visual Studio 2022 (Desktop C++ workload + Windows SDK)
- Flutter SDK 3.38.3
- Git

**Building vpn_easy.dll** (from TrustTunnelClient repo):
- Additional prerequisites: Python 3.13+, Conan 2+, Rust 1.85+, Ninja, Strawberry Perl, NASM
- Build from `TrustTunnelClient/platform/windows/`:
  ```shell
  python scripts/bootstrap_conan_deps.py   # one-time
  cd platform/windows && mkdir build && cd build
  cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ..
  cmake --build . --target vpn_easy
  ```

**Building the Flutter app:**
1. Place `vpn_easy.dll` and `wintun.dll` (from wintun.net, amd64) into `plugins/vpn_plugin/windows/bin/`
2. Generate Pigeon C++ bindings: `cd plugins/vpn_plugin && make gen`
3. Generate Drift + localization: `dart run build_runner build --delete-conflicting-outputs && dart run intl_utils:generate`
4. Build: `flutter build windows --release`
5. Output: `build/windows/x64/runner/Release/vpn.exe`

**Runtime requirements:**
- `vpn_easy.dll` + `wintun.dll` next to `vpn.exe` (Flutter bundles them from `bin/`)
- **Administrator privileges** required (Wintun creates a virtual network adapter)

### Windows Build Machine (va-kids)

- Host: DESKTOP-590NICV (192.168.2.13)
- SSH: `ssh -i ~/.ssh/id_rsa_arch kinder@192.168.2.13`
- OS: Windows 11 Pro, Build 26200, x64
- All build tools installed (VS2022, Flutter, Python, Conan, Rust, Ninja, Perl, NASM)
- TrustTunnelClient clone: `C:\Users\kinder\TrustTunnelClient`
- Flutter client clone: `C:\Users\kinder\TrustTunnelFlutterClient`
