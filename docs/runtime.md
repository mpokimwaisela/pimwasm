# Runtime support

PIMWASM compiles an accepted Wasm module into native UPMEM code ahead of time.
The generated computation runs with a custom runtime for memory, numeric
helpers, calls and traps. The native host uses [pimwasm.h](../include/pimwasm.h).
See the [README](../README.md) for build and execution commands.

## Execution model

- One physical DPU, one tasklet and one guest instance per handle. Host calls
  are serial; there is no CPU backend or simulation fallback.
- Guest addresses are offsets into MRAM-backed Wasm linear memory. A 1-KiB WRAM
  read cache supports checked accesses, including unaligned and crossing loads.
  Writes and lifecycle changes invalidate cached data as needed.
- The build validates Wasm, rejects unsupported features, adapts pinned
  `wasm2c` output, and checks linked DPU memory/code/stack requirements.
- The adapter requires WABT `1.0.36 (git~1.0.36-44-g46648b096)` and rejects
  unexpected generated-code patterns. Native bundles and import bindings are
  trusted; the resource verifier is not a security proof.

## Supported features

| Area | Current support |
| --- | --- |
| Scalars | `i32`, `i64`, `f32`, `f64`; arithmetic, comparisons, bit operations, conversions and reinterpretation, including saturating conversions. Floating point uses software support. |
| Control flow | Blocks, loops, branches, direct calls, checked indirect calls and bounded recursion. |
| Function signatures | Zero to 32 scalar arguments and zero to four scalar results; void returns are supported. |
| Memory | Zero or one nonshared memory32; scalar loads/stores, `memory.size`, bounded `memory.grow`, `memory.copy`, `memory.fill`, `memory.init` and `data.drop`. |
| Initialization | Scalar globals, extended integer constant initializers, active/passive data segments and an optional start function. |
| Exports | Functions, scalar globals, memory and tables. Mutable scalar globals can be read/written through the host API. |
| Function tables | One bounded `funcref` table; mutation, size/growth, fill/copy, element initialization/drop and signature-checked indirect calls. |
| External references | Alternatively, one bounded `externref` table with mutation and growth. References stay inside the DPU; trusted native bindings manage their lifetime. |
| Imports | Default `env.abort`, `env.assert` and `env.tasklet_id`; additional functions, scalar globals, memory or funcref tables require explicit build-time bindings. |
| Exceptions | A bounded legacy `try`/`catch`/`catch_all`/`throw` subset, implemented through explicit propagation. This is separate from SDK faults. |

Exception-enabled modules cannot use imports or indirect calls. Imported/exported
tags, `rethrow`, `delegate`, modern `try_table` and exception references are not
supported. Externref values are not supported in host-facing signatures or
exported globals; imported externref tables and externref element segments are
also excluded.

## Resource limits

These are acceptance ceilings, not a promise that every module below them fits.
The final linked program must pass resource checks.

| Resource | Limit |
| --- | --- |
| Wasm binary | 1 MiB |
| Defined functions / function types | 256 each |
| Function exports | 32; export names up to 63 UTF-8 bytes |
| Scalar globals / import entries | 16 / 32 |
| Linear-memory backing | Up to 1,024 pages of 64 KiB: 64 MiB, subject to device capacity |
| Table backing | One funcref **or** externref table, up to 64 entries |
| Recursive call depth | 8, plus native stack admission checks |
| Native tasklet stack | Configured to 4 KiB; linked usage must pass the verifier |
| Exception tags / payload | 16 / 256 bytes |

The full declared initial memory is allocated and zeroed. Modules containing
`memory.grow` reserve backing up to the smaller of their declared maximum and
64 MiB. New pages are zeroed; failed growth returns `-1` without changing memory.
Declared maxima above physical capacity do not guarantee successful growth.

## Host use and failure behavior

1. `pimwasm_open` loads the compiled bundle and initializes the instance, including
   its start function.
2. `pimwasm_write` uploads data into current linear memory.
3. `pimwasm_invoke` calls the sole function export; `pimwasm_invoke_named` selects
   an export when there are several. Arguments/results carry scalar bit patterns.
4. `pimwasm_read` retrieves output. `pimwasm_reset` reinstantiates the module;
   `pimwasm_close` releases resources.

State persists between calls. A Wasm trap returns `PIMWASM_TRAPPED` with a reason
and no result values. Effects completed before the trap remain visible; traps
are not transactional rollback. A trapping start fails open. A failed reset
makes the instance unavailable until a later reset succeeds.

Invalid host arguments return `PIMWASM_INVALID`. SDK faults and transport failures
return `PIMWASM_BACKEND_ERROR`; guest-trap recovery does not guarantee recovery
from hardware faults. There is no execution budget or infinite-loop cancellation
guarantee yet.

## Unsupported and potential additions

Currently excluded: SIMD, threads/atomics, shared memory, multiple memories,
memory64, GC, tail calls, full exception handling, general WASI and automatic
runtime module linking. Native bindings are not a general Wasm module linker.

Prioritize additions that answer the [research question](research.md):

- Explicit SPMD across tasklets, with private execution state and defined data
  ownership, followed by multi-DPU execution.
- More general read/write buffering and bounded WRAM scratch management.
- Execution budgets and reliable fault recovery.
- Security validation: restricted imports, memory/control-flow isolation,
  adversarial testing and independent review.
- Additional Wasm features when required by useful workloads.

This is a bounded research runtime, not full Wasm compatibility or a validated
security sandbox.
