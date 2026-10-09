#include "gfx/vita_native_assets.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
namespace fs=std::filesystem;
int main(int argc,char** argv) {
  bool includeRgba=false,gpuStatic=false;
  if(argc<3||argc>5){
    std::cerr<<"usage: vita_compile_native_assets REQUEST_DIRECTORY OUTPUT_ROOT [--include-rgba] [--gpu-static]\n";return 2;
  }
  for(int i=3;i<argc;++i) {
    const std::string option=argv[i];
    if(option=="--include-rgba")includeRgba=true;
    else if(option=="--gpu-static")gpuStatic=true;
    else {std::cerr<<"unknown option: "<<option<<"\n";return 2;}
  }
  const auto save=[&](const std::string& relative,const std::vector<uint8_t>& record) {
    const auto output=fs::path(argv[2])/relative;fs::create_directories(output.parent_path());
    if(fs::exists(output)) {
      std::ifstream old(output,std::ios::binary);std::vector<uint8_t> existing((std::istreambuf_iterator<char>(old)),{});
      if(existing!=record){std::cerr<<"content key collision: "<<relative<<"\n";return false;}
    } else {
      std::ofstream out(output,std::ios::binary);out.write(reinterpret_cast<const char*>(record.data()),record.size());
      if(!out){std::cerr<<"write failed: "<<output<<"\n";return false;}
    }
    return true;
  };
  size_t compiled=0,rejected=0,gpuCompiled=0,gpuSkipped=0;
  for(const auto& file:fs::directory_iterator(argv[1])) {
    if(file.path().extension()!=".avrq"||!file.is_regular_file())continue;
    if(file.file_size()>16u*1024u*1024u){++rejected;continue;}
    std::ifstream input(file.path(),std::ios::binary);
    std::vector<uint8_t> request((std::istreambuf_iterator<char>(input)),{}),record;
    if(!aurora::vita::gfx::compile_native_asset(request,record,includeRgba)){++rejected;continue;}
    const auto relative=aurora::vita::gfx::native_asset_path(request);
    if(!save(relative,record))return 1;
    if(gpuStatic&&request.size()>=4&&request[0]==2) {
      if(!aurora::vita::gfx::compile_native_gpu_geometry(request,record))++gpuSkipped;
      else {
        if(!save(aurora::vita::gfx::native_gpu_geometry_path(request),record))return 1;
        ++gpuCompiled;
      }
    }
    ++compiled;
  }
  std::cout<<"{\"compiled\":"<<compiled<<",\"rejected\":"<<rejected
           <<",\"gpu_static_compiled\":"<<gpuCompiled<<",\"gpu_static_skipped\":"<<gpuSkipped<<"}\n";
  return 0;
}
