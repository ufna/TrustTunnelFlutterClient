#include <gtest/gtest.h>

#include "vpn_plugin.h"

namespace vpn_plugin {
namespace test {

TEST(VpnPlugin, InitialStateIsDisconnected) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
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
