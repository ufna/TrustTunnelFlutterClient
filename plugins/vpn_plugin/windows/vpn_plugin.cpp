#include "vpn_plugin.h"

#include <fstream>
#include <regex>
#include <thread>

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

    // Run launch + start on a background thread to avoid blocking the
    // Flutter platform thread (ConnectNamedPipe + UAC prompt can block).
    auto patched = PatchConfig(config);
    auto* mgr = service_manager_;
    auto* self = this;
    std::thread([mgr, self, patched]() {
      if (!mgr->LaunchIfNeeded()) {
        // UAC declined or launch failed.
        self->OnStateChanged(0);  // Disconnected
        return;
      }
      auto temp_path = WriteTempConfig(patched);
      mgr->SendStart(temp_path);
    }).detach();
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
