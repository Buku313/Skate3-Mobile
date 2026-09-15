"""Exercise the changed CPU texture decoder without requiring a GPU."""
from pathlib import Path
import subprocess


from common import b as app, out as root, span, CXX

source = (app / 'src/skate3_native_scene_gpu.cpp').read_text()
code = '''#include <cassert>\n#include <cstdint>\n#include <cstring>\n#include <iostream>\n#include <vector>\nnamespace xenos { enum class TextureFormat { k_DXT1, k_DXT2_3, k_DXT4_5, k_DXT5A, k_DXN }; }\n'''
for signature in ['void DecodeBc1Block(', 'inline bool Bc1BlockUsesPunchOut(', 'void DecodeBcAlphaBlock(', 'void DecodeBc2Block(', 'void DecodeBc3Block(', 'void DecodeBcRowToMapping(']:
    start, end = span(source, signature)
    code += source[start:end] + '\n'
code += r'''
int main() {
  uint8_t block[16] = {}, pixels[16][4];
  // Equal endpoints: index 3 is transparent for BC1 but never for BC2/BC3 color.
  std::memset(block + 4, 255, 4);
  assert(Bc1BlockUsesPunchOut(block));
  DecodeBc1Block(block,pixels);
  for(auto& p:pixels) assert(p[3]==0);
  std::memset(block,255,8); std::memset(block+8,0,4); std::memset(block+12,255,4);
  DecodeBc2Block(block,pixels);
  for(auto& p:pixels) assert(p[3]==255);
  block[0]=255; block[1]=255; std::memset(block+2,0,6);
  DecodeBc3Block(block,pixels);
  for(auto& p:pixels) assert(p[3]==255);
  // Alpha special endpoints, all 16 texels independently selected.
  uint8_t alpha[8]={0,255}, decoded[16]; uint64_t indices=0;
  for(int i=0;i<16;i++) indices|=uint64_t(i%8)<<(i*3);
  for(int i=0;i<6;i++) alpha[i+2]=indices>>(8*i);
  DecodeBcAlphaBlock(alpha,decoded);
  uint8_t expected[8]={0,255,51,102,153,204,0,255};
  for(int i=0;i<16;i++) assert(decoded[i]==expected[i%8]);
  // RG-class CPU fallback: exact-sized small mip targets and RGB565 channel packing.
  uint8_t red[16]={0,0xf8,0,0};
  for(unsigned w=1;w<=4;w++) for(unsigned h=1;h<=4;h++) {
    std::vector<uint8_t> rgba(w*h*4),rgb565(w*h*2);
    DecodeBcRowToMapping(red,1,xenos::TextureFormat::k_DXT1,w,h,0,rgba.data(),w*4,4);
    DecodeBcRowToMapping(red,1,xenos::TextureFormat::k_DXT1,w,h,0,rgb565.data(),w*2,2,true);
    for(unsigned i=0;i<w*h;i++) {
      assert(rgba[i*4]==255 && rgba[i*4+1]==0 && rgba[i*4+2]==0 && rgba[i*4+3]==255);
      uint16_t packed=0; std::memcpy(&packed,rgb565.data()+i*2,2); assert(packed==0xf800);
    }
  }
  std::cout<<"PASS: BC1 transparency, BC2/BC3 explicit alpha, BC alpha endpoints, RGB565 red packing, 16 exact-sized partial mip shapes.\n";
}
'''
cpp = root / 'textures.cpp'
cpp.write_text(code)
exe = root / 'textures'
subprocess.run([*CXX,'-std=c++20','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
