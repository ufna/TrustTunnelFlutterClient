#include "vpn_plugin.h"

#include <chrono>
#include <thread>

namespace vpn_plugin {

// ---- VpnEventStreamHandler ----

void VpnEventStreamHandler::EmitState(VpnManagerState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_) {
    sink_->Success(
        flutter::EncodableValue(static_cast<int64_t>(state)));
  }
}

std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
VpnEventStreamHandler::OnListenInternal(
    const flutter::EncodableValue* /*arguments*/,
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = std::move(events);
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
                                 std::shared_ptr<flutter::TaskRunner> ui_runner)
    : handler_(handler), ui_runner_(std::move(ui_runner)) {}

std::optional<FlutterError> IVpnManagerImpl::Start(
    const std::string& /*server_name*/, const std::string& /*config*/) {
  state_ = VpnManagerState::kConnecting;
  handler_->EmitState(state_);

  // Simulate async connection: transition to connected after 2 seconds.
  std::thread([this]() {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    state_ = VpnManagerState::kConnected;
    ui_runner_->PostTask([this]() { handler_->EmitState(state_); });
  }).detach();

  return std::nullopt;
}

std::optional<FlutterError> IVpnManagerImpl::Stop() {
  state_ = VpnManagerState::kDisconnected;
  handler_->EmitState(state_);
  return std::nullopt;
}

std::optional<FlutterError> IVpnManagerImpl::UpdateConfiguration(
    const std::string* /*server_name*/, const std::string* /*config*/) {
  // No-op on Windows (iOS-specific feature).
  return std::nullopt;
}

ErrorOr<VpnManagerState> IVpnManagerImpl::GetCurrentState() {
  return state_;
}

// ---- IDeepLinkImpl ----

ErrorOr<std::string> IDeepLinkImpl::Decode(const std::string& /*uri*/) {
  return FlutterError("unimplemented", "Deep link decoding is not supported on Windows");
}

// ---- VpnPlugin ----

void VpnPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto messenger = registrar->messenger();
  auto ui_runner = registrar->task_runner();

  auto handler = std::make_unique<VpnEventStreamHandler>();

  auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          messenger, "vpn_plugin_event_channel",
          &flutter::StandardMethodCodec::GetInstance());
  event_channel->SetStreamHandler(
      std::unique_ptr<VpnEventStreamHandler>(handler.get()));

  auto vpn_manager =
      std::make_unique<IVpnManagerImpl>(handler.get(), ui_runner);
  auto deep_link = std::make_unique<IDeepLinkImpl>();

  // Register Pigeon host API handlers.
  IVpnManager::SetUp(messenger, vpn_manager.get());
  IDeepLink::SetUp(messenger, deep_link.get());

  registrar->AddPlugin(std::make_unique<VpnPlugin>(
      std::move(event_channel), std::move(handler), std::move(vpn_manager),
      std::move(deep_link)));
}

VpnPlugin::VpnPlugin(
    std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
        event_channel,
    std::unique_ptr<VpnEventStreamHandler> handler,
    std::unique_ptr<IVpnManagerImpl> vpn_manager,
    std::unique_ptr<IDeepLinkImpl> deep_link)
    : event_channel_(std::move(event_channel)),
      handler_(std::move(handler)),
      vpn_manager_(std::move(vpn_manager)),
      deep_link_(std::move(deep_link)) {}

VpnPlugin::~VpnPlugin() = default;

}  // namespace vpn_plugin
