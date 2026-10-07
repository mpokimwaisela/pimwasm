#!/usr/bin/env python3
"""Check whether a compiled PIMWASM DPU program fits the supported device limits.

Inputs:
  ELF             A linked native DPU executable, often named dpu.unchecked.
  manifest.json   Module metadata in the same directory as the executable.
  --report FILE   Optional output path for a JSON resource report.

Example:
  python3 scripts/verify_pimwasm_dpu.py build/examples/count/dpu.unchecked \
      --report build/examples/count/dpu-resources.json

The verifier checks the executable format, one-tasklet configuration, linked
IRAM/WRAM/MRAM usage, declared guest memory backing and native stack bounds.
It examines native calls and compiler stack-size metadata. Recursive call cycles
must cross generated depth guards; missing metadata or unguarded cycles reject
the program. SDK-reserved stack space is included in the admission check.

We need this after compilation because an accepted Wasm module can still exceed
DPU resources. The Makefile publishes the final dpu executable only after this
check passes.

Success (exit code 0 and "DPU resources accepted") means the executable passed
these resource checks. It does not mean hardware execution, functional
correctness, or a validated sandbox. This verifier trusts compiler output;
it is not a general machine-code security verifier.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

from pimwasm_compile import MAX_INITIAL_MEMORY_PAGES, WASM_PAGE_BYTES, Rejected, require

MRAM_START = 0x08000000
MRAM_BYTES = MAX_INITIAL_MEMORY_PAGES * WASM_PAGE_BYTES
MRAM_END = MRAM_START + MRAM_BYTES


def inspect_elf(path):
    data = Path(path).read_bytes()
    require(len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01",
            "expected ELF32 little-endian native binary")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    require(header[1:3] == (2, 245), "expected linked DPU executable")
    shoff, shentsize, shnum, shstr = header[6], header[11], header[12], header[13]
    require(shentsize == 40 and shnum and shstr < shnum,
            "unsupported ELF section table")
    require(shoff + shnum * 40 <= len(data), "truncated ELF section table")
    raw = [struct.unpack_from("<IIIIIIIIII", data, shoff + i*40) for i in range(shnum)]

    def payload(section):
        offset, size = section[4:6]
        require(offset + size <= len(data), "truncated ELF section")
        return data[offset:offset + size]

    def string(table, offset):
        require(offset < len(table), "invalid ELF string offset")
        end = table.find(b"\0", offset)
        require(end >= 0, "unterminated ELF string")
        return table[offset:end].decode("ascii")

    names = payload(raw[shstr])
    sections = {string(names, s[0]): s for s in raw}
    require(".symtab" in sections and ".stack_sizes" in sections,
            "native function or stack-size metadata missing")
    symtab = sections[".symtab"]
    require(symtab[9] == 16 and symtab[6] < shnum, "unsupported symbol table")
    strings = payload(raw[symtab[6]])
    symbols = {}
    functions = {}
    symbols_raw = payload(symtab)
    require(len(symbols_raw) % 16 == 0, "truncated symbol table")
    for index in range(0, len(symbols_raw), 16):
        name, address, size, info, _, section = struct.unpack_from("<IIIBBH", symbols_raw, index)
        if not name:
            continue
        name = string(strings, name)
        symbols[name] = (address, size, section)
        if info & 15 == 2:
            require(section not in (0, 0xfff1) and size > 0,
                    f"undefined/empty native function: {name}")
            if address in functions:
                previous, previous_size = functions[address]
                require(previous_size == size and symbols[previous][2] == section,
                        "aliased function symbols disagree on body size or section")
            else:
                functions[address] = (name, size)
    frames = {}
    stack_data = payload(sections[".stack_sizes"])
    position = 0
    while position < len(stack_data):
        require(position + 4 <= len(stack_data), "truncated stack-size address")
        address, = struct.unpack_from("<I", stack_data, position)
        position += 4
        size, shift = 0, 0
        while True:
            require(position < len(stack_data) and shift <= 28,
                    "invalid stack-size encoding")
            byte = stack_data[position]
            position += 1
            size |= (byte & 127) << shift
            if not byte & 128:
                break
            shift += 7
        require(address not in frames, "duplicate stack-size record")
        frames[address] = size
    require(functions and frames.keys() == functions.keys(),
            "stack-size metadata does not cover exactly every linked function")
    return sections, symbols, functions, frames


def check_calls(path, symbols, functions, manifest):
    result = subprocess.run(["llvm-objdump", "-d", str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, f"cannot inspect DPU calls: {result.stderr.strip()}")
    edges = {address: set() for address in functions}

    def owner(address):
        found = [start for start, (_, size) in functions.items() if start <= address < start + size]
        require(len(found) == 1, "instruction outside a unique native function")
        return found[0]

    for line in result.stdout.splitlines():
        instruction = re.match(r"([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+){8}\s*(.+)", line)
        if not instruction:
            continue
        address, text = int(instruction[1], 16), instruction[2]
        caller = owner(address)
        if text.startswith("call "):
            call = re.fullmatch(r"call r23, (\w+)", text)
            require(call is not None and call[1] in symbols,
                    f"unrecognized/indirect native call: {text}")
            target = symbols[call[1]][0]
            require(target in functions, f"native call target lacks function metadata: {text}")
            edges[caller].add(target)
        # Include direct inter-function tail branches; ordinary local loops do
        # not create native recursion and must remain permitted.
        branch = re.search(r"(?:^|, )(0x[0-9a-f]+)$", text)
        if branch:
            target = int(branch[1], 16)
            if target in functions and target != caller:
                edges[caller].add(target)
    guarded={symbols[name][0] for name in manifest.get('native_guest_functions',[]) if name in symbols}
    recursive=manifest.get('recursive_calls',False)
    visited, active = set(), set()

    def visit(function):
        require(function not in active, "recursive native call graph unsupported")
        if function in visited:
            return
        active.add(function)
        for callee in edges[function]:
            if not recursive or callee not in guarded:
                visit(callee)
        active.remove(function)
        visited.add(function)

    for function in functions:
        if not recursive or function not in guarded:
            visit(function)
    # Every admitted native cycle must pass a generated Wasm depth guard.
    # Find every vertex that can return to itself; only those frames can repeat
    # on a native call path. Each unguarded interval is acyclic as checked above.
    cyclic=set()
    for start in functions:
        pending=list(edges[start]); reached=set()
        while pending:
            node=pending.pop()
            if node==start:
                cyclic.add(start);break
            if node not in reached:
                reached.add(node);pending.extend(edges[node])
    return cyclic


def verify(path):
    path = Path(path)
    sections, symbols, functions, frames = inspect_elf(path)
    for name in ("NR_TASKLETS", "STACK_SIZE_TASKLET_0", "DPU_IRAM_SIZE", "DPU_WRAM_SIZE"):
        require(name in symbols, f"missing resource symbol {name}")
    require(symbols["NR_TASKLETS"][0] == 1, "resource proof requires one tasklet")
    reserved = symbols["STACK_SIZE_TASKLET_0"][0]
    require(reserved > 64, "insufficient reserved native stack")
    manifest = json.loads((path.parent / "manifest.json").read_text())
    cyclic=check_calls(path, symbols, functions, manifest)
    limit=manifest.get('call_depth_limit',256)
    require(type(limit) is int and 1 <= limit <= 256, 'invalid dynamic call depth bound')
    require('pimwasm_call_depth_limit' in symbols,'missing compiled call depth bound')
    address,size,_=symbols['pimwasm_call_depth_limit']
    backing=[s for s in sections.values() if s[2]&2 and s[1]!=8 and s[3]<=address and address+4<=s[3]+s[5]]
    require(size==4 and len(backing)==1,'compiled call depth bound lacks exact backing')
    section=backing[0]
    compiled_limit=struct.unpack_from('<I',path.read_bytes(),section[4]+address-section[3])[0]
    require(compiled_limit==limit,'manifest call depth differs from compiled depth guard')
    # One extra callee frame exists before its prologue detects exhaustion.
    # Noncyclic functions occur at most once; cyclic frames repeat at most
    # limit+1 times because every cycle crosses a checked Wasm function entry.
    frame_sum = sum(frames.values())
    cyclic_sum=sum(frames[address] for address in cyclic)
    stack_bound=frame_sum + limit*cyclic_sum
    require(stack_bound + 64 <= reserved,
            f"native stack bound {stack_bound}+64 exceeds reserved {reserved} bytes")
    text = sections.get(".text")
    require(text is not None and text[3] == 0x80000000 and text[5] <= symbols["DPU_IRAM_SIZE"][0],
            "native code exceeds linked IRAM limit")
    wram = [s for s in sections.values() if s[2] & 2 and s[3] < 0x10000]
    require(wram and max(s[3] + s[5] for s in wram) <= symbols["DPU_WRAM_SIZE"][0],
            "native allocation exceeds linked WRAM limit")
    declared = manifest["memory_bytes"]
    require(type(declared) is int and 0 <= declared <= MRAM_BYTES and
            declared % WASM_PAGE_BYTES == 0,
            "declared memory must be zero or whole pages within the physical MRAM limit")
    capacity = manifest.get('memory_capacity_bytes')
    require(type(capacity) is int and declared <= capacity <= MRAM_BYTES and
            capacity % WASM_PAGE_BYTES == 0,
            'memory capacity must cover initial memory within physical MRAM')
    require(type(manifest.get('memory_count')) is int and manifest['memory_count'] in (0, 1),
            'invalid memory count')
    maximum = manifest['memory_max_pages']
    require(type(maximum) is int and declared // WASM_PAGE_BYTES <= maximum <= 65536 and
            (manifest['memory_count'] == 1 or (declared == 0 and maximum == 0 and capacity == 0)), 'invalid declared memory maximum')
    expected_capacity = (min(maximum, MAX_INITIAL_MEMORY_PAGES) * WASM_PAGE_BYTES
                         if 'memory.grow' in manifest['operations'] or 'memory_import' in manifest else declared)
    require(capacity == expected_capacity,
            'memory capacity does not match admitted growth requirements')
    # Include named MRAM sections even if a malformed ELF moves them outside
    # MRAM, and unnamed allocations that start in or overlap its address space.
    # The SDK emits an empty .mram section at MRAM_END when guest_memory occupies
    # all 64 MiB; that section is valid and allocates no additional bytes.
    mram = [s for name, s in sections.items() if s[2] & 2 and
            (name == ".mram" or name.startswith(".mram.") or
             MRAM_START <= s[3] <= MRAM_END or
             s[3] < MRAM_START < s[3] + s[5])]
    require(all(MRAM_START <= s[3] <= MRAM_END and s[5] <= MRAM_END - s[3]
                for s in mram), "native MRAM section exceeds physical address range")
    mram_bytes = sum(s[5] for s in mram)
    if capacity == 0:
        require("guest_memory" not in symbols and mram_bytes == 0,
                "zero-capacity module must have no guest_memory symbol or allocated MRAM")
        guest_bytes = 0
    else:
        require("guest_memory" in symbols, "missing resource symbol guest_memory")
        guest_address, guest_bytes, _ = symbols["guest_memory"]
        require(guest_address == MRAM_START and
                guest_bytes == capacity and mram_bytes == capacity,
                "declared guest memory allocation mismatch")
        # Matching the sum alone would accept overlapping sections or holes.
        # Verify exact contiguous backing, including the region's end, and that
        # the full symbol is backed by an allocated section.
        cursor = MRAM_START
        for section in sorted((s for s in mram if s[5]), key=lambda s: s[3]):
            require(section[3] == cursor, "native MRAM sections overlap or have gaps")
            cursor += section[5]
        require(cursor == guest_address + guest_bytes and
                any(s[3] <= guest_address and guest_bytes <= s[3] + s[5] - guest_address
                    for s in mram), "guest memory lacks exact native MRAM backing")
    # wasm2c's immutable segment arrays use native WRAM, not guest MRAM.
    # Count surviving arrays separately; the WRAM section gate above includes
    # these bytes, padding, instance drop flags, stacks and runtime buffers.
    segment_bytes = 0
    for name, (address, size, _) in symbols.items():
        if name.startswith('data_segment_data_'):
            require(any(s[3] <= address and size <= s[3] + s[5] - address for s in wram),
                    'data segment storage lacks WRAM backing')
            segment_bytes += size
    return {"native_stack_frame_sum": frame_sum, "native_stack_margin": 64,
            "native_stack_bound":stack_bound,"native_recursive_frame_sum":cyclic_sum,
            "call_depth_limit":limit,
            "native_stack_reserved": reserved, "native_functions": len(functions),
            "iram_bytes": text[5], "wram_bytes": sum(s[5] for s in wram),
            "guest_memory_bytes": guest_bytes, "mram_bytes": mram_bytes,
            "memory_count": manifest["memory_count"], "initial_memory_bytes": declared, "memory_capacity_bytes": capacity,
            "data_segment_wram_bytes": segment_bytes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.elf)
    except (Rejected, OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f"PIMWASM DPU resource admission rejected: {error}\n")
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PIMWASM DPU resources accepted: {report['iram_bytes']} code bytes, "
          f"{report['wram_bytes']} WRAM bytes, native stack "
          f"{report['native_stack_bound']}+64 <= {report['native_stack_reserved']}")


if __name__ == "__main__":
    main()
