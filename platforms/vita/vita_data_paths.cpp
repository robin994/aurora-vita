#include "vita_data_paths.hpp"

#include <cctype>
#include <cstdio>

#if defined(__vita__)
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace aurora::vita {
namespace {
std::string g_dataRoot;

bool valid_path_char(char c) noexcept {
  const auto u=static_cast<unsigned char>(c);
  return std::isalnum(u)||c=='-'||c=='_';
}

#if defined(__vita__)
std::string current_title_id() noexcept {
  const SceUID pid=sceKernelGetProcessId();
  char titleId[64]{};
  if(sceAppMgrAppParamGetString(pid,12,titleId,sizeof(titleId))>=0) {
    const auto sanitized=sanitize_title_id(titleId);
    if(!sanitized.empty())return sanitized;
  }
  titleId[0]='\0';
  if(sceAppMgrAppParamGetString(pid,6,titleId,sizeof(titleId))>=0) {
    const auto sanitized=sanitize_title_id(titleId);
    if(!sanitized.empty())return sanitized;
  }
  titleId[0]='\0';
  if(sceAppMgrGetNameById(pid,titleId)>=0) {
    const auto sanitized=sanitize_title_id(titleId);
    if(!sanitized.empty())return sanitized;
  }
  // Never share an UNKNOWN directory across titles. A process-scoped fallback
  // is preferable to cross-title cache pollution when an SFO lacks an id.
  char fallback[32]{};
  std::snprintf(fallback,sizeof(fallback),"PID-%08X",static_cast<unsigned>(pid));
  return fallback;
}
#endif
}

std::string sanitize_title_id(std::string_view titleId) {
  std::string out;
  out.reserve(titleId.size());
  for(char c:titleId) {
    if(!valid_path_char(c))continue;
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if(out.size()>=31)break;
  }
  return out;
}

std::string data_root_for_title(std::string_view titleId) {
  const auto sanitized=sanitize_title_id(titleId);
  if(sanitized.empty())return {};
  return "ux0:data/aurora-vita/"+sanitized;
}

bool ensure_directory_tree(const char* path) noexcept {
#if defined(__vita__)
  if(!path||!*path)return false;
  std::string current(path);
  const size_t colon=current.find(':');
  for(size_t i=colon==std::string::npos?0:colon+1;i<current.size();++i) {
    if(current[i]=='/') {
      const auto parent=current.substr(0,i);
      if(!parent.empty())sceIoMkdir(parent.c_str(),0777);
    }
  }
  (void)sceIoMkdir(current.c_str(),0777);
  return true;
#else
  (void)path;
  return true;
#endif
}

bool ensure_parent_directory(const char* path) noexcept {
#if defined(__vita__)
  if(!path||!*path)return false;
  const std::string full(path);
  const auto slash=full.find_last_of('/');
  if(slash==std::string::npos)return true;
  return ensure_directory_tree(full.substr(0,slash).c_str());
#else
  (void)path;
  return true;
#endif
}

std::string default_data_root() noexcept {
#if defined(__vita__)
  return data_root_for_title(current_title_id());
#else
  return {};
#endif
}

void configure_data_root(const char* overridePath) noexcept {
  g_dataRoot=overridePath&&*overridePath?overridePath:default_data_root();
  if(!g_dataRoot.empty())ensure_directory_tree(g_dataRoot.c_str());
}

const std::string& data_root() noexcept { return g_dataRoot; }

std::string data_path(std::string_view relative) {
  if(g_dataRoot.empty())return {};
  while(!relative.empty()&&relative.front()=='/')relative.remove_prefix(1);
  if(relative.empty())return g_dataRoot;
  return g_dataRoot+"/"+std::string(relative);
}

} // namespace aurora::vita
