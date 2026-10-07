#!/usr/bin/env python3
"""Prepare a WebAssembly module for compilation into a native DPU program.

Inputs:
  input.wasm       An already compiled Wasm binary, not C source or WAT text.
  --out DIRECTORY Where to write the generated bundle.
  --imports FILE  Optional JSON bindings for explicitly supplied native imports.

Example:
  python3 scripts/pimwasm_compile.py input.wasm --out build/examples/my_module

The script checks that the module uses supported features and limits, validates
it with WABT, and runs wasm2c to generate C. It adapts that C to use the DPU
runtime and generates typed invocation glue, initialization code, export
metadata, and a manifest. The original Wasm is retained as module.wasm.
Unsupported modules or unexpected generator output are rejected.

We need this step because ordinary wasm2c output expects runtime facilities
that are unavailable or different on a DPU, such as host memory and math
support. The adapter connects generated Wasm computation to our DPU runtime;
it does not replace the computation with a handwritten kernel.

Success (exit code 0 and "Accepted module") means the checks passed and the
source bundle was generated. It does NOT mean a native DPU binary was compiled,
that the program fits the device, or that it ran correctly. The Makefile next
invokes the DPU compiler and verify_pimwasm_dpu.py; the native host runs it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

WABT_VERSION = "1.0.36 (git~1.0.36-44-g46648b096)"
# This is the generic prelude of the installed generator, with a normalized
# module/header name. It is independent of the guest's function and loop.
PRELUDE_SHA256 = "dd0caa9d9e399d51b7f67833f376e428fa629b7a0cd9a3f598b0da09a75467cf"
NOMEM_PRELUDE_SHA256 = "3790626f531a9c6a537b6c42f7c8130287a5da767d7bc6e3e720d5196225bbf4"
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,62}\Z")
MODULE_TYPE = "w2c_pimwasm__module"
MAX_FUNCTIONS = 256
MAX_FUNCTION_TYPES = 256
MAX_EXPORT_NAME_BYTES = 63
MAX_MODULE_BYTES = 1024 * 1024
WASM_PAGE_BYTES = 65536
MAX_INITIAL_MEMORY_PAGES = 1024
RECURSIVE_CALL_DEPTH = 8
class Rejected(ValueError):
    """The artifact is invalid or uses unsupported Wasm features."""


def require(condition, message):
    if not condition:
        raise Rejected(message)


def encode_uleb(value):
    """Encode an unsigned LEB128 value for rewritten Wasm sections."""
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)

class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def byte(self):
        require(self.pos < len(self.data), "truncated Wasm encoding")
        value = self.data[self.pos]
        self.pos += 1
        return value

    def take(self, size):
        require(size <= len(self.data) - self.pos, "truncated Wasm section")
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value

    def uint(self):
        value = 0
        for index in range(5):
            byte = self.byte()
            require(index < 4 or byte <= 0x0f, "invalid u32 LEB encoding")
            value |= (byte & 0x7f) << (7 * index)
            if not byte & 0x80:
                return value
        raise Rejected("invalid u32 LEB encoding")

    def sint(self):
        value = 0
        for index in range(5):
            byte = self.byte()
            require(index < 4 or byte <= 0x07 or 0x78 <= byte <= 0x7f,
                    "invalid i32 LEB encoding")
            value |= (byte & 0x7f) << (7 * index)
            if not byte & 0x80:
                if byte & 0x40:
                    value -= 1 << (7 * (index + 1))
                require(-(1 << 31) <= value < 1 << 31, "invalid i32 constant")
                return value
        raise Rejected("invalid i32 LEB encoding")

    def name(self):
        try:
            return self.take(self.uint()).decode("utf-8")
        except UnicodeDecodeError as error:
            raise Rejected("invalid UTF-8 name") from error

    def done(self):
        require(self.pos == len(self.data), "unexpected trailing section bytes")


# Control flow is generated as ordinary C; traps use the DPU runtime.
SIMPLE_OPS = {
    0x00: "unreachable", 0x01: "nop", 0x0f: "return", 0x1a: "drop",
    0x1b: "select",
}
INDEX_OPS = {0x0c: "br", 0x0d: "br_if", 0x20: "local.get",
             0x21: "local.set", 0x22: "local.tee", 0x23: "global.get",
             0x24: "global.set"}


# Scalar numeric instructions only; no SIMD or threads.
VALUE_TYPES = {0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64'}
C_TYPES = {'void': 'void', 'i32': 'u32', 'i64': 'u64', 'f32': 'f32', 'f64': 'f64', 'externref': 'wasm_rt_externref_t'}
NUMERIC_OPS = {
    0x45: "i32.eqz",
    0x46: "i32.eq",
    0x47: "i32.ne",
    0x48: "i32.lt_s",
    0x49: "i32.lt_u",
    0x4a: "i32.gt_s",
    0x4b: "i32.gt_u",
    0x4c: "i32.le_s",
    0x4d: "i32.le_u",
    0x4e: "i32.ge_s",
    0x4f: "i32.ge_u",
    0x50: "i64.eqz",
    0x51: "i64.eq",
    0x52: "i64.ne",
    0x53: "i64.lt_s",
    0x54: "i64.lt_u",
    0x55: "i64.gt_s",
    0x56: "i64.gt_u",
    0x57: "i64.le_s",
    0x58: "i64.le_u",
    0x59: "i64.ge_s",
    0x5a: "i64.ge_u",
    0x5b: "f32.eq",
    0x5c: "f32.ne",
    0x5d: "f32.lt",
    0x5e: "f32.gt",
    0x5f: "f32.le",
    0x60: "f32.ge",
    0x61: "f64.eq",
    0x62: "f64.ne",
    0x63: "f64.lt",
    0x64: "f64.gt",
    0x65: "f64.le",
    0x66: "f64.ge",
    0x67: "i32.clz",
    0x68: "i32.ctz",
    0x69: "i32.popcnt",
    0x6a: "i32.add",
    0x6b: "i32.sub",
    0x6c: "i32.mul",
    0x6d: "i32.div_s",
    0x6e: "i32.div_u",
    0x6f: "i32.rem_s",
    0x70: "i32.rem_u",
    0x71: "i32.and",
    0x72: "i32.or",
    0x73: "i32.xor",
    0x74: "i32.shl",
    0x75: "i32.shr_s",
    0x76: "i32.shr_u",
    0x77: "i32.rotl",
    0x78: "i32.rotr",
    0x79: "i64.clz",
    0x7a: "i64.ctz",
    0x7b: "i64.popcnt",
    0x7c: "i64.add",
    0x7d: "i64.sub",
    0x7e: "i64.mul",
    0x7f: "i64.div_s",
    0x80: "i64.div_u",
    0x81: "i64.rem_s",
    0x82: "i64.rem_u",
    0x83: "i64.and",
    0x84: "i64.or",
    0x85: "i64.xor",
    0x86: "i64.shl",
    0x87: "i64.shr_s",
    0x88: "i64.shr_u",
    0x89: "i64.rotl",
    0x8a: "i64.rotr",
    0x8b: "f32.abs",
    0x8c: "f32.neg",
    0x8d: "f32.ceil",
    0x8e: "f32.floor",
    0x8f: "f32.trunc",
    0x90: "f32.nearest",
    0x91: "f32.sqrt",
    0x92: "f32.add",
    0x93: "f32.sub",
    0x94: "f32.mul",
    0x95: "f32.div",
    0x96: "f32.min",
    0x97: "f32.max",
    0x98: "f32.copysign",
    0x99: "f64.abs",
    0x9a: "f64.neg",
    0x9b: "f64.ceil",
    0x9c: "f64.floor",
    0x9d: "f64.trunc",
    0x9e: "f64.nearest",
    0x9f: "f64.sqrt",
    0xa0: "f64.add",
    0xa1: "f64.sub",
    0xa2: "f64.mul",
    0xa3: "f64.div",
    0xa4: "f64.min",
    0xa5: "f64.max",
    0xa6: "f64.copysign",
    0xa7: "i32.wrap_i64",
    0xa8: "i32.trunc_f32_s",
    0xa9: "i32.trunc_f32_u",
    0xaa: "i32.trunc_f64_s",
    0xab: "i32.trunc_f64_u",
    0xac: "i64.extend_i32_s",
    0xad: "i64.extend_i32_u",
    0xae: "i64.trunc_f32_s",
    0xaf: "i64.trunc_f32_u",
    0xb0: "i64.trunc_f64_s",
    0xb1: "i64.trunc_f64_u",
    0xb2: "f32.convert_i32_s",
    0xb3: "f32.convert_i32_u",
    0xb4: "f32.convert_i64_s",
    0xb5: "f32.convert_i64_u",
    0xb6: "f32.demote_f64",
    0xb7: "f64.convert_i32_s",
    0xb8: "f64.convert_i32_u",
    0xb9: "f64.convert_i64_s",
    0xba: "f64.convert_i64_u",
    0xbb: "f64.promote_f32",
    0xbc: "i32.reinterpret_f32",
    0xbd: "i64.reinterpret_f64",
    0xbe: "f32.reinterpret_i32",
    0xbf: "f64.reinterpret_i64",
    0xc0: "i32.extend8_s",
    0xc1: "i32.extend16_s",
    0xc2: "i64.extend8_s",
    0xc3: "i64.extend16_s",
    0xc4: "i64.extend32_s",
}
MEMORY_OPS = {
    0x28: ("i32.load", 2),
    0x29: ("i64.load", 3),
    0x2a: ("f32.load", 2),
    0x2b: ("f64.load", 3),
    0x2c: ("i32.load8_s", 0),
    0x2d: ("i32.load8_u", 0),
    0x2e: ("i32.load16_s", 1),
    0x2f: ("i32.load16_u", 1),
    0x30: ("i64.load8_s", 0),
    0x31: ("i64.load8_u", 0),
    0x32: ("i64.load16_s", 1),
    0x33: ("i64.load16_u", 1),
    0x34: ("i64.load32_s", 2),
    0x35: ("i64.load32_u", 2),
    0x36: ("i32.store", 2),
    0x37: ("i64.store", 3),
    0x38: ("f32.store", 2),
    0x39: ("f64.store", 3),
    0x3a: ("i32.store8", 0),
    0x3b: ("i32.store16", 1),
    0x3c: ("i64.store8", 0),
    0x3d: ("i64.store16", 1),
    0x3e: ("i64.store32", 2),
}


def scalar_type(reader, role):
    byte = reader.byte()
    require(byte in VALUE_TYPES, f"only scalar numeric {role} supported")
    return VALUE_TYPES[byte]


def function_type(reader):
    byte=reader.byte()
    require(byte in (*VALUE_TYPES,0x6f), 'only scalar numeric parameters/results or internal externref supported')
    return 'externref' if byte==0x6f else VALUE_TYPES[byte]


def constant(reader, opcode):
    if opcode == 0x41:
        reader.sint()
    elif opcode == 0x42:
        # Validate signed LEB64 terminal bits, not just the byte count.
        for i in range(10):
            b = reader.byte()
            require(i < 9 or b in (0, 0x7f), 'invalid i64 LEB encoding')
            if not b & 0x80:
                return
        raise Rejected('invalid i64 LEB encoding')
    elif opcode in (0x43, 0x44):
        reader.take(4 if opcode == 0x43 else 8)
    else:
        raise Rejected('unsupported constant initializer')


MAX_RESULTS = 4
MAX_TABLE_ENTRIES = 64
TYPE_LETTERS = {'i32': 'i', 'i64': 'j', 'f32': 'f', 'f64': 'd'}

def c_result(results):
    if not results: return 'void'
    if len(results) == 1: return C_TYPES[results[0]]
    return 'struct wasm_multi_' + ''.join(TYPE_LETTERS[t] for t in results)

def const_expression(reader, expected, imported_globals=()):
    """Evaluate only the extended-const integer +,-,* subset and scalar consts.
    Arithmetic uses Wasm wrapping semantics, never Python unbounded offsets.
    wasm-validate independently verifies the original expression's types.
    """
    stack = []
    count = 0
    while True:
        op = reader.byte()
        if op == 0x0b: break
        count += 1
        require(count <= 256, 'at most 256 initializer instructions supported')
        if op == 0x41:
            stack.append(('i32', reader.sint() & 0xffffffff))
        elif op == 0x42:
            start = reader.pos
            constant(reader, op)
            value = 0
            encoded = reader.data[start:reader.pos]
            for i, byte in enumerate(encoded): value |= (byte & 127) << (7*i)
            if encoded[-1] & 64: value -= 1 << (7*len(encoded))
            stack.append(('i64', value & 0xffffffffffffffff))
        elif op in (0x43, 0x44):
            stack.append(('f32' if op == 0x43 else 'f64',
                          int.from_bytes(reader.take(4 if op == 0x43 else 8), 'little')))
        elif op == 0x23:
            index=reader.uint()
            require(index<len(imported_globals) and not imported_globals[index]['mutable'],
                    'constant global.get requires an imported immutable global')
            g=imported_globals[index];stack.append((g['type'],g['bits']))
        elif op in (0x6a,0x6b,0x6c,0x7c,0x7d,0x7e):
            kind = 'i32' if op <= 0x6c else 'i64'
            require(len(stack)>=2 and stack[-1][0]==kind and stack[-2][0]==kind,
                    'invalid extended constant operand types')
            b,a = stack.pop()[1],stack.pop()[1]
            value = a+b if op in (0x6a,0x7c) else a-b if op in (0x6b,0x7d) else a*b
            stack.append((kind, value & ((1 << (32 if kind=='i32' else 64))-1)))
        else:
            raise Rejected('unsupported constant initializer opcode')
    require(len(stack)==1 and stack[0][0]==expected, 'invalid constant initializer result')
    return stack[0][1]


def inspect_body(reader, data_indices=None, indirects=None, type_count=0, element_indices=None, reference_indices=None):
    body = Reader(reader.take(reader.uint()))
    local_groups = body.uint()
    require(local_groups <= 256, "at most 256 local declaration groups supported")
    locals_count = 0
    for _ in range(local_groups):
        locals_count += body.uint()
        local_type = body.byte()
        require(local_type in (*VALUE_TYPES, 0x70, 0x6f), 'only scalar/funcref locals supported')
    require(locals_count <= 256, "at most 256 locals supported")
    nesting = [("function", False)]
    operations = set()
    calls = set()
    instructions = 0
    while nesting:
        instructions += 1
        require(instructions <= 65536, "at most 65536 instructions supported")
        opcode = body.byte()
        if opcode in NUMERIC_OPS:
            operations.add(NUMERIC_OPS[opcode])
        elif opcode in SIMPLE_OPS:
            operations.add(SIMPLE_OPS[opcode])
        elif opcode in INDEX_OPS:
            operations.add(INDEX_OPS[opcode])
            body.uint()
        elif opcode == 0x10:
            operations.add("call")
            calls.add(body.uint())
        elif opcode in (0x25, 0x26):
            require(body.uint() == 0, 'table get/set must target table zero')
            operations.add('table.get' if opcode == 0x25 else 'table.set')
        elif opcode == 0xd0:
            require(body.byte() in (0x70,0x6f), 'only funcref/externref null supported')
            operations.add('ref.null')
        elif opcode == 0xd1:
            operations.add('ref.is_null')
        elif opcode == 0xd2:
            index = body.uint()
            if reference_indices is not None: reference_indices.append(index)
            operations.add('ref.func')
        elif opcode == 0x1c:
            require(body.uint() == 1 and body.byte() in (*VALUE_TYPES, 0x70, 0x6f),
                    'only scalar/funcref typed select supported')
            operations.add('select_typed')
        elif opcode == 0x11:
            index, table = body.uint(), body.uint()
            require(index < type_count and table == 0, 'invalid indirect call type/table')
            if indirects is not None: indirects.append(index)
            operations.add('call_indirect')
        elif opcode in (0x02, 0x03, 0x04, 0x06):
            kind = {0x02: "block", 0x03: "loop", 0x04: "if", 0x06: "try"}[opcode]
            operations.add(kind)
            block_type = body.byte()
            if block_type not in (0x40, 0x70, 0x6f, *VALUE_TYPES):
                body.pos -= 1
                require(0 <= body.sint() < type_count, 'invalid block type index')
            nesting.append((kind, False))
            require(len(nesting) <= 65, "at most 64 nested control constructs supported")
        elif opcode in (0x07,0x19):
            require(nesting[-1][0]=='try' and nesting[-1][1]!='all', 'unexpected catch instruction')
            if opcode==0x07: body.uint()
            nesting[-1]=('try','all' if opcode==0x19 else True)
            operations.add('catch' if opcode==0x07 else 'catch_all')
        elif opcode==0x08:
            body.uint(); operations.add('throw')
        elif opcode == 0x05:
            require(nesting[-1] == ("if", False), "unexpected else instruction")
            nesting[-1] = ("if", True)
            operations.add("else")
        elif opcode == 0x0b:
            nesting.pop()
            operations.add("end")
        elif opcode == 0x0e:
            operations.add("br_table")
            count = body.uint()
            require(count <= 256, "at most 256 br_table entries supported")
            for _ in range(count + 1):
                body.uint()
        elif opcode in (0x42, 0x43, 0x44):
            operations.add({0x42:'i64.const', 0x43:'f32.const', 0x44:'f64.const'}[opcode])
            constant(body, opcode)
        elif opcode == 0xfc:
            sub = body.uint()
            if sub <= 7:
                operations.add('trunc_sat_' + str(sub))
            elif sub in (8, 9):
                index = body.uint()
                if data_indices is not None:
                    data_indices.append(index)
                if sub == 8:
                    require(body.uint() == 0, 'memory.init must target memory zero')
                operations.add('memory.init' if sub == 8 else 'data.drop')
            elif sub in (12, 13):
                index = body.uint()
                if element_indices is not None: element_indices.append(index)
                if sub == 12:
                    require(body.uint() == 0, 'table.init must target table zero')
                operations.add('table.init' if sub == 12 else 'elem.drop')
            elif sub in (14, 15, 16, 17):
                require(body.uint() == 0, 'table operation must target table zero')
                if sub == 14:
                    require(body.uint() == 0, 'table.copy must target table zero')
                operations.add({14:'table.copy',15:'table.grow',16:'table.size',17:'table.fill'}[sub])
            elif sub == 10:
                require(body.uint() == 0 and body.uint() == 0,
                        'memory.copy must target source and destination memory zero')
                operations.add('memory.copy')
            elif sub == 11:
                require(body.uint() == 0, 'memory.fill must target memory zero')
                operations.add('memory.fill')
            else:
                raise Rejected('unsupported operation in prefix 0xfc')
        elif opcode in MEMORY_OPS:
            name, alignment = MEMORY_OPS[opcode]
            require(body.uint() <= alignment, 'invalid scalar memory alignment')
            body.uint()
            operations.add(name)
        elif opcode in (0x3f, 0x40):
            operation = 'memory.size' if opcode == 0x3f else 'memory.grow'
            require(body.uint() == 0, operation + ' must target memory zero')
            operations.add(operation)
        elif opcode == 0x41:
            operations.add("i32.const")
            body.sint()
        else:
            raise Rejected(f"unsupported opcode 0x{opcode:02x}")
    body.done()
    return sorted(operations), sorted(calls)


def inspect_code(reader, function_count, import_count=0, data_indices=None, indirect_calls=None, type_count=0, element_indices=None, reference_indices=None):
    require(reader.uint() == function_count,
            "function/code body counts differ")
    operations = set()
    calls = []
    for _ in range(function_count):
        indirects = []
        body_operations, targets = inspect_body(reader, data_indices, indirects, type_count, element_indices, reference_indices)
        if indirect_calls is not None: indirect_calls.append(indirects)
        require(all(target < import_count + function_count for target in targets),
                "invalid direct call function index")
        operations.update(body_operations)
        calls.append(targets)
    return sorted(operations), calls


def max_call_depth(calls, import_count=0):
    """Return an acyclic depth, or None when dynamic depth accounting is needed."""
    depths = {}
    active = set()

    def visit(index):
        if index < import_count:
            return 0
        if index in active:
            raise RecursionError
        if index in depths:
            return depths[index]
        active.add(index)
        depth = 1 + max((visit(target) for target in calls[index - import_count]), default=0)
        active.remove(index)
        depths[index] = depth
        return depth

    try:
        return max((visit(index + import_count) for index in range(len(calls))), default=0)
    except RecursionError:
        return None


def inspect_wasm(data, bindings=None):
    """Feature inspection. wasm-validate separately checks Wasm typing/indices."""
    require(len(data) <= MAX_MODULE_BYTES, "module exceeds 1 MiB admission limit")
    reader = Reader(data)
    require(reader.take(8) == b"\0asm\x01\0\0\0", "expected Wasm binary version 1")
    seen = set()
    last = 0
    info = {"sha256": hashlib.sha256(data).hexdigest(),
            "memory_count": 0, "memory_bytes": 0, "memory_capacity_bytes": 0,
            "memory_max_pages": 0, "imports": [], "start_function": None,
            "memory_maximum_declared": False, "global_count": 0,
            "data_segments": [], "global_exports": [], "exports": [], "export_name": "",
            "argument_count": 0, "direct_calls": [],
            "data_count": None, "exception_tags": [], "tables": [], "elements": [], "indirect_calls": []}
    data_indices, element_indices, reference_indices = [], [], []
    types, functions, imported_types = [], [], []
    from pimwasm_imports import load as load_bindings,resolve
    bindings=bindings or load_bindings(None)
    imported_globals=[]
    info['function_imports']=[]
    info['defined_global_types']=[]
    while reader.pos < len(reader.data):
        section_id = reader.byte()
        section = Reader(reader.take(reader.uint()))
        if section_id == 0:
            section.name()
            continue  # Custom sections do not change core Wasm execution.
        require(section_id in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13),
                f"unsupported section {section_id}")
        # DataCount (12) precedes Code (10) and Data (11) in Wasm order.
        order = {13:5.5, 12: 10, 10: 11, 11: 12}.get(section_id, section_id)
        require(section_id not in seen and order > last,
                "duplicate or unordered Wasm section")
        seen.add(section_id)
        last = order
        if section_id == 1:
            count = section.uint()
            require(count <= MAX_FUNCTION_TYPES, "at most 256 function types supported")
            for _ in range(count):
                require(section.byte() == 0x60, "only function types supported")
                argc = section.uint()
                require(argc <= 32, "at most 32 function parameters supported")
                params = [function_type(section) for _ in range(argc)]
                result_count = section.uint()
                require(result_count <= MAX_RESULTS, 'at most four scalar results supported')
                result = [function_type(section) for _ in range(result_count)]
                require(len(result)<=1 or 'externref' not in result, 'multiple reference results unsupported')
                types.append((params, result))
        elif section_id == 2:
            count = section.uint()
            require(count <= 32, 'at most 32 import entries supported')
            for ordinal in range(count):
                module, name, kind = section.name(), section.name(), section.byte()
                require(kind in (0,1,2,3),'unsupported import kind')
                record={'module':module,'name':name,'ordinal':ordinal,'kind':('function','table','memory','global')[kind]}
                try:
                    if kind==0:
                        index=len(imported_types);type_index=section.uint()
                        require(type_index<len(types),'invalid import function type index')
                        params,result=types[type_index]
                        require(len(result)<=1,'native imports support MVP void/single results')
                        binding=resolve(bindings,module,name,'function',params,result)
                        record.update(index=index,parameter_types=params,result_type=result[0] if result else 'void',result_types=result)
                        imported_types.append(type_index);info['function_imports'].append(record)
                    elif kind==3:
                        typ=scalar_type(section,'imported globals');mutable=section.byte()
                        require(mutable in (0,1),'invalid imported global mutability')
                        binding=resolve(bindings,module,name,'global')
                        require(binding.get('type')==typ and binding.get('mutable')==bool(mutable),'incompatible global binding')
                        bits=binding.get('bits');bits=int(bits,0) if isinstance(bits,str) else bits
                        require(type(bits)is int and 0<=bits<1<<(32 if typ in ('i32','f32') else 64),'invalid imported global bits')
                        record.update(index=len(imported_globals),type=typ,mutable=mutable,bits=bits)
                        imported_globals.append(record)
                        info.setdefault('global_types',[]).append(typ);info.setdefault('global_mutabilities',[]).append(mutable)
                        info.setdefault('global_initial_bits',[]).append(bits);info['global_count']+=1
                    else:
                        if kind==1:require(section.byte()==0x70,'only imported funcref tables supported')
                        flags=section.uint();require(flags in (0,1),'only MVP import limits supported')
                        minimum=section.uint();maximum=section.uint() if flags else (65536 if kind==2 else 0xffffffff)
                        binding=resolve(bindings,module,name,record['kind'])
                        initial,actual_max=binding.get('initial'),binding.get('maximum')
                        require(type(initial)is int and type(actual_max)is int and minimum<=initial<=actual_max<=maximum,'incompatible import limits')
                        bound=MAX_INITIAL_MEMORY_PAGES if kind==2 else MAX_TABLE_ENTRIES
                        require(initial<=bound,'import object exceeds DPU initial capacity')
                        record.update(index=0,minimum=minimum,maximum=maximum)
                        if kind==2:
                            require(not info['memory_count'],'at most one memory supported')
                            info.update(memory_count=1,memory_bytes=initial*WASM_PAGE_BYTES,memory_max_pages=actual_max,memory_maximum_declared=True,memory_import=ordinal)
                        else:
                            require(not info['tables'],'at most one table supported')
                            info['tables'].append({'initial':initial,'maximum':actual_max,'imported':True});info['table_import']=ordinal
                    record['binding']=binding;info['imports'].append(record)
                except ValueError as error:
                    raise Rejected('unsupported import binding: '+str(error)) from error
        elif section_id == 3:
            count = section.uint()
            require(count <= MAX_FUNCTIONS, "at most 256 defined functions supported")
            for _ in range(count):
                type_index = section.uint()
                require(type_index < len(types),
                        "invalid function type index")
                functions.append(type_index)
        elif section_id == 4:
            require(not info['tables'],'at most one table supported')
            require(section.uint() == 1, 'exactly one bounded funcref table supported')
            table_kind=section.byte()
            require(table_kind in (0x70,0x6f), 'only funcref/externref tables supported')
            flags = section.uint()
            require(flags in (0,1), 'table64/shared tables unsupported')
            initial = section.uint()
            maximum = section.uint() if flags else 0xffffffff
            require(initial <= MAX_TABLE_ENTRIES and initial <= maximum,
                    'table exceeds 64-entry bound')
            info['tables'].append(dict(initial=initial, maximum=maximum, **({'kind':'externref'} if table_kind==0x6f else {})))
        elif section_id == 13:
            count=section.uint();require(count<=16,'at most 16 exception tags supported')
            for _ in range(count):
                require(section.byte()==0,'invalid exception tag attribute')
                index=section.uint();require(index<len(types),'invalid exception tag type')
                params,results=types[index]
                require(not results and all(t in C_TYPES and t!='externref' for t in params),'exception tags require scalar parameters and no results')
                require(sum(8 if t in ('i64','f64') else 4 for t in params)<=256,'exception payload exceeds 256 bytes')
                info['exception_tags'].append(index)
        elif section_id == 5:
            count = section.uint()
            require(count <= 1, "at most one memory supported")
            if count:
                require(not info['memory_count'],'at most one memory supported')
                flags = section.uint()
                require(flags in (0, 1), "shared/memory64 memory is excluded")
                minimum = section.uint()
                maximum = section.uint() if flags else 65536
                require(0 <= minimum <= MAX_INITIAL_MEMORY_PAGES,
                        f"memory initial size must be 0..{MAX_INITIAL_MEMORY_PAGES} pages")
                require(minimum <= maximum <= 65536,
                        "memory maximum must be between initial and 65536")
                info.update(memory_count=1, memory_bytes=minimum * WASM_PAGE_BYTES,
                            memory_max_pages=maximum, memory_maximum_declared=bool(flags))
        elif section_id == 6:
            count = section.uint()
            require(count+len(imported_globals) <= 16, "at most 16 globals supported")
            info["global_count"] += count
            for _ in range(count):
                kind = scalar_type(section, 'globals')
                info.setdefault('global_types', []).append(kind)
                info['defined_global_types'].append(kind)
                mutable = section.byte()
                require(mutable in (0, 1), "invalid global mutability")
                info.setdefault('global_mutabilities', []).append(mutable)
                value = const_expression(section, kind,imported_globals)
                info.setdefault('global_initial_bits', []).append(value)
        elif section_id == 7:
            count = section.uint()
            require(count <= 64,"at most 64 total exports supported")
            exports = []
            globals_ = []
            memories = []
            names = set()
            for _ in range(count):
                name, kind, index = section.name(), section.byte(), section.uint()
                require(len(name.encode("utf-8")) <= MAX_EXPORT_NAME_BYTES,
                        "export names must be at most 63 UTF-8 bytes")
                require(name not in names, "duplicate export name")
                names.add(name)
                require(kind in (0,1,2,3),'invalid export kind')
                if kind == 0:
                    all_functions = imported_types + functions
                    require(index < len(all_functions), "invalid function export index")
                    params, result = types[all_functions[index]]
                    require(all(t!='externref' for t in params+result),'reference-valued host signatures unsupported')
                    exports.append({"name": name, "argument_count": len(params), "index": index,
                                    "parameter_types": params, "result_type": result[0] if result else "void",
                                    "result_types": result, "result_count": len(result)})
                elif kind == 3:
                    require(index < info['global_count'], "invalid global export index")
                    globals_.append({'name': name, 'index': index,
                                     'type': info['global_types'][index],
                                     'mutable': info['global_mutabilities'][index]})
                elif kind==1:
                    require(index==0 and info['tables'],'invalid table export index')
                    info.setdefault('table_exports',[]).append(name)
                else:
                    require(index == 0 and info["memory_count"] == 1, "invalid memory export index")
                    memories.append(name)
            require(len(exports) <= 32, "at most thirty-two function exports supported")
            require(len(memories) <= 16 and len(info.get('table_exports',[]))<=16,
                    "at most sixteen exports of each kind supported")
            require(len(globals_) <= 16, "at most sixteen global exports supported")
            info["exports"] = exports
            info["global_exports"] = globals_
            info['memory_exports']=memories
            info["export_name"] = exports[0]["name"] if exports else ''
            info["argument_count"] = exports[0]["argument_count"] if exports else 0
        elif section_id == 8:
            index = section.uint()
            all_functions = imported_types + functions
            require(index < len(all_functions), 'invalid start function index')
            require(types[all_functions[index]] == ([], []),
                    'start function must have signature () -> void')
            info['start_function'] = index
        elif section_id == 9:
            for _ in range(section.uint()):
                flags = section.uint()
                require(flags in (0,1,2,3), 'only function-index element segments supported')
                if flags == 2: require(section.uint()==0, 'elements must target table zero')
                require(info['tables'], 'element segment requires table zero')
                require(info['tables'][0].get('kind','funcref')=='funcref','externref element segments unsupported')
                offset = const_expression(section, 'i32',imported_globals) if flags in (0,2) else None
                if flags: require(section.byte()==0, 'invalid element kind')
                values = [section.uint() for _ in range(section.uint())]
                require(all(i < len(imported_types)+len(functions) for i in values),
                        'invalid element function index')
                info['elements'].append({'offset':offset, 'functions':values, 'passive': flags == 1, 'declarative': flags == 3})
        elif section_id == 12:
            info['data_count'] = section.uint()
        elif section_id == 10:
            info["operations"], info["direct_calls"] = inspect_code(section, len(functions), len(imported_types), data_indices, info['indirect_calls'], len(types), element_indices, reference_indices)
            require(info['memory_count'] == 1 or
                    not {'memory.init', 'memory.copy', 'memory.fill', 'memory.size', 'memory.grow'}.intersection(info['operations']),
                    'memory.init, memory.copy, memory.fill, memory.size and memory.grow require memory zero')
        elif section_id == 11:
            count = section.uint()
            for _ in range(count):
                flags = section.uint()
                require(flags in (0, 1, 2), "unsupported data segment encoding")
                offset = None
                if flags != 1:
                    require(info["memory_count"] == 1, "active data requires memory zero")
                    if flags == 2:
                        require(section.uint() == 0, "active data must target memory zero")
                    offset = const_expression(section, 'i32',imported_globals)
                length = section.uint()
                segment = section.take(length)
                info["data_segments"].append({"offset": offset, "length": length, "passive": flags == 1,
                                              "sha256": hashlib.sha256(segment).hexdigest()})
        section.done()
    required = {1, 3, 10} if functions else set()
    require(info['data_count'] is None or info['data_count'] == len(info['data_segments']),
            'DataCount does not match data segment count')
    require(not data_indices or info['data_count'] is not None,
            'memory.init/data.drop require DataCount section')
    require(all(index < len(info['data_segments']) for index in data_indices),
            'invalid data segment index')
    require(all(i < len(info['elements']) for i in element_indices), 'invalid element segment index')
    require(info['tables'] or not any(op.startswith('table.') for op in info.get('operations', [])),
            'table operations require table zero')
    require(not info['tables'] or info['tables'][0].get('kind','funcref')!='externref' or 'call_indirect' not in info.get('operations',[]),'indirect calls require a funcref table')
    if info['exception_tags'] or set(info.get('operations',[])) & {'try','catch','catch_all','throw'}:
        require(not info['imports'] and 'call_indirect' not in info.get('operations',[]),'exceptions currently require defined tags and direct guest calls without imports')
    info['data_segment_bytes'] = sum(segment['length'] for segment in info['data_segments'])
    require(required <= seen, "missing required type/function/memory/export/code section")
    info["function_count"] = len(functions)
    require(info['global_count']<=16,'at most 16 globals supported')
    info['import_count'] = len(imported_types)
    info['types'] = [{'parameters': p, 'results': r} for p,r in types]
    all_functions = imported_types + functions
    info['function_types'] = all_functions
    if 'table_import' in info:
        binding=info['imports'][info['table_import']]['binding']
        entries=binding.get('entries',[])
        require(isinstance(entries,list) and len(entries)<=binding['initial'],'invalid imported table entries')
        for entry in entries:
            if entry is None or type(entry)is int and 0<=entry<len(imported_types):continue
            require(isinstance(entry,dict),'imported table entries must name function imports, native functions or null')
            require(re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*',entry.get('symbol','')) is not None,'invalid imported table native symbol')
            signature={'parameters':entry.get('parameters'),'results':entry.get('results')}
            require(signature in info['types'] and len(signature['results'])<=1,'native table entry requires an existing MVP function type')
            entry['type_index']=info['types'].index(signature)
        info['table_binding_entries']=entries
    possible = [set(c) for c in info['direct_calls']]
    require(all(i < len(all_functions) for i in reference_indices), 'invalid ref.func index')
    info['referenced_functions'] = sorted(set(reference_indices))
    table_targets = set(i for e in info['elements'] for i in e['functions']) | set(reference_indices)
    table_targets.update(i for i in info.get('table_binding_entries',[]) if type(i)is int)
    for caller, signatures in enumerate(info['indirect_calls']):
        require(not signatures or info['tables'], 'indirect call requires table zero')
        for signature in signatures:
            possible[caller].update(i for i in table_targets if types[all_functions[i]]==types[signature])
    info['possible_calls'] = [sorted(c) for c in possible]
    info["max_call_depth"] = max_call_depth(info['possible_calls'], len(imported_types))
    info['recursive_calls'] = info['max_call_depth'] is None
    info['call_depth_limit'] = RECURSIVE_CALL_DEPTH if info['recursive_calls'] else 256
    info.setdefault("operations", [])
    info['memory_capacity_bytes'] = (
        min(info['memory_max_pages'], MAX_INITIAL_MEMORY_PAGES) * WASM_PAGE_BYTES
        if 'memory.grow' in info['operations'] or 'memory_import' in info else info['memory_bytes'])
    return info


def command(args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, f"{args[0]} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def global_accessors(header, info):
    """Validate the pinned generator's scalar-global accessors, including aliases."""
    generated = re.findall(
        r"/\* export: '([^']+)' \*/\n(u32|u64|f32|f64)\* (w2c_\w+)\(([^;]+)\);", header)
    accepted = info.get('global_exports', [])
    require(len(generated) == len(accepted), "unexpected wasm2c global export schema")
    for declaration, export in zip(generated, accepted):
        require(declaration[0] == export['name'], "unexpected wasm2c global export order/name")
        require(declaration[1] == C_TYPES[export['type']] and
                declaration[3] == MODULE_TYPE + '* instance',
                "unexpected wasm2c global export signature")
    return [declaration[2] for declaration in generated]


