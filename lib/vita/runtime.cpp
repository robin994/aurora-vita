#include "../internal.hpp"

#include <cstdio>

namespace aurora {

AuroraConfig g_config{};
uint32_t g_sdlCustomEventsStart = 0;
char g_gameName[4]{};

void log_internal(const AuroraLogLevel level, const char* module, const char* message,
                  const unsigned int len) noexcept {
  if (module == nullptr) module = "";
  if (message == nullptr) message = "";

  if (g_config.logCallback != nullptr) {
    g_config.logCallback(level, module, message, len);
    return;
  }

  std::fprintf(stderr, "[aurora:%d] [%s] ", static_cast<int>(level), module);
  if (len != 0) std::fwrite(message, 1, len, stderr);
  std::fputc('\n', stderr);
}

void Module::show_fatal_dialog(const char* module, std::string_view message) noexcept {
  std::fprintf(stderr, "[aurora] fatal renderer error%s%s: ",
               module != nullptr && module[0] != '\0' ? " in " : "",
               module != nullptr ? module : "");
  if (!message.empty()) std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
}

} // namespace aurora
