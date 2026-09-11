"""Compile and force the startup race using current production method bodies."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
from common import sdk as SDK, out as OUT, CXX
CASE = OUT / 'startup-regression'
CASE.mkdir(exist_ok=True)
SOURCE = 'src/core/threading_posix.cpp'
base = subprocess.check_output(['git', '-C', str(SDK), 'show', '004344c6f4e0f265a1492ca292b484bb530f09ff:'+SOURCE], text=True)
fixed = (SDK/SOURCE).read_text()

def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start)+1
    level = 1
    while level:
        if text[end] == '{': level += 1
        elif text[end] == '}': level -= 1
        end += 1
    return text[start:end]+'\n'

for label, source in [('baseline', base), ('patched', fixed)]:
    (CASE/(label+'-start.inc')).write_text(extract(source, 'void* PosixCondition<Thread>::ThreadStartRoutine('))
for name, signature in [('resume', '  bool Resume(uint32_t* out_previous_suspend_count = nullptr)'),
                        ('suspend', '  bool Suspend(uint32_t* out_previous_suspend_count = nullptr)'),
                        ('waitstarted', '  void WaitStarted() const')]:
    method = extract(fixed, signature)
    assert method == extract(base, signature), f'{name} unexpectedly changed'
    (CASE/(name+'-method.inc')).write_text(method)
shutil.copyfile(HERE/'test_startup.cpp', CASE/'test_startup.cpp')

results = {}
for label, definitions in [('baseline', ['-DBASELINE']), ('patched', [])]:
    binary = CASE/('test_'+label)
    command = [*CXX, '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-pthread', *definitions, str(CASE/'test_startup.cpp'), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=60)
    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
    (OUT/('startup-'+label+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stdout+result.stderr
    expected = 'REPRODUCED: first Resume lost' if label == 'baseline' else 'PASS: 6 deterministic'
    assert expected in result.stdout
    results[label] = {'compile_command': command, 'result': result.stdout.strip(),
                      'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    print(result.stdout.strip())

results['inputs'] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in CASE.iterdir()
                     if f.is_file() and f.suffix in ('.inc', '.cpp')}
results['production_source_sha256'] = hashlib.sha256(fixed.encode()).hexdigest()
results['limits'] = 'Production method bodies with test-only synchronization adapters; signal delivery is stubbed. Gameplay requires device testing.'
(OUT/'startup-verification.json').write_text(json.dumps(results, indent=2)+'\n')
