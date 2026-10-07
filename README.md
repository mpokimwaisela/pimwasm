# PIMWASM

Compile portable WebAssembly functions into native UPMEM DPU programs, then
invoke them from a native C host. Currently, execution uses one physical DPU
and one tasklet.

## 1. Write the application

Each example has two files:

- `guest.c` (or `guest.wat`): the computation compiled for the DPU.
- `host.c`: prepares data, loads the DPU program and invokes the computation.

For example, [count/guest.c](examples/count/guest.c) counts values above a threshold.
Its exported C function is named `count`, matching its example directory.
Guest pointers refer to offsets in Wasm linear memory, not native host addresses.

## 2. Compile it

You need the UPMEM SDK, GCC, Python 3, `clang-15`, `wasm-ld-15`, and WABT
(`wasm2c`, `wat2wasm`, `wasm-validate`). The adapter expects WABT
`1.0.36 (git~1.0.36-44-g46648b096)`. Use `make versions` to inspect installed tools.

From the project root:

```sh
cd ~/mpoki/pimwasm
make example NAME=count
```

The build performs this pipeline:

```text
Guest C → clang-15 → Wasm object → wasm-ld-15 → module.wasm
Guest WAT → wat2wasm → module.wasm
module.wasm → wasm2c → adapted C + DPU runtime → UPMEM compiler → dpu
Host C + libpimwasm.a + UPMEM SDK → GCC → host
```

Wasm validation, supported-feature checks and DPU resource checks run during
compilation. This is ahead-of-time compilation: the DPU runs native code,
with runtime support for Wasm memory and traps.

All artifacts go in `build/examples/count/`, including `module.wasm`, generated
C, `dpu` and `host`. Rebuild after editing either source file.

## 3. Run it

Run from the project root so the host can find its compiled bundle:

```sh
./build/examples/count/host 32 21   # length, threshold
```

The [count host](examples/count/host.c) opens the bundle with `pimwasm_open`,
uploads input data, passes typed arguments to `pimwasm_invoke`, checks the result
against a host reference, and releases the DPU with `pimwasm_close`.
The Wasm file is a build artifact; execution loads the native `dpu` binary.

## 4. Try other examples

```sh
make -j4 examples                 # Build everything; also the default target
./build/examples/add/host 20 22
./build/examples/VA/host 1024 7    # vector length, seed
./build/examples/GEMV/host 16 32 7 # rows, columns, seed
```

To add an example, create `examples/<name>/guest.c` and `host.c`, using count as
a starting point. Name the C entry function `<name>` and update the host's bundle
path to `build/examples/<name>`. Examples under `examples/prim/` use the same
build rules; directory names must be unique. WAT declares its exports directly.

The public API is [pimwasm.h](include/pimwasm.h); the runtime is in `src/dpu/`.
See [runtime.md](docs/runtime.md) for supported features and resource limits.
This is a bounded research runtime, not full Wasm compatibility or a validated
security sandbox. Simulation is rejected. No automated test suite is included.
## Third-party reference code

The original PRIM benchmarks are a Git submodule at
`third-party/prim-benchmarks/`. They are reference code; building PIMWASM
examples does not require them. Git records their repository URL and pinned
commit rather than copying their source into this repository.

To download them after cloning:

```sh
git submodule update --init --recursive
```
