from pathlib import Path
import json,hashlib,subprocess
from common import b, sdk, out, span, CXX
p=sdk/'src/core/filesystem_posix.cpp';s=p.read_text()
code=r'''#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>
#include <sys/types.h>
#define REXCVAR_GET(x) (x)
#define REXFS_WARN(...) ((void)0)
int filesystem_fault_short_cap=0,filesystem_fault_fail_every=0,filesystem_fault_errno=EINTR;
bool filesystem_read_loop=true;
std::atomic<uint64_t> g_fault_injection_counter{0};
using Hook=bool(*)(void*,size_t,const char*,size_t);std::atomic<Hook> g_host_buffer_fault_hook{nullptr};
bool FaultInjectionMatches(const std::filesystem::path&){return true;}
bool ShouldWarn(uint64_t*){return false;}
std::vector<int> results;size_t call=0;size_t expected_offset=0;
ssize_t pread(int,void* b,size_t count,off_t off){assert(off==expected_offset);int n=results.at(call++);if(n<0){errno=-n;return -1;}n=std::min(count,size_t(n));std::memset(b,0xA5,n);expected_offset+=n;return n;}
ssize_t pwrite(int,const void*,size_t count,off_t off){char b[100];return pread(0,b,std::min(count,sizeof(b)),off);}
struct File{int handle_=0;std::filesystem::path path_="test";
'''
for sig in ['  bool Read(size_t file_offset,','  bool Write(size_t file_offset,']:
 u,v=span(s,sig);code+=s[u:v].replace(' override','')+'\n'
code+=r'''};
void sequence(std::vector<int> v){results=v;call=0;expected_offset=10;}
int main(){File f;char buffer[16]={};size_t count=999;
 sequence({-EINTR,3,5,8});assert(f.Read(10,buffer,16,&count)&&count==16&&call==4);for(char b:buffer)assert((unsigned char)b==0xA5);
 sequence({3,0});assert(f.Read(10,buffer,16,&count)&&count==3&&call==2);
 sequence({4,-EIO});assert(!f.Read(10,buffer,16,&count)&&count==4);
 sequence(std::vector<int>(65,-EAGAIN));assert(!f.Read(10,buffer,16,&count)&&count==0&&call==65);
 sequence({-EINTR,5,11});assert(f.Write(10,buffer,16,&count)&&count==16&&call==3);
 sequence({3,-EIO});assert(!f.Write(10,buffer,16,&count)&&count==3);
 sequence({});assert(f.Read(10,buffer,0,&count)&&count==0&&call==0);
 std::cout<<"PASS: short read/write completion; EINTR retry; EOF; partial-count errors; bounded EAGAIN retry; zero-length read.\n";
}
'''
c=out/'io-regression.cpp';c.write_text(code);exe=out/'io-regression';cmd=[*CXX,'-std=c++20','-O1','-fsanitize=address,undefined',str(c),'-o',str(exe)];subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],check=True,text=True,capture_output=True);print(r.stdout)
(out/'io-verification.json').write_text(json.dumps({'method':'Actual production Read and Write methods, scripted POSIX syscalls, ASAN+UBSAN','result':r.stdout,'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'compile':cmd},indent=2)+'\n')
