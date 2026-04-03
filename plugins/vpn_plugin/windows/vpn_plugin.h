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

// Mock implementation of the Pigeon-generated IVpnManager host API.
// Simulates VPN state transitions without actual network activity.
class IVpnManagerImpl : public IVpnManager {
 public:
  explicit IVpnManagerImpl(VpnEventStreamHandler* handler);

  std::optional<FlutterError> Start(const std::string& server_name,
                                    const std::string& config) override;
  std::optional<FlutterError> Stop() override;
  std::optional<FlutterError> UpdateConfiguration(
      const std::string* server_name, const std::string* config) override;
  ErrorOr<VpnManagerState> GetCurrentState() override;

 private:
  VpnEventStreamHandler* handler_;
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
      std::unique_ptr<IVpnManagerImpl> vpn_manager,
      std::unique_ptr<IDeepLinkImpl> deep_link);

  ~VpnPlugin() override;

  VpnPlugin(const VpnPlugin&) = delete;
  VpnPlugin& operator=(const VpnPlugin&) = delete;

 private:
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<VpnEventStreamHandler> handler_;
  std::unique_ptr<IVpnManagerImpl> vpn_manager_;
  std::unique_ptr<IDeepLinkImpl> deep_link_;
};

}  // namespace vpn_plugin

#endif  // FLUTTER_PLUGIN_VPN_PLUGIN_H_
