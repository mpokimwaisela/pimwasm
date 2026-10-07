"""Adapt wasm2c-generated exception handling for the DPU.

This is a helper module called by pimwasm_compile.py, not a standalone command.
Its lower() function takes generated C text and a rejection-check function.
It returns transformed C and counts of handlers, throws and call-site checks.

We need it because the DPU SDK does not provide setjmp/longjmp, which the pinned
wasm2c generator uses for legacy Wasm exceptions. This module replaces that
mechanism with explicit pending-exception checks, returns and jumps to handlers.
Guest computation, tag matching and payload extraction remain generated code.

It handles the admitted legacy try/catch/catch_all/throw subset and rejects
unexpected generated patterns. Other exception restrictions are enforced by
pimwasm_compile.py before this helper is called.

Success means the expected exception patterns were transformed and the checked
patterns requiring nonlocal jumps are absent. It does not mean full Wasm
exception support or that the transformed program has run correctly; native
compilation, resource checks and execution still follow.
"""

import re


def lower(module, require):
    output = []
    depth = 0
    function = None
    handlers = []
    tries = throws = guards = prologues = 0
    for line in module.splitlines(keepends=True):
        stripped = line.strip()
        indent = line[: len(line) - len(line.lstrip())]
        signature = re.match(
            r"(void|u32|u64|f32|f64|wasm_rt_externref_t|struct wasm_multi_[ijfd]+) (w2c_\w+)\([^;]*\) \{",
            stripped,
        )
        if signature:
            require(function is None, "unexpected nested generated function")
            function = (signature[1], depth, False)
        if function and stripped == "FUNC_PROLOGUE;":
            function = (*function[:2], True)
            prologues += 1
        if function and function[2]:

            def propagation():
                active = [h for h in handlers if h["phase"] == "try"]
                if active:
                    return "goto " + active[-1]["label"] + ";"
                result = function[0]
                ret = (
                    "return;"
                    if result == "void"
                    else (
                        "return (" + result + "){0};"
                        if result.startswith("struct ")
                        else "return 0;"
                    )
                )
                return "FUNC_EPILOGUE; " + ret

            if "WASM_RT_UNWIND_TARGET" in line:
                require(
                    re.fullmatch(
                        r"WASM_RT_UNWIND_TARGET(?: \*var_T\d+_outer_target = wasm_rt_get_unwind_target\(\)| var_T\d+_unwind_target);",
                        stripped,
                    ),
                    "unexpected generated exception target",
                )
                continue
            match = re.fullmatch(
                r"if \(!wasm_rt_try\((var_T\d+)_unwind_target\)\) \{", stripped
            )
            if match:
                handlers.append(
                    {
                        "depth": depth,
                        "phase": "try",
                        "label": "pimwasm_" + match[1] + "_catch_" + str(tries),
                    }
                )
                line = (
                    indent + "if (1) { /* pimwasm: explicit exception propagation */\n"
                )
                tries += 1
            elif (
                handlers
                and depth == handlers[-1]["depth"] + 1
                and stripped == "} else {"
            ):
                handler = handlers[-1]
                require(handler["phase"] == "try", "unexpected exception catch schema")
                handler["phase"] = "catch"
                line = (
                    indent
                    + "} else {\n"
                    + indent
                    + "  "
                    + handler["label"]
                    + ":;\n"
                    + indent
                    + "  wasm_rt_exception_clear();\n"
                )
            elif "wasm_rt_set_unwind_target" in line:
                require(
                    re.fullmatch(
                        r"wasm_rt_set_unwind_target\((?:&var_T\d+_unwind_target|var_T\d+_outer_target)\);",
                        stripped,
                    ),
                    "unexpected exception target restore",
                )
                continue
            elif stripped == "wasm_rt_throw();":
                line = indent + "wasm_rt_throw(); " + propagation() + "\n"
                throws += 1
            else:
                calls = re.findall(r"\b(w2c_\w+)\(", stripped)
                if calls:
                    require(
                        len(calls) == 1
                        and stripped.endswith(");")
                        and not stripped.startswith(("if", "return")),
                        "unexpected generated exception call schema",
                    )
                    line += (
                        indent
                        + "if (wasm_rt_exception_pending()) { "
                        + propagation()
                        + " }\n"
                    )
                    guards += 1
        # Count original structural braces; replacement adds only balanced ones.
        depth += stripped.count("{") - stripped.count("}")
        while handlers and depth <= handlers[-1]["depth"]:
            handlers.pop()
        output.append(line)
        if function and depth == function[1]:
            function = None
    result = "".join(output)
    require(
        prologues == module.count("FUNC_PROLOGUE;"),
        "unexpected generated guest function schema for exceptions",
    )
    require(not handlers and function is None, "unterminated generated exception scope")
    require(
        "wasm_rt_try(" not in result
        and "wasm_rt_set_unwind_target" not in result
        and "WASM_RT_UNWIND_TARGET" not in result,
        "unlowered exception operation",
    )
    return result, {"tries": tries, "throws": throws, "call_guards": guards}