# Pinned wasm2c emits floating binary arithmetic as compound assignments to
# typed stack temporaries. Do not match integer arithmetic or unary sign ops.
FLOAT_ARITHMETIC = re.compile(r'^(?P<indent> +)(?P<lhs>var_(?P<kind>[fd])[0-9]+) (?P<op>[+*/-])= var_(?P=kind)[0-9]+;$')
NAN_GUARD_MARKER = ' /* pimwasm: quiet arithmetic NaN */'


def quiet_arithmetic(module):
    """Retain each generated operation and quiet only its arithmetic result.

    The DPU compiler ignores signaling-NaN flags and can fold x*1 to x. A
    subsequent bit-based NaN check retains Wasm's quiet-result requirement even
    after folding. Neg/abs/copysign, transport and reinterpretation stay intact.
    """
    require(NAN_GUARD_MARKER not in module, 'unexpected preexisting NaN guard')
    output = []
    for line in module.splitlines(keepends=True):
        match = FLOAT_ARITHMETIC.fullmatch(line.rstrip('\n'))
        # Refuse changed compound schemas and expanded binary assignments.
        # Integer address arithmetic inside load/store calls is unaffected.
        suspect = re.search(r'var_[fd][0-9]+\s*[+*/-]=', line)
        expanded = re.search(r'var_[fd][0-9]+\s*=\s*var_[fd][0-9]+\s*[+*/-]', line)
        require(not (suspect or expanded) or match is not None,
                'unexpected wasm2c floating arithmetic schema')
        output.append(line)
        if match:
            helper = 'wasm_quietf' if match['kind'] == 'f' else 'wasm_quiet'
            output.append(f"{match['indent']}{match['lhs']} = {helper}({match['lhs']});{NAN_GUARD_MARKER}\n")
    return ''.join(output)


