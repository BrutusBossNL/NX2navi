"""Build and run tests/wb_lazy_check.c: runtime/wb_lazy_choice.h, whether the lazy write-back runs, by the player's
setting, the platform and the GPU driver.

Usage: wb_lazy_check.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'wb_lazy'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('wb_lazy_check.exe' if os.name == 'nt' else 'wb_lazy_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    # every platform's and driver's choice is checked through wb_lazy_choice's arguments; WBL_ON_WINDOWS on the one
    # built for
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'wb_lazy_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
