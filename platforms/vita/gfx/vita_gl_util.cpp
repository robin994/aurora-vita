#include "vita_gl_util.hpp"
#include "vita_program_binary_cache.hpp"
#include "../vita_io.hpp"
#if defined(__vita__)
#include <cstdio>
#include <string>
#include <vector>
#include <psp2/io/stat.h>
#ifndef AURORA_VITAGL_CACHE_ABI
#define AURORA_VITAGL_CACHE_ABI ""
#endif
namespace aurora::vita::gfx {
namespace {
std::string programCacheRoot;
uint32_t programCacheHits=0,programCacheMisses=0;

char program_cache_shard(uint64_t sourceHash) noexcept {
  static constexpr char Hex[]="0123456789abcdef";
  return Hex[(sourceHash>>60u)&0x0fu];
}

std::string program_cache_path(uint64_t sourceHash) {
  if(programCacheRoot.empty())return {};
  char file[36];std::snprintf(file,sizeof(file),"/%c/%016llx.bin",program_cache_shard(sourceHash),
                              static_cast<unsigned long long>(sourceHash));
  return programCacheRoot+file;
}

GLuint load_cached_program(const std::string& path,uint64_t sourceHash) noexcept {
  if(path.empty())return 0;
  io::BufferedReader file(path.c_str());
  if(!file.is_open())return 0;
  ProgramCacheHeader header{};
  if(!file.read_exact(&header,sizeof(header))||header.length>MaxProgramCacheBytes||header.length==0)return 0;
  std::vector<uint8_t> binary(header.length);
  const bool readOk=file.read_exact(binary.data(),binary.size())&&file.eof();
  const size_t attributeBytes=sizeof(GLuint)+16*sizeof(SceGxmVertexAttribute);
  if(!readOk||!valid_program_cache(header,binary.data(),binary.size(),sourceHash,attributeBytes))return 0;
  const GLuint program=glCreateProgram();
  if(!program)return 0;
  glProgramBinary(program,header.format,binary.data(),static_cast<GLsizei>(binary.size()));
  GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
  if(!linked){glDeleteProgram(program);return 0;}
  ++programCacheHits;
  if(programCacheHits<=4||(programCacheHits&(programCacheHits-1))==0)
    std::fprintf(stderr,"[aurora-vita] program_cache hits=%u misses=%u\n",programCacheHits,programCacheMisses);
  return program;
}

void save_cached_program(const std::string& path,uint64_t sourceHash,GLuint program) noexcept {
  if(path.empty()||!program)return;
  GLint length=0;glGetProgramiv(program,GL_PROGRAM_BINARY_LENGTH,&length);
  if(length<=0||static_cast<size_t>(length)>MaxProgramCacheBytes)return;
  std::vector<uint8_t> binary(static_cast<size_t>(length));
  GLsizei written=0;GLenum format=0;
  glGetProgramBinary(program,length,&written,&format,binary.data());
  if(written!=length)return;
  ProgramCacheHeader header{};header.sourceHash=sourceHash;header.length=static_cast<uint32_t>(written);
  header.format=format;header.binaryHash=program_cache_hash(binary.data(),binary.size());
  if(!valid_program_cache(header,binary.data(),binary.size(),sourceHash,sizeof(GLuint)+16*sizeof(SceGxmVertexAttribute)))return;
  const std::string temporary=path+".tmp";
  io::BufferedWriter file(temporary.c_str(),false);
  if(!file.is_open())return;
  const bool wrote=file.write(&header,sizeof(header))&&file.write(binary.data(),binary.size());
  const bool closed=file.close();
  if(wrote&&closed&&io::replace_file(temporary.c_str(),path.c_str()))return;
  (void)io::remove_path(temporary.c_str());
}
}

void configure_program_binary_cache(const char* path) noexcept {
  programCacheRoot.clear();programCacheHits=programCacheMisses=0;
  // Never deserialize across an unknown vitaGL build. CMake fingerprints the
  // exact archive linked by this target, including its serialization options.
  if(!path||!*path||!AURORA_VITAGL_CACHE_ABI[0])return;
  programCacheRoot=std::string(path)+"/"+AURORA_VITAGL_CACHE_ABI;
  for(size_t i=programCacheRoot.find(':')+1;i<programCacheRoot.size();++i)
    if(programCacheRoot[i]=='/')sceIoMkdir(programCacheRoot.substr(0,i).c_str(),0777);
  sceIoMkdir(programCacheRoot.c_str(),0777);
  static constexpr char Hex[]="0123456789abcdef";
  for(char shard:Hex) {
    if(!shard)break;
    const std::string dir=programCacheRoot+"/"+shard;
    sceIoMkdir(dir.c_str(),0777);
  }
  std::fprintf(stderr,"[aurora-vita] program_cache abi=%s shards=16 root=%s\n",
               AURORA_VITAGL_CACHE_ABI,programCacheRoot.c_str());
}

GLuint compile_shader(GLenum type,const char* src,std::string* diagnostics) noexcept {
  GLuint s=glCreateShader(type);
  if(!s){
    const char* stage=type==GL_VERTEX_SHADER?"vertex":type==GL_FRAGMENT_SHADER?"fragment":"unknown";
    std::printf("[aurora-vita] glCreateShader failed for %s shader\n",stage);
    if(diagnostics){diagnostics->append("glCreateShader failed for ");diagnostics->append(stage);diagnostics->append(" shader\n");}
    return 0;
  }
  glShaderSource(s,1,&src,nullptr); glCompileShader(s);
  GLint ok=0;
  glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok){
    GLint n=0;
    glGetShaderiv(s,GL_INFO_LOG_LENGTH,&n);
    std::vector<char> log(n>1?n:2);
    glGetShaderInfoLog(s,(GLsizei)log.size(),nullptr,log.data());
    const char* stage=type==GL_VERTEX_SHADER?"vertex":type==GL_FRAGMENT_SHADER?"fragment":"unknown";
    std::printf("[aurora-vita] %s shader compile failed: %s\n",stage,log.data());
    if(diagnostics){
      diagnostics->append(stage);
      diagnostics->append(" shader compile failed:\n");
      diagnostics->append(log.data());
      diagnostics->push_back('\n');
    }
    glDeleteShader(s);
    return 0;
  }
  return s;
}
GLuint link_program(const char* vs,const char* fs,std::string* diagnostics) noexcept {
  const uint64_t sourceHash=program_source_hash(vs,fs);
  const std::string cachePath=program_cache_path(sourceHash);
  if(const GLuint cached=load_cached_program(cachePath,sourceHash))return cached;
  if(!cachePath.empty())++programCacheMisses;
  GLuint v=compile_shader(GL_VERTEX_SHADER,vs,diagnostics),f=compile_shader(GL_FRAGMENT_SHADER,fs,diagnostics);if(!v||!f){if(v)glDeleteShader(v);if(f)glDeleteShader(f);return 0;}
  GLuint p=glCreateProgram();
  if(!p){
    std::printf("[aurora-vita] glCreateProgram failed\n");
    if(diagnostics)diagnostics->append("glCreateProgram failed\n");
    glDeleteShader(v);glDeleteShader(f);return 0;
  }
  glAttachShader(p,v);glAttachShader(p,f);
  glBindAttribLocation(p,0,"a_position");glBindAttribLocation(p,1,"a_color0");glBindAttribLocation(p,2,"a_color1");
  glBindAttribLocation(p,11,"a_normal");glBindAttribLocation(p,12,"a_binormal");glBindAttribLocation(p,13,"a_tangent");glBindAttribLocation(p,14,"a_pn_mtx");
  for(unsigned i=0;i<8;i++){char n[16];std::snprintf(n,sizeof(n),"a_tex%u",i);glBindAttribLocation(p,3+i,n);}glLinkProgram(p);
  GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){GLint n=0;glGetProgramiv(p,GL_INFO_LOG_LENGTH,&n);std::vector<char> log(n>1?n:2);glGetProgramInfoLog(p,(GLsizei)log.size(),nullptr,log.data());std::printf("[aurora-vita] program link failed: %s\n",log.data());if(diagnostics){diagnostics->append("program link failed:\n");diagnostics->append(log.data());diagnostics->push_back('\n');}glDeleteProgram(p);p=0;}
  if(p)save_cached_program(cachePath,sourceHash,p);
  glDeleteShader(v);glDeleteShader(f);return p;
}
} // namespace aurora::vita::gfx
#endif
