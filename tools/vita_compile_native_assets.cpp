#include "gfx/vita_native_assets.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
namespace fs=std::filesystem;
int main(int argc,char** argv) {
  if(argc<3||argc>5) {
    std::cerr<<"usage: vita_compile_native_assets REQUEST_DIRECTORY OUTPUT_ROOT [--no-bc1] [--include-rgba]\n";return 2;
  }
  bool allowBc1=true;
  // WiiCompiled already includes RGBA sidecars. Keep that default while
  // accepting upstream's explicit flag and the existing --no-bc1 contract.
  constexpr bool includeRgba=true;
  for(int i=3;i<argc;++i) {
    const std::string flag=argv[i];
    if(flag=="--no-bc1")allowBc1=false;
    else if(flag!="--include-rgba") {
      std::cerr<<"unknown option: "<<flag<<'\n';return 2;
    }
  }
  size_t compiled=0,rejected=0;
  for(const auto& file:fs::directory_iterator(argv[1])) {
    if(file.path().extension()!=".avrq"||!file.is_regular_file())continue;
    if(file.file_size()>16u*1024u*1024u){++rejected;continue;}
    std::ifstream input(file.path(),std::ios::binary);
    std::vector<uint8_t> request((std::istreambuf_iterator<char>(input)),{}),record;
    if(!aurora::vita::gfx::compile_native_asset(request,record,allowBc1,includeRgba)){++rejected;continue;}
    const auto relative=aurora::vita::gfx::native_asset_path(request);
    const auto output=fs::path(argv[2])/relative;fs::create_directories(output.parent_path());
    if(fs::exists(output)) {
      std::ifstream old(output,std::ios::binary);std::vector<uint8_t> existing((std::istreambuf_iterator<char>(old)),{});
      if(existing!=record){std::cerr<<"content key collision: "<<relative<<"\n";return 1;}
    } else {
      std::ofstream out(output,std::ios::binary);out.write(reinterpret_cast<const char*>(record.data()),record.size());
      if(!out){std::cerr<<"write failed: "<<output<<"\n";return 1;}
    }
    ++compiled;
  }
  std::cout<<"{\"compiled\":"<<compiled<<",\"rejected\":"<<rejected<<"}\n";
  return 0;
}