def adapt_source(source, header, info):
    """Validate pinned schemas, retain computation, add arithmetic NaN guards."""
    if not info['function_count']:
        # With no function declarations the unchanged suffix starts at the
        # first type definition (if any), data bytes or initialization code.
        marker_pattern = r"^(?:FUNC_TYPE_T\(w2c_pimwasm__module_\w+\)|static const u8 data_segment_data_w2c_pimwasm__module_d[0-9]+\[\]|/\* export: |static void init_(?:globals|memories|tables|instance_import)\(w2c_pimwasm__module\*|void wasm2c_pimwasm__module_instantiate\(w2c_pimwasm__module\*)"
    else:
        marker_pattern = r"^static (?:void|u32|u64|f32|f64|wasm_rt_externref_t|struct wasm_multi_[ijfd]+) w2c_pimwasm__module_\w+\("
    marker = re.search(marker_pattern, source, re.MULTILINE)
    require(marker is not None, "unexpected wasm2c module declaration schema")
    prelude, module = source[:marker.start()], source[marker.start():]
    # wasm2c emits internal multi-result structs just before declarations.
    multi_pattern = r'#ifndef wasm_multi_([ijfd]+)\n#define wasm_multi_\1 wasm_multi_\1\nstruct wasm_multi_\1 \{\n(.*?)\};\n#endif  /\* wasm_multi_\1 \*/\n'
    inverse = {v:k for k,v in TYPE_LETTERS.items()}
    for letters, fields in re.findall(multi_pattern, header + prelude, re.S):
        require(2 <= len(letters) <= 32 and (len(letters)<=MAX_RESULTS or any(letters==''.join(TYPE_LETTERS[t] for t in info['types'][i]['parameters']) for i in info['exception_tags'])) and fields == ''.join(
            f'  {C_TYPES[inverse[t]]} {t}{i};\n' for i,t in enumerate(letters)),
            'unexpected wasm2c multi-result schema')
    extra_types = ''.join(m.group(0) for m in re.finditer(multi_pattern, prelude, re.S))
    # Each generated definition is followed by a blank line.
    prelude = re.sub(multi_pattern + r'\n?', '', prelude, flags=re.S)
    tag_typedef='typedef char wasm_tag_placeholder_t;\n\n'
    if info['exception_tags']:
        require(prelude.endswith(tag_typedef),'unexpected generated exception tag schema')
        prelude=prelude[:-len(tag_typedef)]
        extra_types+=tag_typedef
    expected_prelude = PRELUDE_SHA256 if info["memory_count"] else NOMEM_PRELUDE_SHA256
    require(hashlib.sha256(prelude.encode()).hexdigest() == expected_prelude,
            "unexpected wasm2c generic prelude; generator schema requires review")
    structure = re.search(r"typedef struct w2c_pimwasm__module \{\n(.*?)\} w2c_pimwasm__module;", header, re.S)
    require(structure is not None, "unexpected wasm2c instance schema")
    members = [line for line in structure.group(1).splitlines() if not line.strip().startswith('/* import:')]
    from pimwasm_imports import validate_header
    validate_header(header, info)
    contexts=sorted(set(i['c_module'] for i in info['function_imports']))
    expected_contexts=['  struct w2c_'+m+'* w2c_'+m+'_instance;' for m in contexts]
    require([m for m in members if m.startswith('  struct w2c_')]==expected_contexts,'unexpected import contexts')
    members=[m for m in members if m not in expected_contexts]
    expected_objects=[]
    for b in sorted(info['imports'],key=lambda b:(b['c_module'],b['c_name'])):
        if b['kind']=='function':continue
        typ={'global':C_TYPES.get(b.get('type'),'void'),'memory':'wasm_rt_memory_t','table':'wasm_rt_funcref_table_t'}[b['kind']]
        expected_objects.append('  '+typ+' *w2c_'+b['c_module']+'_'+b['c_name']+';')
    require([m for m in members if ' *w2c_' in m]==expected_objects,'unexpected imported object fields')
    members=[m for m in members if m not in expected_objects]
    # Pinned wasm2c emits one drop bit for each nonempty passive segment.
    # Empty/active segments always have effective length zero for memory.init.
    drop_fields = [f'  bool data_segment_dropped_w2c_pimwasm__module_d{i} : 1;'
                   for i, segment in enumerate(info['data_segments'])
                   if segment['passive'] and segment['length']]
    drop_fields += [f'  bool elem_segment_dropped_w2c_pimwasm__module_e{i} : 1;'
                    for i, segment in enumerate(info['elements'])
                    if segment['passive'] and segment['functions']]
    require([line for line in members if line.startswith('  bool ')] == drop_fields,
            'unexpected wasm2c data segment state schema')
    members = [line for line in members if line not in drop_fields]
    table_fields = [line for line in members if re.search(r'wasm_rt_(?:funcref|externref)_table_t',line)]
    require(len(table_fields)==int(bool(info['tables']) and 'table_import' not in info) and
            all(re.fullmatch(r'  wasm_rt_'+info['tables'][0].get('kind','funcref')+r'_table_t w2c_\w+;',field) for field in table_fields),
            'unexpected wasm2c table schema')
    members = [line for line in members if line not in table_fields]
    if not info["memory_count"] and not info["global_count"] and not info['imports'] and not info['tables']:
        require(members == ["  char dummy_member;"], "unexpected wasm2c empty instance schema")
    else:
        require(all(re.fullmatch(r"  (?:u32|u64|f32|f64|wasm_rt_memory_t) w2c_\w+;", line) for line in members),
                "unexpected wasm2c instance field")
        require([line.strip().split()[0] for line in members if not line.startswith("  wasm_rt_memory_t ")] ==
                [C_TYPES[kind] for kind in info.get("defined_global_types", [])],
                "unexpected wasm2c global schema")
    memories = re.findall(r"  wasm_rt_memory_t (w2c_\w+);", structure.group(1))
    require(len(memories) == info["memory_count"]-int('memory_import' in info), "unexpected wasm2c memory schema")
    exports = re.findall(r"/\* export: '([^']+)' \*/\n(void|u32|u64|f32|f64|struct wasm_multi_[ijfd]+) (w2c_\w+)\(([^;]+)\);", header)
    require(len(exports) == len(info["exports"]),
            "unexpected wasm2c function export schema")
    for generated, accepted in zip(exports, info["exports"]):
        require(generated[0] == accepted["name"], "unexpected wasm2c function export order/name")
        expected_signature = MODULE_TYPE + "*" + "".join(", " + C_TYPES[t] for t in accepted["parameter_types"])
        require(generated[1] == c_result(accepted["result_types"]) and generated[3] == expected_signature, "unexpected wasm2c function signature")
    global_accessors(header, info)
    for kind,typ in [('table','wasm_rt_'+(info['tables'][0].get('kind','funcref') if info['tables'] else 'funcref')+'_table_t'),('memory','wasm_rt_memory_t')]:
        accessors=re.findall(r"/\* export: '([^']+)' \*/\n"+typ+r"\* w2c_\w+\(w2c_pimwasm__module\* instance\);",header)
        require(accessors==info.get(kind+'_exports',[]),'unexpected '+kind+' export schema')
    checks = prelude[prelude.index("#define TRAP(x)"):prelude.index("static inline bool func_types_eq")]
    types = prelude[prelude.index("#if defined(__GNUC__) || defined(__clang__)"):
                    prelude.index("#if (__STDC_VERSION__ < 201112L)")]
    # Preserve the pinned WABT numeric checks/helpers; replace only missing
    # platform math functions with real IEEE bit algorithms, not stubs.
    numeric = '#include "wasm-rt-numeric.h"\n' + prelude[
        prelude.index('#define DIV_S'):prelude.index('static inline void memory_fill')]
    indirect_support = ''
    prototypes = ''
    wrapper_prototypes = ''
    dispatchers = ''
    binding_wrappers=''
    if info['tables']:
        # Keep WABT's type/null/index checks. Only the native call mechanism
        # changes: closed, typed direct-call branches instead of a function
        # pointer jump. This retains the conservative native call-graph proof.
        indirect_support = prelude[prelude.index('static inline bool func_types_eq'):
                                   prelude.index('#if WASM_RT_USE_SEGUE_FOR_THIS_MODULE && WASM_RT_SANITY_CHECKS')]
        indirect_support += prelude[prelude.index('typedef struct {\n  enum { RefFunc'):
                                    prelude.index('// Currently wasm2c only supports')]
        table_helpers = prelude[prelude.index('#define DEFINE_TABLE_COPY'):
                                prelude.index('#if defined(__GNUC__) || defined(__clang__)')]
        omitted='funcref' if info['tables'][0].get('kind')=='externref' else 'externref'
        table_helpers = re.sub(r'^DEFINE_TABLE_\w+\('+omitted+r'\)\n', '', table_helpers, flags=re.M)
        # Avoid NULL + 0 pointer arithmetic for zero-capacity tables. Keep
        # both complete bounds checks even when the requested length is zero.
        copy_marker = '    memmove(dest->data + dest_addr, src->data + src_addr,'
        require(table_helpers.count(copy_marker) == 1, 'unexpected table.copy helper schema')
        table_helpers = table_helpers.replace(copy_marker,
            '    if (!n) return;                                                        ' + chr(92) + '\n' + copy_marker)
        indirect_support += table_helpers
        indirect_support += ('#undef CALL_INDIRECT\n'
            '#define CALL_INDIRECT(table,t,ft,x,...) (CHECK_CALL_INDIRECT(table,ft,x), '
            'pimwasm_indirect_##ft(table.data[x].func,__VA_ARGS__))\n')
        entries = re.findall(r'\{RefFunc, (w2c_pimwasm__module_t[0-9]+), \(wasm_rt_function_ptr_t\)(wrap_\w+), \{NULL\}, (0|offsetof\(w2c_pimwasm__module, w2c_\w+_instance\))\}', module)
        require(len(entries) == sum(len(e['functions']) for e in info['elements']),
                'unexpected wasm2c element schema')
        wrappers = {}
        for index in sorted({i for i in info.get('table_binding_entries',[]) if type(i)is int}):
            imported=info['function_imports'][index];sig=info['types'][info['function_types'][index]]
            wrapper=f'pimwasm_native_table_{index}'
            signature='static '+c_result(sig['results'])+' '+wrapper+'(void *instance'+''.join(f', {C_TYPES[t]} var_{i}' for i,t in enumerate(sig['parameters']))+')'
            wrapper_prototypes+=signature+';\n'
            target='w2c_'+imported['c_module']+'_'+imported['c_name']
            call=target+'(instance'+''.join(f', var_{i}' for i in range(len(sig['parameters'])))+')'
            binding_wrappers+=signature+' { '+('return ' if sig['results'] else '')+call+'; }\n'
            wrappers[wrapper]=sig
        for slot,entry in enumerate(info.get('table_binding_entries',[])):
            if not isinstance(entry,dict):continue
            sig=info['types'][entry['type_index']];wrapper=f'pimwasm_external_table_{slot}'
            target=entry['symbol']
            wrapper_prototypes+=c_result(sig['results'])+' '+target+'('+(', '.join(C_TYPES[t] for t in sig['parameters']) or 'void')+');\n'
            signature='static '+c_result(sig['results'])+' '+wrapper+'(void *instance'+''.join(f', {C_TYPES[t]} a{i}' for i,t in enumerate(sig['parameters']))+')'
            wrapper_prototypes+=signature+';\n'
            call=target+'('+', '.join(f'a{i}' for i in range(len(sig['parameters'])))+')'
            binding_wrappers+=signature+' { (void)instance; '+('return ' if sig['results'] else '')+call+'; }\n'
            wrappers[wrapper]=sig
        target_records = list(zip(entries, [i for e in info['elements'] for i in e['functions']]))
        # ref.func may name an exported function without an element segment.
        # Derive its symbol from the pinned declaration order, not its name.
        declarations = re.findall(r'^static (?:void|u32|u64|f32|f64|wasm_rt_externref_t|struct wasm_multi_[ijfd]+) (w2c_pimwasm__module_\w+)\(w2c_pimwasm__module\*[^;]*\);$', module, re.M)
        require(len(declarations) == info['function_count'], 'unexpected function declaration order/schema')
        for index in info['referenced_functions']:
            imported=info['function_imports'][index] if index<info['import_count'] else None
            symbol = ('w2c_'+imported['c_module']+'_'+imported['c_name'] if imported
                      else declarations[index-info['import_count']])
            type_symbol = f'w2c_pimwasm__module_t{info["function_types"][index]}'
            offset = 'offsetof(w2c_pimwasm__module, w2c_'+imported['c_module']+'_instance)' if imported else '0'
            target_records.append(((type_symbol, 'wrap_' + symbol, offset), index))
        for (type_symbol, wrapper, offset), index in target_records:
            type_index = info['function_types'][index]
            imported=info['function_imports'][index] if index<info['import_count'] else None
            require(offset == ('offsetof(w2c_pimwasm__module, w2c_'+imported['c_module']+'_instance)' if imported else '0'), 'unexpected element instance offset')
            require(type_symbol == f'w2c_pimwasm__module_t{type_index}', 'unexpected element function type')
            sig = info['types'][type_index]
            # Wrapper signatures are checked, including those for selected imports.
            signature = 'static ' + c_result(sig['results']) + ' ' + wrapper + '(void *instance'
            signature += ''.join(f', {C_TYPES[t]} var_{i}' for i,t in enumerate(sig['parameters'])) + ') {'
            require(signature in module, 'unexpected wasm2c indirect wrapper schema')
            wrappers[wrapper] = sig
            wrapper_prototypes += signature[:-2] + ';\n'
        for i,sig in enumerate(info['types']):
            ret = c_result(sig['results'])
            decl = ret + f' pimwasm_indirect_w2c_pimwasm__module_t{i}(wasm_rt_function_ptr_t target, void *context'
            decl += ''.join(f', {C_TYPES[t]} a{j}' for j,t in enumerate(sig['parameters'])) + ')'
            prototypes += 'static ' + decl + ';\n'
            dispatchers += '\nstatic ' + decl + ' {\n'
            for wrapper, target_sig in wrappers.items():
                if target_sig != sig: continue
                call = wrapper + '(context' + ''.join(f', a{j}' for j in range(len(sig['parameters']))) + ')'
                action = call + '; return;' if not sig['results'] else 'return ' + call + ';'
                dispatchers += f'  if (target == (wasm_rt_function_ptr_t){wrapper}) {{ {action} }}\n'
            dispatchers += '  wasm_rt_trap(WASM_RT_TRAP_CALL_INDIRECT);\n}\n'
    original_module_hash = hashlib.sha256(module.encode()).hexdigest()
    module = quiet_arithmetic(module)
    if info['exception_tags'] or set(info['operations']) & {'try','catch','catch_all','throw'}:
        from pimwasm_exceptions import lower
        module,info['exception_lowering']=lower(module,require)
    adapted = ('/* Generated by scripts/pimwasm_compile.py; arithmetic NaN guards added. */\n'
               '#include <assert.h>\n#include <stdarg.h>\n#include <stddef.h>\n#include <string.h>\n'
               '#include "module.h"\n' + checks + types + numeric + indirect_support +
               extra_types + wrapper_prototypes + prototypes + dispatchers + binding_wrappers + module)
    if 'table_import' in info:
        adapted+='\nvoid pimwasm_guest_import_table_entries(wasm_rt_funcref_table_t *table, void **contexts) {\n (void)table;(void)contexts;\n'
        for slot,index in enumerate(info.get('table_binding_entries',[])):
            if index is None:continue
            if isinstance(index,dict):
                typ=index['type_index'];wrapper=f'pimwasm_external_table_{slot}';context='NULL'
            else:
                imp=info['function_imports'][index];typ=info['function_types'][index];wrapper=f'pimwasm_native_table_{index}'
                context=f"contexts[{info['import_modules'].index(imp['c_module'])}]"
            adapted+=f' table->data[{slot}]=(wasm_rt_funcref_t){{.func_type=w2c_pimwasm__module_t{typ},.func=(wasm_rt_function_ptr_t){wrapper},.module_instance={context}}};\n' 
        adapted+='}\n'

    return (adapted, [export[2] for export in exports], memories[0] if memories else None,
            original_module_hash, hashlib.sha256(module.encode()).hexdigest())


