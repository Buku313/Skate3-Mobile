"""Run the combined app/runtime host checks on macOS or Linux."""
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
for name in ('startup', 'audio', 'shaders', 'io', 'guest-guards'):
    print(f'Running {name} regression', flush=True)
    subprocess.run([sys.executable, str(here / f'check-{name}.py')], check=True, timeout=120)
print('PASS: all five RP6 host regression checks')
