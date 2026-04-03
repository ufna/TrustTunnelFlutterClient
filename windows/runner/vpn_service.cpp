#include <windows.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

// ---- Minimal vpn_easy.dll loader (inline, no external deps) ----

using VpnEasyStartFn = void (*)(const char*, void (*)(void*, int), void*);
using VpnEasyStopFn = void (*)();

static VpnEasyStartFn g_start_fn = nullptr;
static VpnEasyStopFn g_stop_fn = nullptr;
static bool g_vpn_running = false;

static bool LoadVpnEasy() {
  HMODULE mod = LoadLibraryA("vpn_easy.dll");
  if (!mod) return false;
  g_start_fn = reinterpret_cast<VpnEasyStartFn>(
      GetProcAddress(mod, "vpn_easy_start"));
  g_stop_fn = reinterpret_cast<VpnEasyStopFn>(
      GetProcAddress(mod, "vpn_easy_stop"));
  return g_start_fn && g_stop_fn;
}

// ---- Pipe I/O ----

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::mutex g_pipe_mutex;

static bool SendLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_pipe_mutex);
  std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile(g_pipe, data.c_str(), static_cast<DWORD>(data.size()),
                   &written, nullptr) != 0;
}

static std::string ReadLine() {
  std::string result;
  char ch = 0;
  DWORD bytes_read = 0;
  while (ReadFile(g_pipe, &ch, 1, &bytes_read, nullptr) && bytes_read == 1) {
    if (ch == '\n') break;
    if (ch != '\r') result += ch;
  }
  return result;
}

// ---- VPN state callback (called from vpn_easy background thread) ----

static void OnStateChanged(void* /*arg*/, int state) {
  SendLine("STATE " + std::to_string(state));
}

// ---- Read config file ----

static std::string ReadFileContent(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- Debug log ----

static void HelperLog(const std::string& msg) {
  std::ofstream log("C:\\Users\\kinder\\vpn_service_debug.log", std::ios::app);
  log << msg << std::endl;
}

// ---- Main ----

int main(int argc, char* argv[]) {
  HelperLog("=== vpn_service.exe started, argc=" + std::to_string(argc));
  if (argc < 2) {
    HelperLog("ERROR: no pipe name argument");
    return 1;
  }

  const std::string pipe_name = argv[1];
  HelperLog("Pipe name: " + pipe_name);

  // Connect to the named pipe created by the Flutter plugin.
  g_pipe = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (g_pipe == INVALID_HANDLE_VALUE) {
    HelperLog("ERROR: CreateFileA failed, err=" + std::to_string(GetLastError()));
    return 1;
  }
  HelperLog("Connected to pipe OK");

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(g_pipe, &mode, nullptr, nullptr);

  // Load vpn_easy.dll
  if (LoadVpnEasy()) {
    HelperLog("vpn_easy.dll loaded OK, sending READY");
    SendLine("READY");
  } else {
    HelperLog("ERROR: LoadVpnEasy failed, err=" + std::to_string(GetLastError()));
    SendLine("ERROR Failed to load vpn_easy.dll");
  }

  // Command loop
  while (true) {
    std::string line = ReadLine();
    if (line.empty()) {
      // Pipe broken (app closed) — stop VPN and exit.
      if (g_vpn_running && g_stop_fn) g_stop_fn();
      break;
    }

    if (line.rfind("START ", 0) == 0) {
      std::string config_path = line.substr(6);
      std::string config = ReadFileContent(config_path);
      if (config.empty()) {
        SendLine("ERROR Cannot read config file: " + config_path);
        SendLine("STATE 0");
        continue;
      }
      if (!g_start_fn) {
        SendLine("ERROR vpn_easy.dll not loaded");
        SendLine("STATE 0");
        continue;
      }
      // Stop previous session if running.
      if (g_vpn_running && g_stop_fn) g_stop_fn();
      g_vpn_running = true;
      g_start_fn(config.c_str(), &OnStateChanged, nullptr);

    } else if (line == "STOP") {
      if (g_vpn_running && g_stop_fn) {
        g_stop_fn();
        g_vpn_running = false;
      }
      SendLine("STATE 0");
    }
  }

  CloseHandle(g_pipe);
  return 0;
}
