#!/usr/bin/env python3
"""Mark an exported Wasm function to run automatically during instantiation.

Inputs:
  module.wasm     A Wasm binary in the project's supported subset.
  --export NAME   An existing function export with signature () -> void.
  --out FILE      Where to write the modified Wasm; it may equal the input path.

Example:
  python3 scripts/wasm_set_start.py module.wasm --export init --out started.wasm

We need this optional step because wasm-ld-15's --entry does not itself insert
an instantiation-time Wasm start section. Normal exported function calls do
not need it.

The script rejects modules with an existing start function, checks the selected
export, inserts a start section, and validates the result before publishing it.
Existing sections and function bodies are preserved.

Success (exit code 0) means a validated output Wasm with the requested start
function was written. The function has not been executed. Adaptation, DPU
compilation, resource checks and execution must still be performed.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from pimwasm_compile import Reader, Rejected, inspect_wasm, require, encode_uleb


def set_start(data, export_name):
    info = inspect_wasm(data)
    require(info['start_function'] is None, 'module already has a start function')
    function = next((f for f in info['exports'] if f['name'] == export_name), None)
    require(function is not None, f'no function export named {export_name}')
    require(function['parameter_types'] == [] and function['result_type'] == 'void',
            'start function must have signature () -> void')

    # Place section 8 before code/data, preserving all existing bytes, including
    # custom sections. No function indices, exports or bodies are changed.
    reader = Reader(data)
    reader.take(8)
    insertion = len(data)
    while reader.pos < len(data):
        position = reader.pos
        section_id = reader.byte()
        reader.take(reader.uint())
        if section_id > 8:
            insertion = position
            break
    index = encode_uleb(function['index'])
    result = data[:insertion] + b'\x08' + encode_uleb(len(index)) + index + data[insertion:]
    require(inspect_wasm(result)['start_function'] == function['index'],
            'start section verification failed')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wasm', type=Path)
    parser.add_argument('--export', dest='export_name', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        subprocess.run(['wasm-validate', '--enable-extended-const', str(args.wasm)], check=True)
        result = set_start(args.wasm.read_bytes(), args.export_name)
        # Publish only a validated artifact, allowing input == output safely.
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.out.parent) as directory:
            staged = Path(directory) / 'module.wasm'
            staged.write_bytes(result)
            subprocess.run(['wasm-validate', '--enable-extended-const', str(staged)], check=True)
            staged.replace(args.out)
    except (Rejected, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Cannot set Wasm start: {error}\n')


if __name__ == '__main__':
    main()