def c_string(value):
    """Byte-exact UTF-8 C literal; fixed-width octal cannot swallow later digits."""
    if IDENTIFIER.fullmatch(value):
        return '"' + value + '"'
    return '"' + ''.join('\\%03o' % byte for byte in value.encode('utf-8')) + '"'


def generation_exports(data):
    """Rename only unsafe export names in a generator-only copy of the artifact.

    Preserve all other sections byte for byte. Unique names are chosen against
    the complete export namespace, including memory/global names and aliases.
    The original module.wasm and its digest remain the host-facing artifact.
    """
    reader = Reader(data)
    result = bytearray(reader.take(8))
    mapping = {}
    while reader.pos < len(data):
        start = reader.pos
        kind = reader.byte()
        payload = reader.take(reader.uint())
        if kind != 7:
            result.extend(data[start:reader.pos])
            continue
        section = Reader(payload)
        entries = [(section.name(), section.byte(), section.uint())
                   for _ in range(section.uint())]
        section.done()
        used = {name for name, _, _ in entries}
        rewritten = bytearray(encode_uleb(len(entries)))
        for index, (name, export_kind, target) in enumerate(entries):
            safe = name
            if not IDENTIFIER.fullmatch(name):
                safe = f'pimwasm_export_{index}'
                while safe in used:
                    safe += '_'
                used.add(safe)
            mapping[name] = safe
            encoded = safe.encode('utf-8')
            rewritten.extend(encode_uleb(len(encoded)) + encoded + bytes([export_kind]) + encode_uleb(target))
        result.extend(bytes([kind]) + encode_uleb(len(rewritten)) + rewritten)
    return bytes(result), mapping


