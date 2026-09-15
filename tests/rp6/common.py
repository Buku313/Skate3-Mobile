"""Host regression support; reads production source without changing it."""
from pathlib import Path
import os
import re
import shlex

b = Path(__file__).resolve().parents[2]
sdk = b / 'third_party/rexglue-sdk'
out = Path(os.environ.get('RP6_TEST_OUTPUT', str(b / 'out/rp6-checks'))).resolve()
out.mkdir(parents=True, exist_ok=True)
CXX = shlex.split(os.environ.get('CXX', 'clang++'))

def span(s,sig):
 st=s.index(sig);op=s.index('{',st);d=0
 for m in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',s[op:]):
  if m[0]=='{':d+=1
  elif m[0]=='}':
   d-=1
   if d==0:return st,op+m.end()
 raise ValueError(sig)
