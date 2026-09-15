from pathlib import Path
import re,subprocess,json,hashlib
from common import b, sdk, out, span, CXX
x=(sdk/'src/audio/xma_context.cpp').read_text()
def fun(s,sig):u,v=span(s,sig);return s[u:v]
code=r'''#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#define assert_false(x) assert(!(x))
#define REXCVAR_GET(x) (x)
bool xma_old_fix_input_overrun=true;
namespace rex {
uint64_t byte_swap(uint64_t x){return __builtin_bswap64(x);}
float byte_swap(float x){uint32_t u;memcpy(&u,&x,4);u=__builtin_bswap32(u);memcpy(&x,&u,4);return x;}
}
'''
code+='namespace rex::stream {\n'+fun((sdk/'include/rex/stream.h').read_text(),'class BitStream')+';\n'
stream=(sdk/'src/core/bit_stream.cpp').read_text();code+=stream[stream.index('BitStream::BitStream'):stream.rindex('}  // namespace rex::stream')]+'}\nusing rex::stream::BitStream;\n'
helpers=(sdk/'include/rex/audio/xma/helpers.h').read_text();code+=helpers[helpers.index('namespace rex::audio::xma'):]+ '\nnamespace xma=rex::audio::xma;\n'
code+=fun((sdk/'include/rex/audio/xma/context.h').read_text(),'struct kPacketInfo')+';\n'
code+='struct XMA_CONTEXT_DATA {uint32_t loop_count=0,loop_start=0,loop_end=0,input_buffer_read_offset=0,current_buffer=0,input_buffer_0_packet_count=1,input_buffer_1_packet_count=1;};\nstruct XmaContext {static constexpr uint32_t kBytesPerPacket=2048,kBitsPerPacket=16384,kBitsPerPacketHeader=32,kBitsPerFrameHeader=15;\n'
for sig in ['kPacketInfo XmaContext::GetPacketInfo(', 'bool XmaContext::TrySetupNextLoopOld(', 'bool XmaContext::ValidFrameOffsetOld(', 'size_t XmaContext::GetNextFrameOld(', 'int XmaContext::GetFramePacketNumberOld(']: code+=fun(x,sig).replace('XmaContext::','')+'\n'
code+='};\n'
c=(sdk/'include/rex/audio/conversion.h').read_text().split('#else\n')[-1];code+=fun(c,'inline void sequential_6_BE_to_interleaved_2_LE(')+'\n'
code+=r'''
void Put(std::vector<uint8_t>&p,size_t at,uint32_t value,size_t n){for(size_t i=0;i<n;i++){size_t b=at+i;p[b/8]=(p[b/8]&~(1u<<(7-b%8)))|(((value>>(n-i-1))&1)<<(7-b%8));}}
int main(){
 // The reported RP6 overrun: a stream loop points to packet 1 while only packet 0 is mapped.
 XmaContext decoder;XMA_CONTEXT_DATA d;d.loop_count=2;d.loop_start=16416;d.loop_end=18000;d.input_buffer_read_offset=18000;
 xma_old_fix_input_overrun=false;assert(decoder.TrySetupNextLoopOld(&d,false)&&d.input_buffer_read_offset==16416);
 xma_old_fix_input_overrun=true;d.input_buffer_read_offset=18000;d.loop_count=2;assert(!decoder.TrySetupNextLoopOld(&d,false));assert(d.input_buffer_read_offset==18000&&d.loop_count==2);
 d.loop_start=32;assert(decoder.TrySetupNextLoopOld(&d,false)&&d.input_buffer_read_offset==32&&d.loop_count==1);
 std::vector<uint8_t> p(2048,0);Put(p,32,16344,15);Put(p,16375,1,1);
 auto info=decoder.GetPacketInfo(p.data(),32);assert(info.frame_count_==2&&!info.isLastFrameInPacket());
 info=decoder.GetPacketInfo(p.data(),16376);assert(info.current_frame_==1&&info.current_frame_size_==0);
 assert(!decoder.ValidFrameOffsetOld(p.data(),p.size(),16416));
 // Exact end-of-allocation packet read, including zero-width read.
 p.back()=0x81;BitStream bits(p.data(),p.size()*8);bits.SetOffset(16376);assert(bits.Read(8)==0x81);assert(bits.Read(0)==0);
 // Each channel has a known independent impulse: rear channels must stay on their own side.
 for(int channel=0;channel<6;channel++){
  float input[6]={},output[2]={};input[channel]=rex::byte_swap(1.0f);sequential_6_BE_to_interleaved_2_LE(output,input,1);
  float l=(channel==0||channel==4)?0.4f:channel==2?0.2f:0;
  float r=(channel==1||channel==5)?0.4f:channel==2?0.2f:0;
  assert(std::abs(output[0]-l)<1e-6&&std::abs(output[1]-r)<1e-6);
 }
 std::cout<<"PASS: RP6 16416/16384 loop overrun reproduced and prevented; split-header counting; packet-end reads; all six channel impulses.\n";
}
'''
p=out/'audio-regression.cpp';p.write_text(code);exe=out/'audio-regression';cmd=[*CXX,'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',str(p),'-o',str(exe)]
subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],text=True,capture_output=True);(out/'audio-regression.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);assert r.returncode==0
(out/'audio-verification.json').write_text(json.dumps({'method':'Actual XMA parser/loop method bodies, BitStream, scalar ARM downmix, ASAN+UBSAN; no FFmpeg or audio device invoked','result':r.stdout,'sources':{str(sdk/f):hashlib.sha256((sdk/f).read_bytes()).hexdigest() for f in ['src/audio/xma_context.cpp','include/rex/audio/conversion.h','src/core/bit_stream.cpp']},'compile':cmd},indent=2)+'\n')
