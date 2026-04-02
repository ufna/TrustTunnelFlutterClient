#include <gtest/gtest.h>

#include "vpn_plugin.h"

namespace vpn_plugin {
namespace test {

TEST(VpnPlugin, InitialStateIsDisconnected) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  // ui_runner is nullptr in tests — we won't exercise async paths.
  IVpnManagerImpl manager(handler.get(), nullptr);
  auto result = manager.GetCurrentState();
  ASSERT_FALSE(result.has_error());
  EXPECT_EQ(result.value(), VpnManagerState::kDisconnected);
}

TEST(VpnPlugin, StopReturnsDisconnected) {
  auto handler = std::make_unique<VpnEventStreamHandler>();
  IVpnManagerImpl manager(handler.get(), nullptr);
  auto err = manager.Stop();
  EXPECT_FALSE(err.has_value());
  auto result = manager.GetCurrentState();
  EXPECT_EQ(result.value(), VpnManagerState::kDisconnected);
}

TEST(VpnPlugin, DeepLinkReturnsUnimplemented) {
  IDeepLinkImpl deep_link;
  auto result = deep_link.Decode("trusttunnel://example");
  EXPECT_TRUE(result.has_error());
  EXPECT_EQ(result.error().code(), "unimplemented");
}

}  // namespace test
}  // namespace vpn_plugin
