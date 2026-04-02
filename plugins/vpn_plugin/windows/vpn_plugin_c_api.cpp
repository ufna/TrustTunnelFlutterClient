#include "include/vpn_plugin/vpn_plugin.h"

#include <flutter/plugin_registrar_windows.h>

#include "vpn_plugin.h"

void VpnPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  vpn_plugin::VpnPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
