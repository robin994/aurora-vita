#include "vita_io.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace aurora::vita;

namespace {
int fail(int code, const char* path, const char* temp) {
  (void)io::remove_path(path);
  (void)io::remove_path(temp);
  return code;
}
}

int main() {
  const char* path = "vita_io_test.bin";
  const char* temp = "vita_io_test.tmp";
  (void)io::remove_path(path);
  (void)io::remove_path(temp);

  std::vector<uint8_t> expected(50000);
  for(size_t i=0;i<expected.size();++i)
    expected[i]=static_cast<uint8_t>((i*37u+11u)&0xffu);

  {
    io::BufferedWriter writer(path,false);
    if(!writer.is_open())return fail(1,path,temp);
    for(size_t offset=0;offset<expected.size();) {
      const size_t chunk=(offset%23u)+1u;
      const size_t bytes=std::min(chunk,expected.size()-offset);
      if(!writer.write(expected.data()+offset,bytes))return fail(2,path,temp);
      offset+=bytes;
    }
    if(!writer.close())return fail(3,path,temp);
  }

  // Worst case for unbuffered sceIo: one-byte logical reads. The reader must
  // satisfy these from its 16 KiB buffer instead of issuing one syscall each.
  {
    io::BufferedReader reader(path);
    if(!reader.is_open())return fail(4,path,temp);
    for(size_t i=0;i<expected.size();++i)
      if(reader.get_byte()!=expected[i])return fail(5,path,temp);
    if(reader.get_byte()!=-1 || !reader.eof())return fail(6,path,temp);
  }

  // Exercise transitions between buffered small reads and direct large reads.
  {
    io::BufferedReader reader(path);
    if(!reader.is_open())return fail(7,path,temp);
    std::array<uint8_t,7> head{};
    if(!reader.read_exact(head.data(),head.size()) ||
       std::memcmp(head.data(),expected.data(),head.size())!=0)
      return fail(8,path,temp);
    std::vector<uint8_t> large(32768);
    if(!reader.read_exact(large.data(),large.size()) ||
       std::memcmp(large.data(),expected.data()+head.size(),large.size())!=0)
      return fail(9,path,temp);
    const size_t tailOffset=head.size()+large.size();
    std::vector<uint8_t> tail(expected.size()-tailOffset);
    if(!reader.read_exact(tail.data(),tail.size()) ||
       std::memcmp(tail.data(),expected.data()+tailOffset,tail.size())!=0 ||
       !reader.eof())
      return fail(10,path,temp);
  }

  {
    static constexpr uint8_t suffix[]{0xaa,0xbb,0xcc,0xdd};
    io::BufferedWriter writer(path,true);
    if(!writer.is_open() || !writer.write(suffix,sizeof(suffix)) || !writer.close())
      return fail(11,path,temp);
    io::BufferedReader reader(path);
    std::vector<uint8_t> all(expected.size()+sizeof(suffix));
    if(!reader.read_exact(all.data(),all.size()) || !reader.eof() ||
       std::memcmp(all.data(),expected.data(),expected.size())!=0 ||
       std::memcmp(all.data()+expected.size(),suffix,sizeof(suffix))!=0)
      return fail(12,path,temp);
  }

  {
    static constexpr char replacement[]="replacement";
    if(!io::write_file(temp,replacement,sizeof(replacement)-1) ||
       !io::replace_file(temp,path))
      return fail(13,path,temp);
    io::BufferedReader reader(path);
    std::array<char,sizeof(replacement)-1> data{};
    if(!reader.read_exact(data.data(),data.size()) || !reader.eof() ||
       std::memcmp(data.data(),replacement,data.size())!=0)
      return fail(14,path,temp);
  }

  (void)io::remove_path(path);
  (void)io::remove_path(temp);
  return 0;
}
