#include "vpn_plugin.h"

#include <algorithm>
#include <fstream>
#include <regex>

namespace vpn_plugin {

// Debug: write text to a log file in a known writable location.
static void DebugLog(const std::string& msg) {
  std::ofstream f("C:\\Users\\kinder\\vpn_plugin_debug.log", std::ios::app);
  f << msg << std::endl;
}

// Redirect stderr to a file so vpn_easy log output is captured.
static void RedirectStderrToFile() {
  static bool done = false;
  if (!done) {
    FILE* f = nullptr;
    freopen_s(&f, "C:\\Users\\kinder\\vpn_easy_stderr.log", "a", stderr);
    done = true;
  }
}

// Patch the config TOML to fix known incompatibilities between
// the Flutter ConfigurationEncoder output and vpn_easy expectations.
static std::string PatchConfig(const std::string& config) {
  std::string out = config;

  // Remove standalone [listener] line (empty parent table before [listener.tun]).
  // vpn_easy's TOML parser may choke on the empty section.
  {
    std::regex re(R"(\n\[listener\]\s*\n)");
    out = std::regex_replace(out, re, "\n");
  }

  // Remove custom_sni key (not in vpn_easy TOML schema).
  {
    std::regex re(R"(custom_sni\s*=\s*"[^"]*"\n?)");
    out = std::regex_replace(out, re, "");
  }

  // Remove upstream_fallback_protocol key (not in vpn_easy TOML schema).
  {
    std::regex re(R"(upstream_fallback_protocol\s*=\s*"[^"]*"\n?)");
    out = std::regex_replace(out, re, "");
  }

  // Fix DNS upstreams: strip "http://" prefix which is invalid for DNS.
  // Valid formats: plain ("9.9.9.9"), tls:// , https:// (for DoH), quic://
  {
    std::regex re("\"http://([^\"]+)\"");
    out = std::regex_replace(out, re, "\"$1\"");
  }

  return out;
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
                                 VpnEasyLoader* loader)
    : handler_(handler), loader_(loader) {}

void IVpnManagerImpl::VpnStateCallback(void* arg, int state) {
  DebugLog("VpnStateCallback: state=" + std::to_string(state));
  auto* self = static_cast<IVpnManagerImpl*>(arg);
  if (state < 0 || state > 5) state = 0;
  self->OnStateChanged(static_cast<VpnManagerState>(state));
}

void IVpnManagerImpl::OnStateChanged(VpnManagerState new_state) {
  state_ = new_state;
  handler_->EmitState(state_);
}

std::optional<FlutterError> IVpnManagerImpl::Start(
    const std::string& /*server_name*/, const std::string& config) {
  DebugLog("=== Start() called ===");
  DebugLog("DLL loaded: " + std::string(loader_ && loader_->IsLoaded() ? "YES" : "NO"));
  DebugLog("Config length: " + std::to_string(config.size()));
  DebugLog("Config:\n" + config);

  if (loader_ && loader_->IsLoaded()) {
    RedirectStderrToFile();

    auto patched = PatchConfig(config);
    DebugLog("Patched config:\n" + patched);

    state_ = VpnManagerState::kConnecting;
    handler_->EmitState(state_);
    DebugLog("Calling vpn_easy_start...");
    loader_->Start(patched.c_str(), &VpnStateCallback, this);
    DebugLog("vpn_easy_start returned");
  } else {
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

VpnPlugin::~VpnPlugin() = default;

}  // namespace vpn_plugin
