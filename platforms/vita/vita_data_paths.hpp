#pragma once

#include <string>
#include <string_view>

namespace aurora::vita {

std::string sanitize_title_id(std::string_view titleId);
std::string data_root_for_title(std::string_view titleId);
std::string default_data_root() noexcept;
void configure_data_root(const char* overridePath=nullptr) noexcept;
const std::string& data_root() noexcept;
std::string data_path(std::string_view relative);
bool ensure_directory_tree(const char* path) noexcept;
bool ensure_parent_directory(const char* path) noexcept;

} // namespace aurora::vita