def write_metadata(stage, info):
    """Write module limits and export descriptors used by the DPU runtime."""
    (stage / "metadata.h").write_text(
        '#ifndef PIMWASM_GENERATED_METADATA_H\n#define PIMWASM_GENERATED_METADATA_H\n'
        f'#define PIMWASM_CALL_DEPTH_LIMIT {info["call_depth_limit"]}u\n'
        f'#define PIMWASM_EXCEPTION_TAG_COUNT {len(info["exception_tags"])}u\n'
        f'#define PIMWASM_EXCEPTION_ENABLED {int(bool(info["exception_tags"] or set(info["operations"]) & {"try","catch","catch_all","throw"}))}u\n'
        f'#define PIMWASM_TABLE_EXTERNREF {int(bool(info["tables"] and info["tables"][0].get("kind")=="externref"))}u\n'
        f'#define PIMWASM_TABLE_COUNT {len(info["tables"])}u\n'
        f'#define PIMWASM_TABLE_ENTRIES {info["tables"][0]["initial"] if info["tables"] else 0}u\n'
        f'#define PIMWASM_TABLE_CAPACITY {min(info["tables"][0]["maximum"], MAX_TABLE_ENTRIES) if info["tables"] and ("table.grow" in info["operations"] or "table_import" in info) else (info["tables"][0]["initial"] if info["tables"] else 0)}u\n'
        f'#define PIMWASM_TABLE_MAXIMUM {info["tables"][0]["maximum"] if info["tables"] else 0}u\n'
        f'#define PIMWASM_MEMORY_COUNT {info["memory_count"]}u\n'
        f'#define PIMWASM_MEMORY_BYTES {info["memory_bytes"]}u\n'
        f'#define PIMWASM_MEMORY_CAPACITY_BYTES {info["memory_capacity_bytes"]}u\n'
        f'#define PIMWASM_MEMORY_MAX_PAGES {info["memory_max_pages"]}u\n'
        f'#define PIMWASM_EXPORT_COUNT {len(info["exports"])}u\n'
        '#define PIMWASM_EXPORT_TABLE ' + ', '.join(
            '{' + str(export['argument_count']) + 'u, ' + c_string(export['name']) + ', '+str(len(export['name'].encode('utf-8'))) + 'u, {' +
            ','.join('PIMWASM_' + t.upper() for t in export['parameter_types']) +
            ('0' if not export['parameter_types'] else '') + '}, PIMWASM_' + export['result_type'].upper() + ', ' + str(export['result_count']) + 'u, {' + ','.join('PIMWASM_'+t.upper() for t in export['result_types']) + ('0' if not export['result_types'] else '') + '}}'
            for export in info['exports']) + '\n'
        f'#define PIMWASM_GLOBAL_COUNT {len(info["global_exports"])}u\n'
        '#define PIMWASM_GLOBAL_TABLE ' + ', '.join(
            '{PIMWASM_' + export['type'].upper() + ', ' + str(export['mutable']) +
            'u, ' + c_string(export['name']) + ', '+str(len(export['name'].encode('utf-8'))) + 'u}' for export in info['global_exports']) + '\n'
        f'#define PIMWASM_TABLE_EXPORT_COUNT {len(info.get("table_exports",[]))}u\n'
        '#define PIMWASM_TABLE_EXPORT_TABLE '+', '.join('{'+c_string(name)+', '+str(len(name.encode('utf-8')))+'u, PIMWASM_TABLE_ENTRIES, PIMWASM_TABLE_MAXIMUM, PIMWASM_TABLE_CAPACITY, PIMWASM_TABLE_ENTRIES}' for name in info.get('table_exports',[]))+'\n#endif\n')


