#!/usr/bin/env python3
"""Linux host regression tests for the Chakeyy NVN port; no game files or GPU needed."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-include', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    headers = args.vulkan_include.resolve()
    if not (headers / 'vulkan/vulkan.h').is_file():
        parser.error('--vulkan-include must contain vulkan/vulkan.h')
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = ['-std=c++20', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
             '-I', str(ROOT / 'runtime/src'), '-I', str(ROOT / 'runtime/include'), '-I', str(headers)]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    tests = ['nvn_dynamic_offsets', 'nvn_command_blob', 'nvn_command_pool', 'nvn_constants_cache', 'nvn_descriptor_cache']
    with tempfile.TemporaryDirectory(prefix='p4g-nvn-tests-') as directory:
        for name in tests:
            binary = Path(directory) / name
            subprocess.run(compiler + flags + [str(ROOT / 'tests' / (name + '_test.cpp')),
                           '-Wl,--gc-sections', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(f'{name}: PASS', flush=True)


if __name__ == '__main__':
    main()
