#include "vita_native_assets.hpp"
#include <xxhash.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <limits>
namespace aurora::vita::gfx {
static_assert(std::endian::native == std::endian::little,
              "AVNR canonical float records require a little-endian host");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
namespace {
constexpr uint32_t Magic=0x524e5641,Version=1,Limit=16*1024*1024;
NativeAssetReader sReader=nullptr;void* sContext=nullptr;NativeAssetStats sStats{};
void word(std::vector<uint8_t>& b,uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));}
void span(std::vector<uint8_t>& b,const void* p,size_t n){if(n){const auto* d=static_cast<const uint8_t*>(p);b.insert(b.end(),d,d+n);}}
struct Input {
  const std::vector<uint8_t>& b;size_t pos=0;bool ok=true;
  uint32_t word(){if(pos>b.size()||b.size()-pos<4){ok=false;return 0;}uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(b[pos++])<<(8*i);return v;}
  const uint8_t* span(size_t n){if(pos>b.size()||n>b.size()-pos){ok=false;return nullptr;}const auto* p=b.data()+pos;pos+=n;return p;}
};
bool texture_input(const std::vector<uint8_t>& req,TextureDesc& d) {
  Input in{req};if(in.word()!=1)return false;
  d.width=in.word();d.height=in.word();const auto fmt=in.word(),palette=in.word();
  const auto levels=in.word(),generate=in.word();const size_t bytes=in.word(),palBytes=in.word();
  if(fmt>unsigned(TextureFormat::RGBA8888)||palette>unsigned(PaletteFormat::RGB5A3)||
      !levels||levels>13||generate||!bytes||bytes>Limit||palBytes>65536)return false;
  d.format=TextureFormat(fmt);d.paletteFormat=PaletteFormat(palette);d.mipCount=levels;
  d.data=in.span(bytes);d.dataSize=bytes;d.palette=in.span(palBytes);d.paletteSize=palBytes;
  d.cacheable=false;d.immutableSource=true;
  return in.ok&&in.pos==req.size();
}
bool geometry_input(const std::vector<uint8_t>& req,PreparedDraw& out) {
  Input in{req};if(in.word()!=2)return false;
  const auto count=in.word(),primitive=in.word(),stride=in.word(),le=in.word(),attrs=in.word(),rawBytes=in.word();
  if(!count||count>65535||primitive>unsigned(SourcePrimitive::TriangleFan)||!stride||stride>4096||
      attrs>24||le>1||rawBytes>Limit||uint64_t(count)*stride!=rawBytes)return false;
  VertexDecodeLayout layout{};layout.count=attrs;layout.streamStride=stride;layout.streamLittleEndian=le;
  for(unsigned i=0;i<attrs;++i) {
    auto& a=layout.attributes[i];const auto sem=in.word(),source=in.word(),component=in.word(),components=in.word(),frac=in.word();
    const auto offset=in.word(),valueOffset=in.word(),arrayStride=in.word(),arrayLe=in.word(),arrayBytes=in.word();
    if(sem>unsigned(VertexSemantic::Tex7)||source>unsigned(VertexSource::Index16)||
        component>unsigned(VertexComponent::RGBA8)||components>4||frac>31||offset>65535||valueOffset>65535||
        arrayStride>65535||arrayLe>1||arrayBytes>Limit)return false;
    a.semantic=VertexSemantic(sem);a.source=VertexSource(source);a.component=VertexComponent(component);
    a.components=components;a.frac=frac;a.streamOffset=offset;a.valueOffset=valueOffset;
    a.array.stride=arrayStride;a.array.littleEndian=arrayLe;a.array.size=arrayBytes;a.array.data=in.span(arrayBytes);
  }
  const auto* raw=in.span(rawBytes);if(!in.ok||in.pos!=req.size())return false;
  compile_vertex_decode_layout(layout);
  PipelineDesc pipeline{};pipeline.fixedVertexOnGpu=true;
  auto recipe=build_draw_recipe(pipeline);recipe.decodeSemantics=AllVertexSemantics;
  return prepare_draw_into(out,raw,rawBytes,count,SourcePrimitive(primitive),layout,pipeline,{},nullptr,{},nullptr,true,&recipe);
}
std::vector<uint8_t> record_payload(const std::vector<uint8_t>& req,uint32_t type) {
  std::vector<uint8_t> record;if(!sReader||req.empty())return record;
  const auto path=native_asset_path(req);
  if(!sReader(path.c_str(),record,sContext)){++sStats.misses;return {};}
  Input in{record};const auto magic=in.word(),version=in.word(),kind=in.word(),sourceBytes=in.word(),payloadBytes=in.word();
  const uint32_t checksumLow=in.word(),checksumHigh=in.word();
  const uint64_t checksum=uint64_t(checksumLow)|(uint64_t(checksumHigh)<<32);
  const auto* source=in.span(sourceBytes);const auto* payload=in.span(payloadBytes);
  if(!in.ok||in.pos!=record.size()||magic!=Magic||version!=Version||kind!=type||sourceBytes!=req.size()||
      !payloadBytes||payloadBytes>Limit||std::memcmp(source,req.data(),sourceBytes)||
      XXH64(payload,payloadBytes,0)!=checksum) {++sStats.rejected;return {};}
  return {payload,payload+payloadBytes};
}
}
void configure_native_assets(NativeAssetReader reader,void* context) noexcept {sReader=reader;sContext=context;sStats={};}
NativeAssetStats native_asset_stats() noexcept {return sStats;}
std::vector<uint8_t> native_texture_request(const TextureDesc& d) {
  if(!d.data||!d.dataSize||d.dataSize>Limit||d.paletteSize>65536||(!d.palette&&d.paletteSize))return {};
  std::vector<uint8_t> req;req.reserve(32+d.dataSize+d.paletteSize);
  for(uint32_t v:{1u,d.width,d.height,unsigned(d.format),unsigned(d.paletteFormat),unsigned(d.mipCount),unsigned(d.generateMipmaps),unsigned(d.dataSize),unsigned(d.paletteSize)})word(req,v);
  span(req,d.data,d.dataSize);span(req,d.palette,d.paletteSize);return req;
}
std::vector<uint8_t> native_geometry_request(const uint8_t* raw,size_t bytes,uint32_t count,
                                            SourcePrimitive primitive,const VertexDecodeLayout& layout) {
  if(!raw||!bytes||bytes>Limit||layout.count>24)return {};
  size_t total=28+bytes+size_t(layout.count)*40;
  for(unsigned i=0;i<layout.count;++i){const auto& a=layout.attributes[i];
    if(a.array.size>Limit||total>Limit-a.array.size||(!a.array.data&&a.array.size))return {};total+=a.array.size;}
  std::vector<uint8_t> req;req.reserve(total);
  for(uint32_t v:{2u,count,unsigned(primitive),unsigned(layout.streamStride),unsigned(layout.streamLittleEndian),unsigned(layout.count),unsigned(bytes)})word(req,v);
  for(unsigned i=0;i<layout.count;++i) {const auto& a=layout.attributes[i];
    for(uint32_t v:{unsigned(a.semantic),unsigned(a.source),unsigned(a.component),unsigned(a.components),unsigned(a.frac),unsigned(a.streamOffset),unsigned(a.valueOffset),unsigned(a.array.stride),unsigned(a.array.littleEndian),unsigned(a.array.size)})word(req,v);
    span(req,a.array.data,a.array.size);
  }
  span(req,raw,bytes);return req;
}
std::string native_asset_path(const std::vector<uint8_t>& req) {
  if(req.empty())return {};
  char path[96];std::snprintf(path,sizeof path,"native/v1/%s/%016llx.avnr",req[0]==1?"textures":"geometry",
      (unsigned long long)XXH64(req.data(),req.size(),0));return path;
}
bool read_native_blob(const char* path,std::vector<uint8_t>& bytes,size_t limit) noexcept {
  bytes.clear();
  if(!sReader||!path||!sReader(path,bytes,sContext))return false;
  if(bytes.empty()||bytes.size()>limit){bytes.clear();return false;}
  return true;
}
bool compile_native_asset(const std::vector<uint8_t>& req,std::vector<uint8_t>& record,bool includeRgba) {
  record.clear();if(req.empty()||req.size()>Limit)return false;
  std::vector<uint8_t> payload;const uint32_t type=req[0];
  if(type==1) {
    TextureDesc desc;if(!texture_input(req,desc))return false;
    auto out=gxm::prepare_texture_upload(desc,true,true);
    if(!out.ok()||(!includeRgba&&out.format==NativeTextureFormat::Rgba8))return false;
    for(uint32_t v:{unsigned(out.format),out.stride,out.mipCount,unsigned(out.swizzled)})word(payload,v);
    span(payload,out.pixels.data(),out.pixels.size());
  } else if(type==2) {
    PreparedDraw out;if(!geometry_input(req,out))return false;
    word(payload,out.vertices.size());word(payload,out.indices.size());
    // Canonical wire ABI: 13 floats, 8 color bytes, 24 texcoord floats,
    // 9 matrix selectors, 3 zero padding bytes. No host pointer or enum ABI.
    static_assert(sizeof(CanonicalVertex)==168);
    for(const auto& v:out.vertices){
      span(payload,v.position,16);span(payload,v.normal,12);span(payload,v.binormal,12);span(payload,v.tangent,12);
      span(payload,v.color0,4);span(payload,v.color1,4);span(payload,v.texcoord,96);
      span(payload,&v.pnMatrixIndex,1);span(payload,v.texMatrixIndex,8);payload.insert(payload.end(),3,0);
    }
    for(uint16_t i:out.indices){payload.push_back(i&255);payload.push_back(i>>8);}
  } else return false;
  if(payload.empty()||payload.size()>Limit)return false;
  const auto checksum=XXH64(payload.data(),payload.size(),0);
  for(uint32_t v:{Magic,Version,type,unsigned(req.size()),unsigned(payload.size()),uint32_t(checksum),uint32_t(checksum>>32)})word(record,v);
  span(record,req.data(),req.size());span(record,payload.data(),payload.size());return true;
}
bool load_native_texture(const TextureDesc& desc,gxm::CompactTextureData& out,bool allowBc1) noexcept {
  if(!sReader||!desc.immutableSource)return false;
  auto payload=record_payload(native_texture_request(desc),1);if(payload.empty())return false;
  Input in{payload};const auto fmt=in.word(),stride=in.word(),mips=in.word(),swizzled=in.word();
  if(fmt<unsigned(NativeTextureFormat::Intensity8)||fmt>unsigned(NativeTextureFormat::Rgba8)||
      !mips||mips>13||mips!=std::max<unsigned>(desc.mipCount,1)||swizzled>1||
      (fmt==unsigned(NativeTextureFormat::Bc1)&&!allowBc1)||
      stride!=(swizzled?desc.width:fmt==unsigned(NativeTextureFormat::Bc1)?desc.width:(desc.width+7u)&~7u))return false;
  size_t expected=0;
  for(unsigned level=0;level<mips;++level) {
    const uint32_t w=std::max(1u,desc.width>>level),h=std::max(1u,desc.height>>level);
    const unsigned bpp=fmt==unsigned(NativeTextureFormat::Intensity8)?1:fmt==unsigned(NativeTextureFormat::Rgba8)?4:2;
    expected+=fmt==unsigned(NativeTextureFormat::Bc1)?dxt1_texture_size(w,h):size_t(swizzled?w:(w+7u)&~7u)*h*bpp;
  }
  if(!in.ok||payload.size()-in.pos!=expected)return false;
  out={};out.format=NativeTextureFormat(fmt);out.stride=stride;out.mipCount=mips;out.swizzled=swizzled;
  out.pixels.assign(payload.data()+in.pos,payload.data()+payload.size());++sStats.textureHits;return true;
}
bool load_native_geometry(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                          const VertexDecodeLayout& layout,PreparedDraw& out) noexcept {
  if(!sReader||primitive>SourcePrimitive::TriangleFan)return false;
  auto payload=record_payload(native_geometry_request(raw,bytes,count,primitive,layout),2);if(payload.empty())return false;
  Input in{payload};const size_t vertices=in.word(),indices=in.word();
  if(!vertices||vertices>count||indices>65535*6u||payload.size()!=8+vertices*168+indices*2)return false;
  out={};out.vertices.resize(vertices);out.indices.resize(indices);
  const auto* v=in.span(vertices*168);if(!v)return false;
  std::memcpy(out.vertices.data(),v,vertices*168);
  for(size_t i=0;i<indices;++i){const auto* p=in.span(2);if(!p)return false;out.indices[i]=uint16_t(p[0])|(uint16_t(p[1])<<8);if(out.indices[i]>=vertices)return false;}
  out.primitive=Primitive::Triangles;out.error=PrepareDrawError::None;++sStats.geometryHits;return true;
}
} // namespace aurora::vita::gfx