def write_guest(stage, info, functions, globals_, bindings):
    """Generate typed invocation, global access, and import glue."""
    from pimwasm_imports import write as write_imports

    (stage / "guest.h").write_text(
        '#ifndef PIMWASM_GENERATED_GUEST_H\n#define PIMWASM_GENERATED_GUEST_H\n'
        '#include "module.h"\n'
        f'typedef {MODULE_TYPE} pimwasm_guest_t;\n'
        'void pimwasm_guest_init(pimwasm_guest_t *instance);\n'
        'void pimwasm_guest_free(pimwasm_guest_t *instance);\n'
        'uint64_t pimwasm_guest_call_bits(pimwasm_guest_t *, uint32_t, const uint64_t *args, uint64_t *extra);\n'
        'uint64_t pimwasm_guest_global_get_bits(pimwasm_guest_t *, uint32_t);\n'
        'void pimwasm_guest_global_set_bits(pimwasm_guest_t *, uint32_t, uint64_t);\n'
        '#endif\n')
    dispatch = ''
    for index, (function, export) in enumerate(zip(functions, info['exports'])):
        args = []
        for arg, kind in enumerate(export['parameter_types']):
            args.append({'i32':f'(uint32_t)args[{arg}]', 'i64':f'args[{arg}]',
                         'f32':f'bits_f32((uint32_t)args[{arg}])',
                         'f64':f'bits_f64(args[{arg}])'}[kind])
        invocation = function + '(instance' + ''.join(', ' + a for a in args) + ')'
        if export['result_count'] > 1:
            dispatch += f'    case {index}u: {{ {c_result(export["result_types"])} r = {invocation};\n'
            values = []
            for j,kind in enumerate(export['result_types']):
                value = f'r.{TYPE_LETTERS[kind]}{j}'
                if kind in ('f32','f64'): value = kind + '_bits(' + value + ')'
                values.append(value)
            for j,value in enumerate(values[1:]): dispatch += f'      extra[{j}] = {value};\n'
            dispatch += f'      return {values[0]}; }}\n'
            continue
        if export['result_type'] == 'void':
            dispatch += f'    case {index}u: {invocation}; return 0;\n'
            continue
        if export['result_type'] in ('f32','f64'):
            invocation = export['result_type'] + '_bits(' + invocation + ')'
        dispatch += f'    case {index}u: return {invocation};\n'
    get_globals, set_globals = '', ''
    for index, (accessor, export) in enumerate(zip(globals_, info['global_exports'])):
        pointer = accessor + '(instance)'
        kind = export['type']
        if kind in ('f32', 'f64'):
            bits_type, size = ('uint32_t', 4) if kind == 'f32' else ('uint64_t', 8)
            get_globals += (f'    case {index}u: {{ {bits_type} bits; '
                            f'memcpy(&bits, {pointer}, {size}); return bits; }}\n')
            if export['mutable']:
                set_globals += (f'    case {index}u: {{ {bits_type} bits = ({bits_type})value; '
                                f'memcpy({pointer}, &bits, {size}); return; }}\n')
        else:
            get_globals += f'    case {index}u: return *{pointer};\n'
            if export['mutable']:
                set_globals += f'    case {index}u: *{pointer} = ({C_TYPES[kind]})value; return;\n'
    import_arguments=write_imports(stage,info,bindings)
    info['native_bindings']={
        'version':1,'reset':bindings.get('reset'),
        'sources':[{'file':f'binding_{i}.c','sha256':hashlib.sha256((stage/f'binding_{i}.c').read_bytes()).hexdigest()}
                   for i,_ in enumerate(bindings.get('sources',[]))],
        'generated_sha256':hashlib.sha256((stage/'imports.c').read_bytes()).hexdigest()}

    (stage / "guest.c").write_text(
        '/* Generated glue; scalar values cross the native ABI as raw bits. */\n'
        '#include "guest.h"\n#include <stddef.h>\n#include <string.h>\n'
        '#include "imports.c"\n' +
        'static inline float bits_f32(uint32_t x) { float f; memcpy(&f,&x,4); return f; }\n'
        'static inline double bits_f64(uint64_t x) { double f; memcpy(&f,&x,8); return f; }\n'
        'static inline uint32_t f32_bits(float f) { uint32_t x; memcpy(&x,&f,4); return x; }\n'
        'static inline uint64_t f64_bits(double f) { uint64_t x; memcpy(&x,&f,8); return x; }\n'
        'void pimwasm_guest_init(pimwasm_guest_t *instance) { pimwasm_imports_reset(); wasm2c_pimwasm__module_instantiate(instance' +
        import_arguments + '); }\n'
        'void pimwasm_guest_free(pimwasm_guest_t *instance) { wasm2c_pimwasm__module_free(instance); }\n'
        'uint64_t pimwasm_guest_call_bits(pimwasm_guest_t *instance, uint32_t export_index, const uint64_t *args, uint64_t *extra) {\n'
        '  (void)instance; (void)args; switch (export_index) {\n' + dispatch +
        '    default: wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);\n  }\n}\n'
        'uint64_t pimwasm_guest_global_get_bits(pimwasm_guest_t *instance, uint32_t export_index) {\n'
        '  (void)instance; switch (export_index) {\n' + get_globals +
        '    default: wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);\n  }\n}\n'
        'void pimwasm_guest_global_set_bits(pimwasm_guest_t *instance, uint32_t export_index, uint64_t value) {\n'
        '  (void)instance; (void)value; switch (export_index) {\n' + set_globals +
        '    default: wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);\n  }\n}\n')


