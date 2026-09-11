from pathlib import Path
import subprocess,json,hashlib,re
from common import b, sdk, out, span, CXX
b=b.resolve();sdk=sdk.resolve().resolve()
gpu=(b/'src/skate3_native_scene_gpu.cpp').read_text();post=(b/'src/skate3_native_scene_post.cpp').read_text();runtime=(sdk/'src/graphics/vulkan/native_rhi_vulkan.cpp').read_text()
def fun(s,sig):x,y=span(s,sig);return s[x:y]
def layout(s,name):
 end=s.index('g_r.'+name+' = ');start=s.rindex('    nrhi::BindingLayoutDesc ld;',0,end)
 return 'nrhi::BindingLayoutDesc '+name+'(){\n'+s[start:end]+'return ld;\n}\n'
code=r'''#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <unordered_map>
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
'''
code+='#include "'+str(sdk/'include/rex/graphics/native_rhi.h')+'"\nnamespace nrhi=rex::graphics::nrhi;\n'
code+='#include "'+str(b/'src/native/shaders/spirv/skate3_native_shaders_spirv.h')+'"\n'
code+=r'''
using Bindings=std::set<std::pair<uint32_t,uint32_t>>;
std::map<VkDescriptorSetLayout,std::vector<uint32_t>> captured;
Bindings pipeline_bindings;
size_t pipeline_set_count=0;
namespace ui::vulkan {
struct VulkanDevice {
 struct Properties{bool samplerAnisotropy=true;float maxSamplerAnisotropy=16;uint32_t maxBoundDescriptorSets=4;} p;
 struct Functions {
  VkResult vkCreateSampler(VkDevice,const VkSamplerCreateInfo*,const void*,VkSampler* s) const {*s=(VkSampler)1;return VK_SUCCESS;}
  VkResult vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo* info,const void*,VkDescriptorSetLayout* l)const{
   *l=(VkDescriptorSetLayout)(captured.size()+1);auto& v=captured[*l];
   for(uint32_t i=0;i<info->bindingCount;i++){auto n=info->pBindings[i].binding;assert(std::find(v.begin(),v.end(),n)==v.end());v.push_back(n);}return VK_SUCCESS;
  }
  VkResult vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo* info,const void*,VkPipelineLayout* l)const{
   pipeline_bindings.clear();pipeline_set_count=info->setLayoutCount;
   for(uint32_t i=0;i<info->setLayoutCount;i++)for(auto n:captured.at(info->pSetLayouts[i]))pipeline_bindings.emplace(i,n);
   *l=(VkPipelineLayout)1;return VK_SUCCESS;
  }
 } f;
 const Properties& properties() const{return p;}const Functions& functions()const{return f;}VkDevice device()const{return VK_NULL_HANDLE;}
};
}
inline constexpr uint32_t kMaxPackedTableSlots=16;
'''
code+=fun(runtime,'class NrBindingLayoutVulkan :')+';\n'
code+='struct TestDevice {ui::vulkan::VulkanDevice* vulkan_device_;std::vector<NrBindingLayoutVulkan*> layouts_;~TestDevice(){for(auto p:layouts_)delete p;}\n'
code+=fun(runtime,'  nrhi::BindingLayout* CreateBindingLayout(').replace(' override','')+'\n};\n'
code+=fun(runtime,'  static bool RemapTableDescriptorSets(')+'\n'+fun(gpu,'nrhi::ShaderDesc MakeShaderDesc(')+'\n'
for s,n in [(gpu,'layout'),(gpu,'pfx_layout'),(post,'ao_layout'),(post,'ssr_layout'),(post,'hdr_layout')]:code+=layout(s,n)
code+=r'''
Bindings Reflect(const uint32_t* w,size_t bytes){
 std::map<uint32_t,std::map<uint32_t,uint32_t>> d;
 for(size_t i=5;i<bytes/4;){auto n=w[i]>>16,op=w[i]&0xffff;assert(n>0&&i+n<=bytes/4);if(op==71&&n>=4&&(w[i+2]==33||w[i+2]==34))d[w[i+1]][w[i+2]]=w[i+3];i+=n;}
 Bindings r;for(auto&[id,x]:d)if(x.contains(33)&&x.contains(34))r.emplace(x[34],x[33]);return r;
}
int main(){
 ui::vulkan::VulkanDevice vk;TestDevice device{&vk,{}};
 std::map<std::string,Bindings> layouts;
 for(auto [name,desc]:std::vector<std::pair<std::string,nrhi::BindingLayoutDesc>>{{"scene",layout()},{"pfx",pfx_layout()},{"ssao",ao_layout()},{"ssr",ssr_layout()},{"hdr",hdr_layout()}}){
  auto* l=static_cast<NrBindingLayoutVulkan*>(device.CreateBindingLayout(desc));assert(l&&pipeline_set_count==2);layouts[name]=pipeline_bindings;
  // The API-facing table metadata must address exactly the slots the driver declares.
  for(uint32_t i=0;i<desc.param_count;i++)if(desc.params[i].kind==nrhi::BindingParamKind::kTextureTable){auto& p=l->params[i];for(uint32_t j=0;j<p.table_size;j++)assert(pipeline_bindings.contains({p.set_index,p.table_binding_base+j}));}
 }
 size_t tested=0;
 for(const auto& blob:skate3::native_spirv::kNativeSpirvBlobs){
  std::string file=blob.file;
  std::string family=file=="ssao.hlsl"?"ssao":file=="ssr.hlsl"?"ssr":file=="hdr.hlsl"?"hdr":file=="photo_fx.hlsl"?"pfx":"scene";
  auto sd=MakeShaderDesc(nrhi::ShaderStage::kPixel,blob.file,"",blob.entry,nullptr,blob.variant);
  assert(sd.spirv==blob.data&&sd.spirv_size_bytes==blob.size_bytes);
  std::vector<uint32_t> mapped;
  bool changed=RemapTableDescriptorSets(sd.spirv,sd.spirv_size_bytes,&mapped);
  auto actual=Reflect(changed?mapped.data():sd.spirv,changed?mapped.size()*4:sd.spirv_size_bytes);
  for(auto bind:actual)if(!layouts.at(family).contains(bind)){std::cerr<<file<<' '<<blob.entry<<' '<<blob.variant<<" undeclared "<<bind.first<<','<<bind.second<<'\n';return 1;}
  tested++;
 }
 vk.p.maxBoundDescriptorSets=1;assert(device.CreateBindingLayout(layout())==nullptr);
 std::vector<uint32_t> unchanged{42};assert(!RemapTableDescriptorSets(nullptr,0,&unchanged));assert(unchanged==std::vector<uint32_t>{42});
 std::cout<<"PASS: "<<tested<<" shipped shader variants fit five production layouts, each using two descriptor sets; one-set device rejected.\n";
}
'''
p=out/'shader-regression.cpp';p.write_text(code);exe=out/'shader-regression';cmd=[*CXX,'-std=c++20','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(exe)]
# Use only Vulkan headers with the host compiler, never Android libc headers.
import os
candidates = [sdk/'thirdparty/vulkan-headers/include', sdk/'thirdparty/Vulkan-Headers/include']
if os.environ.get('VULKAN_HEADERS'):
 candidates.insert(0, Path(os.environ['VULKAN_HEADERS']))
candidates += [p.parents[1] for p in (b/'out/build/android-release').glob('_deps/*/include/vulkan/vulkan.h')]
headers = next((p for p in candidates if (p/'vulkan/vulkan.h').is_file()), None)
assert headers, 'Initialize submodules or set VULKAN_HEADERS to the include directory containing vulkan/vulkan.h'
cmd.insert(len(CXX), '-I'+str(headers))
subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],text=True,capture_output=True,check=True);print(r.stdout)
(out/'shader-verification.json').write_text(json.dumps({'method':'Actual production layout creation, SPIR-V remapping, scene lookup and shipped blobs; Vulkan API mocked, ASAN+UBSAN host run','result':r.stdout,'sources':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [b/'src/skate3_native_scene_gpu.cpp',b/'src/skate3_native_scene_post.cpp',sdk/'src/graphics/vulkan/native_rhi_vulkan.cpp']},'compile':cmd},indent=2)+'\n')
