#include "vita_efb_copy.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace aurora::vita::gfx {
bool copy_efb_rgba8(const uint8_t* source, uint32_t width, uint32_t height,
                    const Scissor& rect, uint32_t dstWidth, uint32_t dstHeight,
                    EfbCopyFormat format, bool flipX, bool flipY,
                    std::vector<uint8_t>& destination) noexcept {
  if (!source || !width || !height || !dstWidth || !dstHeight ||
      rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
      uint64_t(rect.x) + rect.width > width || uint64_t(rect.y) + rect.height > height ||
      !is_supported_color_copy_format(format) ||
      uint64_t(width)*height > std::numeric_limits<size_t>::max()/4 ||
      uint64_t(dstWidth)*dstHeight > std::numeric_limits<size_t>::max()/4) return false;
  destination.resize(size_t(dstWidth)*dstHeight*4);
  const auto quant4=[](float v) { return std::floor(v*16.f)/15.f; };
  for (uint32_t y=0;y<dstHeight;++y) {
    const float sy=std::clamp((float(y)+.5f)*float(rect.height)/float(dstHeight)-.5f,0.f,float(rect.height-1));
    const float py=flipY?float(rect.height-1)-sy:sy;
    const uint32_t y0=uint32_t(py),y1=std::min(y0+1,uint32_t(rect.height-1));
    const float fy=py-float(y0);
    for (uint32_t x=0;x<dstWidth;++x) {
      const float sx=std::clamp((float(x)+.5f)*float(rect.width)/float(dstWidth)-.5f,0.f,float(rect.width-1));
      const float px=flipX?float(rect.width-1)-sx:sx;
      const uint32_t x0=uint32_t(px),x1=std::min(x0+1,uint32_t(rect.width-1));
      const float fx=px-float(x0);
      float c[4];
      for (unsigned ch=0;ch<4;++ch) {
        const auto sample=[&](uint32_t ix,uint32_t iy) {
          return float(source[(size_t(uint32_t(rect.y)+iy)*width+uint32_t(rect.x)+ix)*4+ch])/255.f;
        };
        const float a=sample(x0,y0)*(1.f-fx)+sample(x1,y0)*fx;
        const float b=sample(x0,y1)*(1.f-fx)+sample(x1,y1)*fx;
        c[ch]=a*(1.f-fy)+b*fy;
      }
      const float intensity=.257f*c[0]+.504f*c[1]+.098f*c[2]+16.f/255.f;
      float value=0.f;
      switch (format) {
      case EfbCopyFormat::Passthrough: break;
      case EfbCopyFormat::RGB565: c[3]=1.f; break;
      case EfbCopyFormat::I4: value=quant4(intensity);c[0]=c[1]=c[2]=c[3]=value;break;
      case EfbCopyFormat::I8: c[0]=c[1]=c[2]=c[3]=intensity;break;
      case EfbCopyFormat::IA4: c[0]=c[1]=c[2]=quant4(intensity);c[3]=quant4(c[3]);break;
      case EfbCopyFormat::IA8: c[0]=c[1]=c[2]=intensity;break;
      case EfbCopyFormat::R4: c[0]=c[1]=c[2]=c[3]=quant4(c[0]);break;
      case EfbCopyFormat::RA4: c[0]=c[1]=c[2]=quant4(c[0]);c[3]=quant4(c[3]);break;
      case EfbCopyFormat::RA8: c[1]=c[2]=c[0];break;
      case EfbCopyFormat::A8: c[0]=c[1]=c[2]=c[3];break;
      case EfbCopyFormat::R8: c[1]=c[2]=c[3]=c[0];break;
      case EfbCopyFormat::G8: c[0]=c[2]=c[3]=c[1];break;
      case EfbCopyFormat::B8: c[0]=c[1]=c[3]=c[2];break;
      case EfbCopyFormat::RG8: c[3]=c[1];c[1]=c[2]=c[0];break;
      case EfbCopyFormat::GB8: c[3]=c[2];c[0]=c[2]=c[1];break;
      default:return false;
      }
      for (unsigned ch=0;ch<4;++ch)
        destination[(size_t(y)*dstWidth+x)*4+ch]=uint8_t(std::clamp(std::floor(c[ch]*255.f+.5f),0.f,255.f));
    }
  }
  return true;
}
}
