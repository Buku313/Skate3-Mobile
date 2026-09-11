from pathlib import Path
import re,json,hashlib,subprocess
from common import b, sdk, out, span, CXX
p=b/'src/skate3_audio_fixes.cpp';s=p.read_text()
def fun(sig):x,y=span(s,sig);return s[x:y]
code='#include <cassert>\n#include <cstdint>\n#include <cstring>\n#include <iostream>\n#include <thread>\n#include <map>\n'
fields=[f'r{i}' for i in range(3,11)]+[f'f{i}' for i in range(1,14)]+['ctr']
code+='union Reg{uint64_t u64;uint32_t u32;};struct PPCContext{'+''.join('Reg '+f+'{};' for f in fields)+'uint64_t lr=0;};using PPCFunc=void(PPCContext&,uint8_t*);\n'
code+='std::map<uint32_t,uint32_t> memory;\n#define REX_LOAD_U32(x) memory.at(x)\n'
for c in ['kSysLockFn','kSysCritSec','kLockAttempts']:code+=re.search(r'constexpr[^\n]+\b'+c+r'\s*=\s*[^;]+;',s)[0]+'\n'
code+='int tries=0,unlocks=0,calls=0;bool available=true;\nvoid clobber(PPCContext& c,uint64_t value){'+''.join(f'c.{f}.u64=value;' for f in fields)+'c.lr=value;}\n'
code+='void __imp__RtlTryEnterCriticalSection(PPCContext& c,uint8_t*){++tries;clobber(c,999);c.r3.u32=available;}\nvoid __imp__RtlLeaveCriticalSection(PPCContext& c,uint8_t*){++unlocks;clobber(c,888);}\n'
code+=fun('bool SystemTryLock(')+'\n'+fun('void SystemUnlock(')+'\n'+fun('struct VolatileState')+';\n'+fun('void RunUnderSystemLock(')+'\n'
code+='void original(PPCContext& c,uint8_t*){++calls;'+''.join(f'assert(c.{f}.u64==123);' for f in fields)+'assert(c.lr==123);clobber(c,456);}\n'
code+='int main(){PPCContext c;memory[184]=0;memory[196]=0x300;for(int scenario=0;scenario<3;scenario++){tries=unlocks=calls=0;available=scenario!=1;clobber(c,123);RunUnderSystemLock(c,nullptr,scenario==2?0:100,original);assert(calls==1);assert(tries==(scenario==2?0:scenario==1?16:1));assert(unlocks==(scenario==0?1:0));'+''.join(f'assert(c.{f}.u64==456);' for f in fields)+'assert(c.lr==456);}\nstd::cout<<"PASS: audio append guards preserve arguments and returns; one balanced unlock on acquisition; contended path bounded to 16 attempts; zero-System bypass.\\n";}\n'
c=out/'guest-guard-regression.cpp';c.write_text(code);exe=out/'guest-guard-regression';cmd=[*CXX,'-std=c++20','-O1','-fsanitize=address,undefined',str(c),'-o',str(exe)];subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],check=True,text=True,capture_output=True);print(r.stdout)
(out/'guest-guard-verification.json').write_text(json.dumps({'method':'Actual SystemTryLock, SystemUnlock, VolatileState and RunUnderSystemLock with stub guest calls; ASAN+UBSAN','result':r.stdout,'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest()},indent=2)+'\n')
