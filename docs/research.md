# Research direction

## Research question

**Can portable user-defined functions execute efficiently near resident data,
while the system automatically manages the accelerator’s memory and resource
constraints?**

PIMWASM investigates this through WebAssembly on UPMEM. Users write portable
computation and invoke it through a uniform native host API. The runtime handles
MRAM/WRAM transfers, checked memory access and resource admission. Users should
not need to implement DMA or manage DPU scratch buffers themselves.

## Starting point

We can compile C/WAT through Wasm and `wasm2c` into native DPU code, with a custom
runtime and per-example hosts. Execution currently uses one DPU and one tasklet.
The examples demonstrate functionality; they do not establish efficient execution,
full Wasm compatibility or a validated security sandbox. Previous measurement
logs were removed, so performance evidence must be collected again.

## Steps to answer the question

1. **Establish a reproducible baseline.** Measure the current runtime against
   efficient native PRIM implementations, starting with VA, GEMV and reduction.
   Match inputs, arithmetic and data layouts; independently check results.
2. **Add explicit SPMD invocation.** Let the host supply per-tasklet arguments,
   shared read-only inputs and disjoint output regions. Start with independent
   workers, then allow phases that reuse resident data. Define private globals,
   stacks, caches and trap state. Do not infer parallelism from function names.
3. **Improve automatic memory management.** Evaluate multiple read buffers,
   buffered writes and bounded WRAM scratch. Choose policies using declared
   access requirements or measured behavior, while preserving bounds checks,
   unaligned accesses, invalidation and trap semantics.
4. **Enforce resource limits.** Account for code, WRAM, native stacks and MRAM
   before launch, including per-tasklet costs. Admit a safe configuration or
   reject it explicitly. Add execution budgets and reliable failure recovery.
5. **Define and validate isolation.** Treat guest Wasm as untrusted. Protect
   runtime state and other workers’ data, restrict native imports, and bound
   execution. Audit the adapter and runtime, then test malicious modules,
   overflow cases, unauthorized accesses and recovery. SDK faults alone do not
   enforce Wasm memory boundaries.
6. **Evaluate the combination.** Measure tasklet scaling and memory policies
   separately, then together. Expand to irregular access and multi-phase
   workloads to test whether the mechanisms generalize.

The application still defines its data layout, partitioning and reductions.
The runtime manages placement and transfers within that explicit contract;
ordinary Wasm code is not automatically safe to execute concurrently.

## Evidence to collect

- Resident-data execution time, host launch/wait time, transfers and compilation
  time reported separately, with repeated runs and variability.
- Speed relative to optimized native kernels, with equal work and coordination.
  Sharing a slow DMA helper does not demonstrate negligible Wasm overhead.
- IRAM, WRAM, stack and MRAM usage; DMA calls/bytes and cache hits/misses collected
  in separate instrumented runs.
- Correctness at boundaries, empty inputs, invalid ranges, writes, traps and
  reset. Every optimization retains a checked fallback where applicable.
- Programming effort: which memory and orchestration responsibilities move
  from application code into reusable runtime mechanisms?

## Security question

Can the system enforce memory, control-flow and resource isolation without
losing the performance benefit of near-data execution? Native code generation
must preserve Wasm checks, and shared-data SPMD needs explicit access permissions.
Measure the cost of those protections. Passing functional tests does not establish
a validated sandbox; that claim needs a threat model, adversarial testing and
independent review. Timing side channels and physical attacks need an explicit
scope rather than an implied guarantee.

## Intended contribution

A reusable execution and memory-management design for portable functions on
resource-constrained PIM hardware, supported by measured overheads, scaling and
clear limits. The contribution must go beyond compilation: demonstrate when
runtime-managed memory makes portable execution practical, and where native
implementations retain an advantage.

Full Wasm coverage, service deployment and automatic algorithm partitioning are
outside the initial scope. See [runtime.md](runtime.md) for implementation details.