def compile_module(input_path, output_path, imports_path=None):
    data = input_path.read_bytes()
    from pimwasm_imports import load as load_bindings, generation as generation_imports
    bindings=load_bindings(imports_path)
    info = inspect_wasm(data,bindings)
    require(command(["wasm-validate", "--version"]) == WABT_VERSION,
            "unsupported wasm-validate version")
    require(command(["wasm2c", "--version"]) == WABT_VERSION, "unsupported wasm2c version")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pimwasm-compile-", dir=output_path.parent) as temporary:
        stage = Path(temporary)
        (stage / "module.wasm").write_bytes(data)
        command(["wasm-validate", "--enable-extended-const", "--enable-exceptions", str(stage / "module.wasm")])
        generation, names = generation_exports(data)
        generation=generation_imports(generation,info)
        (stage / "generation.wasm").write_bytes(generation)
        generation_info = dict(info)
        for key in ('exports', 'global_exports'):
            generation_info[key] = [dict(export, name=names[export['name']]) for export in info[key]]
            for export in info[key]:
                export['generator_export_name'] = names[export['name']]
        for key in ('memory_exports','table_exports'):
            generation_info[key]=[names[n] for n in info.get(key,[])]
        command(["wasm2c", "--enable-extended-const", "--enable-exceptions", str(stage / "generation.wasm"), "--no-debug-names",
                 "--module-name=pimwasm_module", "-o", str(stage / "module.c")])
        source, header = (stage / "module.c").read_text(), (stage / "module.h").read_text()
        info['native_guest_functions'] = re.findall(
            r'^static (?:void|u32|u64|f32|f64|wasm_rt_externref_t|struct wasm_multi_[ijfd]+) (w2c_pimwasm__module_\w+)\(w2c_pimwasm__module\*[^;]*\);$', source, re.M)
        require(len(info['native_guest_functions']) == info['function_count'],
                'unexpected generated guest-function declarations')
        adapted, functions, memory, module_hash, adapted_hash = adapt_source(source, header, generation_info)
        if 'exception_lowering' in generation_info: info['exception_lowering']=generation_info['exception_lowering']
        globals_ = global_accessors(header, generation_info)
        (stage / "module_dpu.c").write_text(adapted)
        info["generator_version"] = WABT_VERSION
        info["generated_module_suffix_sha256"] = module_hash
        # Hash the exact transformed suffix too; the original hash remains
        # available for auditing against the unmodified wasm2c output.
        info['adapted_module_suffix_sha256'] = adapted_hash
        info['arithmetic_nan_guards'] = adapted.count(NAN_GUARD_MARKER)
        write_metadata(stage, info)
        write_guest(stage, info, functions, globals_, bindings)
        (stage / "manifest.json").write_text(json.dumps(info, indent=2) + "\n")
        output_path.mkdir(parents=True, exist_ok=True)
        # Regeneration must not leave an executable from an earlier embedding
        # when native compilation subsequently fails, even for identical Wasm.
        changed=any(not (output_path/p.name).is_file() or (output_path/p.name).read_bytes()!=p.read_bytes()
                    for p in stage.iterdir())
        if changed:
            for name in ('dpu','dpu.unchecked','dpu-resources.json'):
                (output_path/name).unlink(missing_ok=True)
        for generated in sorted(stage.iterdir()):
            destination = output_path / generated.name
            # module.wasm may be the build input in this same bundle directory.
            # Preserve its timestamp so adaptation cannot retrigger itself.
            if (generated.name == "module.wasm" and destination.is_file()
                    and destination.read_bytes() == generated.read_bytes()):
                continue
            shutil.copyfile(generated, destination)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument('--imports',type=Path,help='explicit DPU native/object embedding bindings')
    args = parser.parse_args()
    try:
        info = compile_module(args.input, args.out,args.imports)
    except (Rejected, OSError, ValueError, TypeError) as error:
        parser.exit(1, f"PIMWASM rejected module: {error}\n")
    print(f'Accepted module, {len(info["exports"])} function export(s), {len(info["global_exports"])} global export(s): '
          f'{info["memory_bytes"]} memory bytes, SHA-256 {info["sha256"]}')


if __name__ == "__main__":
    main()