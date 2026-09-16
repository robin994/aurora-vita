#pragma once
#include <cstdint>
#if defined(__vita__)
#include <vitaGL.h>
#include <string>
#endif
namespace aurora::vita::gfx {
#if defined(__vita__)
void configure_program_binary_cache(const char* path) noexcept;
GLuint compile_shader(GLenum type,const char* src,std::string* diagnostics=nullptr) noexcept;
GLuint link_program(const char* vs,const char* fs,std::string* diagnostics=nullptr) noexcept;
#endif
} // namespace aurora::vita::gfx
