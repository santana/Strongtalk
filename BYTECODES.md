# Strongtalk Bytecodes

Complete reference for the Delta/Strongtalk bytecode set (version 2), the
threaded-code interpreter that executes it, and how to extend it.

The authoritative source of this document is `vm/interpreter/bytecodes.hpp`
(the `def()` calls in `vm/interpreter/bytecodes.cpp`) and
`vm/interpreter/interpreter.cpp` (the generated handlers). Additional reference:
`documentation/internal/vm/bctable.pdf` (the original Digitalk bytecode table).

---

## 1. Overview

The bytecode set is **version 2**, matching the original Digitalk Smalltalk
system (`Bytecodes::version()`).

| Property            | Value                          |
| ------------------- | ------------------------------ |
| Opcode address space | 256 codes (`0x00`–`0xFF`)      |
| Opcode size          | 1 byte                         |
| Operand alignment   | Oop/long operands aligned to word size |
| Bytecode set version| 2                              |

Each instruction is a **1-byte opcode** followed by **zero or more operands**.
Operands come in three widths:

- **byte (`B`)** — a bare `uint8_t` immediately after the opcode.
- **long (`L`)** — a word-sized signed operand (jump offset, instance-variable
  offset, primitive id). It occupies one aligned word slot — 4 bytes in the
  original 32-bit VM, 8 bytes in the 64-bit port — always on a word boundary.
- **oop (`O`)** — a word-sized reference to an object in the method's oop table
  (literal, selector, inline-cache entry, global association, ...).

**Alignment rule:** a `long` or `oop` operand is always stored **aligned to the
word size** on a word boundary within the bytecode stream. The bytes between the
previous byte operand and the aligned operand are padding, filled with `0xFF`
(the `halt` opcode) so that a mis-decoded instruction stops the interpreter
instead of doing something random. The `*` in the format names (below) marks
this padding.

---

## 2. Instruction Formats

`Bytecodes::Format` encodes the operand layout. Format names are strings of
`B`/`L`/`O`/`S` characters:

| Format  | Meaning                                        | Operand layout                            |
| ------- | ---------------------------------------------- | ----------------------------------------- |
| `B`     | opcode only                                    | `{op}`                                    |
| `BB`    | opcode + 1 byte                                | `{op, b1}`                                |
| `BBB`   | opcode + 2 bytes                               | `{op, b1, b2}`                            |
| `BBBB`  | opcode + 3 bytes                               | `{op, b1, b2, b3}`                        |
| `BO`    | opcode + aligned oop                           | `{op, *, oop}`                            |
| `BOO`   | opcode + 2 aligned oops                        | `{op, *, oop, oop}`                       |
| `BL`    | opcode + aligned long                          | `{op, *, long}`                           |
| `BBO`   | opcode + byte + aligned oop                    | `{op, b1, *, oop}`                        |
| `BBOO`  | opcode + byte + 2 aligned oops                 | `{op, b1, *, oop, oop}`                   |
| `BBL`   | opcode + 2 bytes + aligned long                | `{op, b1, b2, *, long}`                   |
| `BLO`   | opcode + aligned long + aligned oop            | `{op, *, long, oop}`                      |
| `BLL`   | opcode + 2 aligned longs                       | `{op, *, long, long}`                     |
| `BBLO`  | opcode + byte + aligned long + aligned oop     | `{op, b1, *, long, oop}`                  |
| `BOL`   | opcode + aligned oop + aligned long            | `{op, *, oop, long}`                      |
| `BOOLB` | opcode + oop + oop + long + byte (DLL call)    | `{op, *, oop, oop, long, b1}`             |
| `BBS`   | opcode + byte count + that many bytes          | `{op, n, {byte}×n}`                       |
| `UNDEF` | reserved / undefined slot                      | —                                         |

> `*` marks word-alignment padding (bytes with value `0xFF` / `halt`).
>
> `BLB` appears in `bytecodes.hpp` but is unused by any concrete `def()`; it was
> intended as opcode + aligned long + byte. Do not rely on it.

Concrete layout of a send (see §5 for exact cache layouts):

```
[opcode] [n] 0xFF 0xFF  [word 0: selector]  [word 1: cached method/class]
```

---

## 3. Bytecode Categories

Every code is assigned a `CodeType` (14 categories):

| CodeType             | Pops | Description                                            |
| -------------------- | ---- | ------------------------------------------------------ |
| `local_access`       | —    | Load/store temporaries and arguments of the current method |
| `instVar_access`     | —    | Load/store instance variables (by offset or by name)   |
| `context_access`     | —    | Load/store temporaries of an enclosing (heap) context  |
| `classVar_access`    | —    | Load/store class variables                             |
| `global_access`      | —    | Load/store global variables (associations)             |
| `new_closure`        | —    | Closure creation (`[]` blocks)                         |
| `new_context`        | —    | Context creation (method/block contexts)               |
| `control_struc`      | —    | Branches, loops, local returns, `halt`                 |
| `message_send`       | —    | All message sends                                      |
| `nonlocal_return`    | —    | Non-local returns (block → enclosing method)           |
| `primitive_call`     | —    | Direct primitive calls                                 |
| `dll_call`           | —    | Foreign-function (DLL) calls                           |
| `float_operation`    | —    | Floating-point arithmetic (operand registers)          |
| `miscellaneous`      | —    | Everything else (pushes, pops, returns, allocates)     |

**Single stepping:** `single_step` is enabled for most codes that store or send
(used by the debugger's step mode). Control structures are *never* intercepted
(`control_struc` ⇒ `single_step == false`).

---

## 4. Complete Bytecode Table

`Pop` = instruction pops TOS when done. Reserved slots are shown as
`unimplemented_XX`.

### 4.0 `0x00`–`0x0F` — temp/argument loads, allocation

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x00 | `push_temp_0` | B | local_access | — | | Push temporary 0 |
| 0x01 | `push_temp_1` | B | local_access | — | | Push temporary 1 |
| 0x02 | `push_temp_2` | B | local_access | — | | Push temporary 2 |
| 0x03 | `push_temp_3` | B | local_access | — | | Push temporary 3 |
| 0x04 | `push_temp_4` | B | local_access | — | | Push temporary 4 |
| 0x05 | `push_temp_5` | B | local_access | — | | Push temporary 5 |
| 0x06 | `unimplemented_06` | UNDEF | — | — | | Reserved |
| 0x07 | `push_temp_n` | BB | local_access | `n` (byte) | | Push temporary `n` |
| 0x08 | `push_arg_1` | B | local_access | — | | Push argument 1 |
| 0x09 | `push_arg_2` | B | local_access | — | | Push argument 2 |
| 0x0A | `push_arg_3` | B | local_access | — | | Push argument 3 |
| 0x0B | `push_arg_n` | BB | local_access | `n` (byte) | | Push argument `n` |
| 0x0C | `allocate_temp_1` | B | miscellaneous | — | | Push nil, allocate temp slot 1 |
| 0x0D | `allocate_temp_2` | B | miscellaneous | — | | Push nil, allocate temp slot 2 |
| 0x0E | `allocate_temp_3` | B | miscellaneous | — | | Push nil, allocate temp slot 3 |
| 0x0F | `allocate_temp_n` | BB | miscellaneous | `n` (byte; 0 ⇒ 256) | | Push nil, allocate `n` temps |

### 4.1 `0x10`–`0x1F` — temp stores, immediate pushes

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x10 | `store_temp_0_pop` | B | local_access | — | ✓ | Store TOS to temp 0, pop |
| 0x11 | `store_temp_1_pop` | B | local_access | — | ✓ | Store TOS to temp 1, pop |
| 0x12 | `store_temp_2_pop` | B | local_access | — | ✓ | Store TOS to temp 2, pop |
| 0x13 | `store_temp_3_pop` | B | local_access | — | ✓ | Store TOS to temp 3, pop |
| 0x14 | `store_temp_4_pop` | B | local_access | — | ✓ | Store TOS to temp 4, pop |
| 0x15 | `store_temp_5_pop` | B | local_access | — | ✓ | Store TOS to temp 5, pop |
| 0x16 | `store_temp_n` | BB | local_access | `n` (byte, `255−b`) | | Store TOS to temp `n` |
| 0x17 | `store_temp_n_pop` | BB | local_access | `n` (byte, `255−b`) | ✓ | Store TOS to temp `n`, pop |
| 0x18 | `push_neg_n` | BB | miscellaneous | `n` (byte) | | Push `SMI(−b)` |
| 0x19 | `push_succ_n` | BB | miscellaneous | `n` (byte) | | Push `SMI(b+1)` |
| 0x1A | `push_literal` | BO | miscellaneous | `literal` (oop) | | Push literal |
| 0x1B | `push_tos` | B | miscellaneous | — | | Push TOS (duplicate top) |
| 0x1C | `push_self` | B | miscellaneous | — | | Push `self` |
| 0x1D | `push_nil` | B | miscellaneous | — | | Push `nil` |
| 0x1E | `push_true` | B | miscellaneous | — | | Push `true` |
| 0x1F | `push_false` | B | miscellaneous | — | | Push `false` |

`store_temp_n`'s operand is encoded as `255 − n` (a wrapped form so that common
counts have short encodings).

### 4.2 `0x20`–`0x2F` — instance/class variable access (offset forms)

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x20 | `unimplemented_20` | UNDEF | — | — | | Reserved |
| 0x21 | `unimplemented_21` | UNDEF | — | — | | Reserved |
| 0x22 | `unimplemented_22` | UNDEF | — | — | | Reserved |
| 0x23 | `unimplemented_23` | UNDEF | — | — | | Reserved |
| 0x24 | `unimplemented_24` | UNDEF | — | — | | Reserved |
| 0x25 | `unimplemented_25` | UNDEF | — | — | | Reserved |
| 0x26 | `unimplemented_26` | UNDEF | — | — | | Reserved |
| 0x27 | `unimplemented_27` | UNDEF | — | — | | Reserved |
| 0x28 | `return_instVar_name` | BO | instVar_access | `name` (symbolOop) | | Return instVar looked up by name |
| 0x29 | `push_classVar` | BO | classVar_access | `assoc` (associationOop) | | Push class variable |
| 0x2A | `store_classVar_pop` | BO | classVar_access | `assoc` | ✓ | Store TOS to class var, pop |
| 0x2B | `store_classVar` | BO | classVar_access | `assoc` | | Store TOS to class var |
| 0x2C | `return_instVar` | BL | instVar_access | `offset` (long) | | Return instance variable at `offset` |
| 0x2D | `push_instVar` | BL | instVar_access | `offset` (long) | | Push instance variable at `offset` |
| 0x2E | `store_instVar_pop` | BL | instVar_access | `offset` (long) | ✓ | Store TOS to instVar at `offset`, pop |
| 0x2F | `store_instVar` | BL | instVar_access | `offset` (long) | | Store TOS to instVar at `offset` |

### 4.3 `0x30`–`0x3F` — float operations, named instVar access

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x30 | `float_allocate` | BBBB | float_operation | `nofTemps, nofFloats, floatExprStack` | | Allocate float registers |
| 0x31 | `float_floatify_pop` | BB | float_operation | `floatNum` | ✓ | Convert TOS oop → float register, pop |
| 0x32 | `float_move` | BBB | float_operation | `dstFloat, srcFloat` | | Move between float registers |
| 0x33 | `float_set` | BBO | float_operation | `floatNum, value` (doubleOop) | | Set float register from double literal |
| 0x34 | `float_nullary_op` | BBB | float_operation | `floatNum, function` | | 0-operand float op, result → `floatNum` |
| 0x35 | `float_unary_op` | BBB | float_operation | `floatNum, function` | | 1-operand float op, result → `floatNum` |
| 0x36 | `float_binary_op` | BBB | float_operation | `floatNum, function` | | 2-operand float op (`floatNum`, `floatNum−1`), result → `floatNum` |
| 0x37 | `float_unary_op_to_oop` | BBB | float_operation | `floatNum, function` | | 1-operand float op, result → oop TOS |
| 0x38 | `float_binary_op_to_oop` | BBB | float_operation | `floatNum, function` | | 2-operand float op, result → oop TOS |
| 0x39 | `unimplemented_39` | UNDEF | — | — | | Reserved |
| 0x3A | `unimplemented_3a` | UNDEF | — | — | | Reserved |
| 0x3B | `unimplemented_3b` | UNDEF | — | — | | Reserved |
| 0x3C | `unimplemented_3c` | UNDEF | — | — | | Reserved |
| 0x3D | `push_instVar_name` | BO | instVar_access | `name` (symbolOop) | | Push instVar looked up by name |
| 0x3E | `store_instVar_pop_name` | BO | instVar_access | `name` | ✓ | Store TOS to named instVar, pop |
| 0x3F | `store_instVar_name` | BO | instVar_access | `name` | | Store TOS to named instVar |

The `*_name` variants (0x28, 0x3D–0x3F, 0xF3–0xF5 below) resolve the variable
**by name at runtime** — they are defined but not yet implemented by the
interpreter generator (see §9).

### 4.4 `0x40`–`0x4F` — context access (level 0), closures, method contexts

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x40 | `push_temp_0_context_0` | B | context_access | — | | Push temp 0 of context level 0 |
| 0x41 | `push_temp_1_context_0` | B | context_access | — | | Push temp 1 of context level 0 |
| 0x42 | `push_temp_2_context_0` | B | context_access | — | | Push temp 2 of context level 0 |
| 0x43 | `push_temp_n_context_0` | BB | context_access | `n` | | Push temp `n` of context level 0 |
| 0x44 | `store_temp_0_context_0_pop` | B | context_access | — | ✓ | Store TOS to context-0 temp 0, pop |
| 0x45 | `store_temp_1_context_0_pop` | B | context_access | — | ✓ | Store to context-0 temp 1, pop |
| 0x46 | `store_temp_2_context_0_pop` | B | context_access | — | ✓ | Store to context-0 temp 2, pop |
| 0x47 | `store_temp_n_context_0_pop` | BB | context_access | `n` | ✓ | Store to context-0 temp `n`, pop |
| 0x48 | `push_new_closure_context_0` | BO | new_closure | `method` (methodOop) | | Push closure (0 args, uses context) |
| 0x49 | `push_new_closure_context_1` | BO | new_closure | `method` | | Push closure (1 arg, uses context) |
| 0x4A | `push_new_closure_context_2` | BO | new_closure | `method` | | Push closure (2 args, uses context) |
| 0x4B | `push_new_closure_context_n` | BBO | new_closure | `n, method` | | Push closure (`n` args, uses context) |
| 0x4C | `install_new_context_method_0` | B | new_context | — | | Install method context (0 vars) |
| 0x4D | `install_new_context_method_1` | B | new_context | — | | Install method context (1 var) |
| 0x4E | `install_new_context_method_2` | B | new_context | — | | Install method context (2 vars) |
| 0x4F | `install_new_context_method_n` | BB | new_context | `n` | | Install method context (`n` vars) |

### 4.5 `0x50`–`0x5F` — context access (level 1), closures (TOS), block contexts

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x50 | `push_temp_0_context_1` | B | context_access | — | | Push temp 0 of context level 1 |
| 0x51 | `push_temp_1_context_1` | B | context_access | — | | Push temp 1 of context level 1 |
| 0x52 | `push_temp_2_context_1` | B | context_access | — | | Push temp 2 of context level 1 |
| 0x53 | `push_temp_n_context_1` | BB | context_access | `n` | | Push temp `n` of context level 1 |
| 0x54 | `store_temp_0_context_1_pop` | B | context_access | — | ✓ | Store to context-1 temp 0, pop |
| 0x55 | `store_temp_1_context_1_pop` | B | context_access | — | ✓ | Store to context-1 temp 1, pop |
| 0x56 | `store_temp_2_context_1_pop` | B | context_access | — | ✓ | Store to context-1 temp 2, pop |
| 0x57 | `store_temp_n_context_1_pop` | BB | context_access | `n` | ✓ | Store to context-1 temp n, pop |
| 0x58 | `push_new_closure_tos_0` | BO | new_closure | `method` | | Push closure (0 args, uses TOS) |
| 0x59 | `push_new_closure_tos_1` | BO | new_closure | `method` | | Push closure (1 arg, TOS) |
| 0x5A | `push_new_closure_tos_2` | BO | new_closure | `method` | | Push closure (2 args, TOS) |
| 0x5B | `push_new_closure_tos_n` | BBO | new_closure | `n, method` | | Push closure (`n` args, TOS) |
| 0x5C | `only_pop` | B | new_context | — | ✓ | Pop TOS (discard value) |
| 0x5D | `install_new_context_block_1` | B | new_context | — | | Install block context (1 var) |
| 0x5E | `install_new_context_block_2` | B | new_context | — | | Install block context (2 vars) |
| 0x5F | `install_new_context_block_n` | BB | new_context | `n` | | Install block context (`n` vars) |

### 4.6 `0x60`–`0x6F` — context access (level n), self/param copies into context

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x60 | `push_temp_0_context_n` | BB | context_access | `level` | | Push temp 0 of context at `level` |
| 0x61 | `push_temp_1_context_n` | BB | context_access | `level` | | Push temp 1 of context at `level` |
| 0x62 | `push_temp_2_context_n` | BB | context_access | `level` | | Push temp 2 of context at `level` |
| 0x63 | `push_temp_n_context_n` | BBB | context_access | `level, n` | | Push temp `n` at context `level` |
| 0x64 | `store_temp_0_context_n_pop` | BB | context_access | `level` | ✓ | Store to temp 0 at `level`, pop |
| 0x65 | `store_temp_1_context_n_pop` | BB | context_access | `level` | ✓ | Store to temp 1 at `level`, pop |
| 0x66 | `store_temp_2_context_n_pop` | BB | context_access | `level` | ✓ | Store to temp 2 at `level`, pop |
| 0x67 | `store_temp_n_context_n_pop` | BBB | context_access | `level, n` | ✓ | Store to temp n at `level`, pop |
| 0x68 | `set_self_via_context` | B | context_access | — | | Walk context chain to find home, set `self` |
| 0x69 | `copy_1_into_context` | BB | context_access | `n` | | Copy 1 param into context |
| 0x6A | `copy_2_into_context` | BBB | context_access | `n` | | Copy 2 params into context |
| 0x6B | `copy_n_into_context` | BBS | context_access | `n, {byte}×n` | | Copy `n` params into context |
| 0x6C | `copy_self_into_context` | B | context_access | — | | Copy `self` into context |
| 0x6D | `copy_self_1_into_context` | BB | context_access | `n` | | Copy `self` + 1 param |
| 0x6E | `copy_self_2_into_context` | BBB | context_access | `n` | | Copy `self` + 2 params |
| 0x6F | `copy_self_n_into_context` | BBS | context_access | `n, {byte}×n` | | Copy `self` + `n` params |

### 4.7 `0x70`–`0x7F` — control structures (branches, loops)

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x70 | `ifTrue_byte` | BBB | control_struc | `info, offset` (bytes) | | Jump forward if TOS is `true` |
| 0x71 | `ifFalse_byte` | BBB | control_struc | `info, offset` (bytes) | | Jump forward if TOS is `false` |
| 0x72 | `and_byte` | BB | control_struc | `offset` (byte) | | Short-circuit `and` — jump over second operand |
| 0x73 | `or_byte` | BB | control_struc | `offset` (byte) | | Short-circuit `or` — jump over second operand |
| 0x74 | `whileTrue_byte` | BB | control_struc | `offset` (byte) | | Loop while TOS is `true` (backward jump) |
| 0x75 | `whileFalse_byte` | BB | control_struc | `offset` (byte) | | Loop while TOS is `false` (backward) |
| 0x76 | `jump_else_byte` | BB | control_struc | `offset` (byte) | | Unconditional jump |
| 0x77 | `jump_loop_byte` | BBB | control_struc | `loopInfo, offset` (bytes) | | Backward jump (with stack check) |
| 0x78 | `ifTrue_word` | BBL | control_struc | `info, offset` (long) | | Jump forward if TOS is `true` |
| 0x79 | `ifFalse_word` | BBL | control_struc | `info, offset` (long) | | Jump forward if TOS is `false` |
| 0x7A | `and_word` | BL | control_struc | `offset` (long) | | Short-circuit `and` |
| 0x7B | `or_word` | BL | control_struc | `offset` (long) | | Short-circuit `or` |
| 0x7C | `whileTrue_word` | BL | control_struc | `offset` (long) | | Loop while TOS is `true` |
| 0x7D | `whileFalse_word` | BL | control_struc | `offset` (long) | | Loop while TOS is `false` |
| 0x7E | `jump_else_word` | BL | control_struc | `offset` (long) | | Unconditional jump |
| 0x7F | `jump_loop_word` | BLL | control_struc | `loopInfo, offset` (longs) | | Backward jump (with stack check) |

**Jump semantics:** forward jumps target `next_instruction + offset`; backward
jumps (`whileTrue`, `whileFalse`, `jump_loop`) target
`current_instruction − offset`. `and_byte/word` and `or_byte/word` test TOS and
either consume it (result already determined) or branch to skip the second
operand evaluation. `loopType` distinguishes `loop_start` (`jump_loop_*`) from
`loop_end` (`whileTrue/whileFalse`).

The `info`/`loopInfo` operands of `ifTrue`, `ifFalse`, `jump_loop` are decoder
metadata written by the compiler (`at: pos fixupFlags: jumpInfo:`) — they record
branch distances/flags for the decompiler and are skipped by the interpreter.
The runtime reads only the `offset` operand.

### 4.8 `0x80`–`0x8F` — interpreted sends, TOS returns

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x80 | `interpreted_send_0` | BOO | message_send | `sel, cache` | | Send to interpreted method, 0 args |
| 0x81 | `interpreted_send_1` | BOO | message_send | `sel, cache` | | Send, 1 arg |
| 0x82 | `interpreted_send_2` | BOO | message_send | `sel, cache` | | Send, 2 args |
| 0x83 | `interpreted_send_n` | BBOO | message_send | `n, sel, cache` | | Send, `n` args |
| 0x84 | `interpreted_send_0_pop` | BOO | message_send | `sel, cache` | ✓ | Send, 0 args, pop result |
| 0x85 | `interpreted_send_1_pop` | BOO | message_send | `sel, cache` | ✓ | Send, 1 arg, pop result |
| 0x86 | `interpreted_send_2_pop` | BOO | message_send | `sel, cache` | ✓ | Send, 2 args, pop result |
| 0x87 | `interpreted_send_n_pop` | BBOO | message_send | `n, sel, cache` | ✓ | Send, n args, pop result |
| 0x88 | `interpreted_send_self` | BOO | message_send | `sel, cache` | | `self` send |
| 0x89 | `interpreted_send_self_pop` | BOO | message_send | `sel, cache` | ✓ | `self` send, pop |
| 0x8A | `interpreted_send_super` | BOO | message_send | `sel, cache` | | `super` send |
| 0x8B | `interpreted_send_super_pop` | BOO | message_send | `sel, cache` | ✓ | `super` send, pop |
| 0x8C | `return_tos_pop_0` | B | miscellaneous | — | ✓ | Return TOS, pop 0 args |
| 0x8D | `return_tos_pop_1` | B | miscellaneous | — | ✓ | Return TOS, pop 1 arg |
| 0x8E | `return_tos_pop_2` | B | miscellaneous | — | ✓ | Return TOS, pop 2 args |
| 0x8F | `return_tos_pop_n` | BB | miscellaneous | `n` | ✓ | Return TOS, pop `n` args |

### 4.9 `0x90`–`0x9F` — polymorphic sends, self returns

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0x90 | `polymorphic_send_0` | BOO | message_send | `sel, pic` | | Polymorphic send, 0 args |
| 0x91 | `polymorphic_send_1` | BOO | message_send | `sel, pic` | | Polymorphic send, 1 arg |
| 0x92 | `polymorphic_send_2` | BOO | message_send | `sel, pic` | | Polymorphic send, 2 args |
| 0x93 | `polymorphic_send_n` | BBOO | message_send | `n, sel, pic` | | Polymorphic send, `n` args |
| 0x94 | `polymorphic_send_0_pop` | BOO | message_send | `sel, pic` | ✓ | Polymorphic send, pop |
| 0x95 | `polymorphic_send_1_pop` | BOO | message_send | `sel, pic` | ✓ | Polymorphic send, pop |
| 0x96 | `polymorphic_send_2_pop` | BOO | message_send | `sel, pic` | ✓ | Polymorphic send, pop |
| 0x97 | `polymorphic_send_n_pop` | BBOO | message_send | `n, sel, pic` | ✓ | Polymorphic send, pop |
| 0x98 | `polymorphic_send_self` | BOO | message_send | `sel, pic` | | Polymorphic `self` send |
| 0x99 | `polymorphic_send_self_pop` | BOO | message_send | `sel, pic` | ✓ | Polymorphic `self` send, pop |
| 0x9A | `polymorphic_send_super` | BOO | message_send | `sel, pic` | | Polymorphic `super` send |
| 0x9B | `polymorphic_send_super_pop` | BOO | message_send | `sel, pic` | ✓ | Polymorphic `super` send, pop |
| 0x9C | `return_self_pop_0` | B | miscellaneous | — | ✓ | Return `self`, pop 0 args |
| 0x9D | `return_self_pop_1` | B | miscellaneous | — | ✓ | Return `self`, pop 1 arg |
| 0x9E | `return_self_pop_2` | B | miscellaneous | — | ✓ | Return `self`, pop 2 args |
| 0x9F | `return_self_pop_n` | BB | miscellaneous | `n` | ✓ | Return `self`, pop `n` args |

### 4.A `0xA0`–`0xAF` — compiled sends, zap returns, non-local returns

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xA0 | `compiled_send_0` | BLO | message_send | `entry, sel` (nmethod entry, selector) | | Send to compiled method, 0 args |
| 0xA1 | `compiled_send_1` | BLO | message_send | `entry, sel` | | Send to compiled method, 1 arg |
| 0xA2 | `compiled_send_2` | BLO | message_send | `entry, sel` | | Send to compiled method, 2 args |
| 0xA3 | `compiled_send_n` | BBLO | message_send | `n, entry, sel` | | Send to compiled method, `n` args |
| 0xA4 | `compiled_send_0_pop` | BLO | message_send | `entry, sel` | ✓ | Compiled send, pop |
| 0xA5 | `compiled_send_1_pop` | BLO | message_send | `entry, sel` | ✓ | Compiled send, pop |
| 0xA6 | `compiled_send_2_pop` | BLO | message_send | `entry, sel` | ✓ | Compiled send, pop |
| 0xA7 | `compiled_send_n_pop` | BBLO | message_send | `n, entry, sel` | ✓ | Compiled send, pop |
| 0xA8 | `compiled_send_self` | BLO | message_send | `entry, sel` | | Compiled `self` send |
| 0xA9 | `compiled_send_self_pop` | BLO | message_send | `entry, sel` | ✓ | Compiled `self` send, pop |
| 0xAA | `compiled_send_super` | BLO | message_send | `entry, sel` | | Compiled `super` send |
| 0xAB | `compiled_send_super_pop` | BLO | message_send | `entry, sel` | ✓ | Compiled `super` send, pop |
| 0xAC | `return_tos_zap_pop_n` | BB | miscellaneous | `n` | ✓ | Return TOS, zap context, pop `n` args |
| 0xAD | `return_self_zap_pop_n` | BB | miscellaneous | `n` | ✓ | Return `self`, zap context, pop `n` |
| 0xAE | `non_local_return_tos_pop_n` | BB | nonlocal_return | `n` | ✓ | Non-local return with TOS |
| 0xAF | `non_local_return_self_pop_n` | BB | nonlocal_return | `n` | ✓ | Non-local return with `self` |

### 4.B `0xB0`–`0xBF` — primitive calls, DLL calls, accessor/primitive sends

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xB0 | `prim_call` | BL | primitive_call | `entry` (primitive fn) | | Call primitive (resolved) |
| 0xB1 | `predict_prim_call` | BL | primitive_call | `entry` | | Predicted primitive call (runtime no-op) |
| 0xB2 | `prim_call_failure` | BLL | primitive_call | `entry, failure_block` | | Primitive call with failure block |
| 0xB3 | `predict_prim_call_failure` | BLL | primitive_call | `entry, failure_block` | | Predicted call with failure block |
| 0xB4 | `dll_call_sync` | BOOLB | dll_call | `sel, dll, index, n` | | Synchronous DLL call |
| 0xB5 | `prim_call_self` | BL | primitive_call | `entry` | | Primitive call (self form) |
| 0xB6 | `prim_call_self_failure` | BLL | primitive_call | `entry, failure_block` | | Self primitive call with failure block |
| 0xB7 | `unimplemented_b7` | UNDEF | — | — | | Reserved |
| 0xB8 | `access_send_self` | BOO | message_send | `sel, cache` | | Accessor `self` send (instVar predict) |
| 0xB9 | `primitive_send_0` | BOO | message_send | `sel, cache` | | Primitive send, 0 args |
| 0xBA | `primitive_send_super` | BOO | message_send | `sel, cache` | | Primitive `super` send |
| 0xBB | `primitive_send_super_pop` | BOO | message_send | `sel, cache` | ✓ | Primitive `super` send, pop |
| 0xBC | `unimplemented_bc` | UNDEF | — | — | | Reserved |
| 0xBD | `primitive_send_1` | BOO | message_send | `sel, cache` | | Primitive send, 1 arg |
| 0xBE | `primitive_send_2` | BOO | message_send | `sel, cache` | | Primitive send, 2 args |
| 0xBF | `primitive_send_n` | BBOO | message_send | `n, sel, cache` | | Primitive send, `n` args |

### 4.C `0xC0`–`0xCF` — primitive lookups, async DLL, accessor/primitive sends (pop)

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xC0 | `prim_call_lookup` | BO | primitive_call | `selector` | | Primitive call + lookup (patchable) |
| 0xC1 | `predict_prim_call_lookup` | BO | primitive_call | `selector` | | Predicted call + lookup |
| 0xC2 | `prim_call_failure_lookup` | BOL | primitive_call | `selector, failure_word` | | Call with failure + lookup |
| 0xC3 | `predict_prim_call_failure_lookup` | BOL | primitive_call | `selector, failure_word` | | Predicted call with failure + lookup |
| 0xC4 | `dll_call_async` | BOOLB | dll_call | `sel, dll, index, n` | | Asynchronous DLL call |
| 0xC5 | `prim_call_self_lookup` | BO | primitive_call | `selector` | | Self primitive + lookup |
| 0xC6 | `prim_call_self_failure_lookup` | BOL | primitive_call | `selector, failure_word` | | Self primitive + failure + lookup |
| 0xC7 | `unimplemented_c7` | UNDEF | — | — | | Reserved |
| 0xC8 | `access_send_0` | BOO | message_send | `sel, cache` | | Accessor send, 0 args (instVar predict) |
| 0xC9 | `primitive_send_0_pop` | BOO | message_send | `sel, cache` | ✓ | Primitive send, 0 args, pop |
| 0xCA | `primitive_send_self` | BOO | message_send | `sel, cache` | | Primitive `self` send |
| 0xCB | `primitive_send_self_pop` | BOO | message_send | `sel, cache` | ✓ | Primitive `self` send, pop |
| 0xCC | `unimplemented_cc` | UNDEF | — | — | | Reserved |
| 0xCD | `primitive_send_1_pop` | BOO | message_send | `sel, cache` | ✓ | Primitive send, 1 arg, pop |
| 0xCE | `primitive_send_2_pop` | BOO | message_send | `sel, cache` | ✓ | Primitive send, 2 args, pop |
| 0xCF | `primitive_send_n_pop` | BBOO | message_send | `n, sel, cache` | ✓ | Primitive send, `n` args, pop |

### 4.D `0xD0`–`0xDF` — megamorphic sends, special hint

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xD0 | `megamorphic_send_0` | BOO | message_send | `sel, cache` | | Megamorphic send, 0 args |
| 0xD1 | `megamorphic_send_1` | BOO | message_send | `sel, cache` | | Megamorphic send, 1 arg |
| 0xD2 | `megamorphic_send_2` | BOO | message_send | `sel, cache` | | Megamorphic send, 2 args |
| 0xD3 | `megamorphic_send_n` | BBOO | message_send | `n, sel, cache` | | Megamorphic send, n args |
| 0xD4 | `megamorphic_send_0_pop` | BOO | message_send | `sel, cache` | ✓ | Megamorphic send, pop |
| 0xD5 | `megamorphic_send_1_pop` | BOO | message_send | `sel, cache` | ✓ | Megamorphic send, pop |
| 0xD6 | `megamorphic_send_2_pop` | BOO | message_send | `sel, cache` | ✓ | Megamorphic send, pop |
| 0xD7 | `megamorphic_send_n_pop` | BBOO | message_send | `n, sel, cache` | ✓ | Megamorphic send, pop |
| 0xD8 | `megamorphic_send_self` | BOO | message_send | `sel, cache` | | Megamorphic `self` send |
| 0xD9 | `megamorphic_send_self_pop` | BOO | message_send | `sel, cache` | ✓ | Megamorphic `self` send, pop |
| 0xDA | `megamorphic_send_super` | BOO | message_send | `sel, cache` | | Megamorphic `super` send |
| 0xDB | `megamorphic_send_super_pop` | BOO | message_send | `sel, cache` | ✓ | Megamorphic `super` send, pop |
| 0xDC | `unimplemented_dc` | UNDEF | — | — | | Reserved |
| 0xDD | `special_primitive_send_1_hint` | BB | miscellaneous | `n` | | Hint byte (skipped at runtime) |
| 0xDE | `unimplemented_de` | UNDEF | — | — | | Reserved |
| 0xDF | `unimplemented_df` | UNDEF | — | — | | Reserved |

### 4.E `0xE0`–`0xEF` — predicted sends, object identity

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xE0 | `smi_add` | BOO | message_send | `sel, cache` | | Predicted integer `+` |
| 0xE1 | `smi_sub` | BOO | message_send | `sel, cache` | | Predicted integer `-` |
| 0xE2 | `smi_mult` | BOO | message_send | `sel, cache` | | Predicted integer `*` |
| 0xE3 | `smi_div` | BOO | message_send | `sel, cache` | | Predicted integer `/` (not yet implemented) |
| 0xE4 | `smi_mod` | BOO | message_send | `sel, cache` | | Predicted integer `\\` (not yet implemented) |
| 0xE5 | `smi_create_point` | BOO | message_send | `sel, cache` | | Predicted `Point` creation (not yet implemented) |
| 0xE6 | `smi_equal` | BOO | message_send | `sel, cache` | | Predicted integer `=` |
| 0xE7 | `smi_not_equal` | BOO | message_send | `sel, cache` | | Predicted integer `~=` |
| 0xE8 | `smi_less` | BOO | message_send | `sel, cache` | | Predicted integer `<` |
| 0xE9 | `smi_less_equal` | BOO | message_send | `sel, cache` | | Predicted integer `<=` |
| 0xEA | `smi_greater` | BOO | message_send | `sel, cache` | | Predicted integer `>` |
| 0xEB | `smi_greater_equal` | BOO | message_send | `sel, cache` | | Predicted integer `>=` |
| 0xEC | `objArray_at` | BOO | message_send | `sel, cache` | | Predicted `objArray at:` (not yet implemented) |
| 0xED | `objArray_at_put` | BOO | message_send | `sel, cache` | | Predicted `at:put:` (not yet implemented) |
| 0xEE | `double_equal` | B | miscellaneous | — | | Object identity comparison `==` |
| 0xEF | `double_tilde` | B | miscellaneous | — | | Object identity comparison `~~` |

### 4.F `0xF0`–`0xFF` — globals, class vars (name), bit ops, halt

| Hex | Name | Format | Category | Operands | Pop | Description |
| --- | ---- | ------ | -------- | -------- | --- | ----------- |
| 0xF0 | `push_global` | BO | global_access | `assoc` (associationOop) | | Push global variable |
| 0xF1 | `store_global_pop` | BO | global_access | `assoc` | ✓ | Store TOS to global, pop |
| 0xF2 | `store_global` | BO | global_access | `assoc` | | Store TOS to global |
| 0xF3 | `push_classVar_name` | BO | classVar_access | `name` (symbolOop) | | Push class variable by name (not yet implemented) |
| 0xF4 | `store_classVar_pop_name` | BO | classVar_access | `name` | ✓ | Store to class var by name (not yet implemented) |
| 0xF5 | `store_classVar_name` | BO | classVar_access | `name` | | Store to class var by name (not yet implemented) |
| 0xF6 | `smi_and` | BOO | message_send | `sel, cache` | | Predicted bitwise `bitAnd:` |
| 0xF7 | `smi_or` | BOO | message_send | `sel, cache` | | Predicted bitwise `bitOr:` |
| 0xF8 | `smi_xor` | BOO | message_send | `sel, cache` | | Predicted bitwise `bitXor:` |
| 0xF9 | `smi_shift` | BOO | message_send | `sel, cache` | | Predicted bitwise shift |
| 0xFA | `unimplemented_fa` | UNDEF | — | — | | Reserved |
| 0xFB | `unimplemented_fb` | UNDEF | — | — | | Reserved |
| 0xFC | `unimplemented_fc` | UNDEF | — | — | | Reserved |
| 0xFD | `unimplemented_fd` | UNDEF | — | — | | Reserved |
| 0xFE | `unimplemented_fe` | UNDEF | — | — | | Reserved |
| 0xFF | `halt` | B | control_struc | — | | Halt the interpreter (invalid/marker) |

---

## 5. Send Bytecodes

### 5.1 Naming convention

```
<send_type>_send_<argument_specification>[_pop]
```

| `send_type`     | Meaning                                                    |
| --------------- | ---------------------------------------------------------- |
| `interpreted`   | Monomorphic send to an **interpreted** method              |
| `compiled`      | Monomorphic send to a **compiled** method (nmethod)        |
| `polymorphic`   | Polymorphic inline cache (PIC) send                        |
| `megamorphic`   | Global lookup-cache send (no local cache)                  |
| `predicted`     | Predicted send (SMI arithmetic, `at:`, identity)           |
| `accessor`      | Predicted accessor send (instVar get/set)                  |
| `primitive`     | Send to a method whose body is a single primitive          |

| `argument_specification` | Stack contents                                          |
| ------------------------ | ------------------------------------------------------- |
| `0`                      | receiver + 0 arguments                                   |
| `1`                      | receiver + 1 argument                                    |
| `2`                      | receiver + 2 arguments                                   |
| `n`                      | receiver + n arguments (n in following byte)             |
| `self`                   | arguments only (implicit `self` receiver)                |
| `super`                  | arguments only (implicit dynamic superclass receiver)    |

A trailing `_pop` means "send, but discard the result" (used for statement
expressions whose value is not consumed).

### 5.2 Inline cache layouts

**Interpreted / compiled / primitive / accessor sends** (monomorphic):

```
[send byte code]     1 byte
[no. of args]        1 byte   (only when arg_spec == n)
alignment            0..3 bytes (0xFF padding to word boundary)
[selector/method]    1 word   (symbolOop, or methodOop / nmethod entry)
[0/class]            1 word   (0 = uninitialized, or klassOop of receiver)
next instr ...      
```

**Polymorphic sends** (PIC):

```
[send byte code]     1 byte
[no. of args]        1 byte   (only when arg_spec == n)
alignment            0..3 bytes
[selector]           1 word   (symbolOop)
[pic]                1 word   (objArrayOop: [length, klass₁, method₁, klass₂, method₂, ...])
next instr ...
```

**Megamorphic sends** (global lookup caches):

```
[send byte code]     1 byte
[no. of args]        1 byte   (only when arg_spec == n)
alignment            0..3 bytes
[selector]           1 word   (symbolOop)
[0/class]            1 word   (unused cache slot)
next instr ...
```

Megamorphic sends probe the global primary/secondary lookup caches in
`vm/lookup/lookupCache.cpp`.

**Compiled sends** store the **nmethod entry** plus the selector:

```
[send byte code]     1 byte
[no. of args]        1 byte   (only when arg_spec == n)
alignment            0..3 bytes
[nmethod entry]      1 word   (code address)
[selector]           1 word   (symbolOop)
next instr ...
```

### 5.3 Inline-cache transitions

On a cache miss, bytecodes *transition* to a more general form by patching the
bytecode in place (via `interpreted_send_code_for` / `compiled_send_code_for`
and friends in `bytecodes.cpp`).

```
predicted_send  (smi_add, smi_equal, objArray_at, ..., special_primitive_send_1_hint)
        |
        |  receiver is not a SmallInteger (or not the predicted shape)
        v
interpreted_send_1
        |
        |-- target is compiled ------------------------------> compiled_send_1
        |-- target is a primitive method --------------------> primitive_send_1
        |-- > 2 distinct receiver classes seen ------------> polymorphic_send_1
        |                                                        |
        |                                                        | > 8 entries
        |                                                        v
        |                                                   megamorphic_send_1
        v
accessor_send  (access_send_0, access_send_self)
        |
        |  cache miss
        v
interpreted_send_0 / interpreted_send_self
```

All 20 predicted sends (`smi_*`, `objArray_*`, `double_equal`, `double_tilde`)
degrade to `interpreted_send_1` on their first miss. The transition functions
map any send variant to the canonical `interpreted_send_*` form first, then to
the target form.

---

## 6. Interpreter Architecture

The interpreter is **generated at startup** as native machine code by
`InterpreterGenerator` (`vm/interpreter/interpreter.cpp`) using
`MacroAssembler`. There is no `switch` on opcodes: each bytecode is a generated
code fragment that runs and then jumps directly to the next handler — classic
*threaded code* dispatch through a 256-entry function-pointer table.

### 6.1 Dispatch table

```cpp
extern "C" doFn dispatch_table[Bytecodes::number_of_codes];  // patched (step mode)
extern "C" doFn original_table[Bytecodes::number_of_codes];  // canonical entries
```

- `Bytecodes::set_entry_point()` fills both `original_table` and the internal
  `_entry_point` array as handlers are generated.
- `dispatchTable::reset()` restores `dispatch_table` from `original_table`.
- `dispatchTable::patch_with_sst_stub()` replaces entries with the
  single-step stub for debugging (all single-step bytecodes; control
  structures are exempt).

Dispatch modes (`dispatchTable::Mode`): `normal_mode`, `step_mode`
(single-step), `next_mode` (step-over, break on next frame), `return_mode`
(break on return).

### 6.2 Register conventions (x86-64)

| Register | Role                                             |
| -------- | ------------------------------------------------ |
| `eax`    | Top of expression stack (TOS), kept in register  |
| `ebx`    | Current opcode byte                              |
| `esi`    | Bytecode pointer (HP = hardware instruction pointer) |
| `edi`    | Dispatch target address / inline-cache scratch   |
| `ebp`    | Frame pointer                                    |
| `edx`/`ecx` | Scratch registers                             |

On AArch64 the backend uses the corresponding register mapping (see
`vm/asm/mapping_aarch64.hpp`); the dispatch mechanism — load opcode, index the
table, thread to the next handler — is identical.

### 6.3 Generation order

`generate_all()` (interpreter.cpp:3981) emits, in order:

1. Stub routines (`restart_primitiveValue`, ...)
2. Float operation handlers (from `Floats::Function` table)
3. Error handler code
4. Non-local return code
5. Method entry code (invocation counter, stack check)
6. Inline-cache miss handler
7. Predicted SMI-send failure handler
8. Redo-send code
9. Deoptimized-return code
10. `primitiveValue` 0–9 stubs
11. Individual bytecode handlers (loop over all `number_of_codes`)

Each handler returns a `char*` entry point, registered via
`Bytecodes::set_entry_point()`.

### 6.4 Key instruction patterns

```
load_ebx()   ; ebx <- byte[esi]                  -- read current opcode
next_ebx()   ; ebx <- byte[esi+1]; esi++         -- advance + read (B-format)
jump_ebx()   ; jump dispatch_table[ebx]          -- thread to next handler
load_edi()   ; edi <- dispatch_table[ebx]
jump_edi()   ; jump edi                          -- thread via edi
```

`next_ebx` is used by B-format (operand-less) bytecodes that can advance a
single byte; bytecodes with operands compute the next instruction pointer by
skipping their operands and then do `load_ebx`/`jump_ebx`.

---

## 7. Float operations

Float bytecodes operate on a set of **float registers** allocated by
`float_allocate` (`nofFloats` of them, plus `nofTemps` expression temp slots,
plus `floatExprStack` oop-expression slots). The `function` operand is a
`Floats::Function` code (see `vm/interpreter/floats.hpp`):

```c
enum Function {
    // nullary  (0 floats -> 1 float)
    zero,  one,
    // unary   (1 float  -> 1 float)
    abs, negated, squared, sqrt, sin, cos, tan, exp, ln,
    // binary  (2 floats -> 1 float)
    add, subtract, multiply, divide, modulo,
    // unary  -> oop
    is_zero, is_not_zero, oopify,
    // binary -> oop
    is_equal, is_not_equal, is_less, is_less_equal,
    is_greater, is_greater_equal,
};
```

The `function` byte operand names a handler in `Floats::_function_table`
(`float_op()` indexes it). The source floats for unary/binary ops are the
registers `floatNum` and `floatNum − 1` (binary ops read both, with
`floatNum − 1` pushed first); the result is stored back to `floatNum`, or
pushed as an oop on the expression stack by the `to_oop` variants (whose
`function` is one of `is_zero` … `is_greater_equal`, or `oopify` for
float→doubleOop conversion).

---

## 8. HCodeBuffer: how bytecodes are emitted

The Smalltalk-side compiler writes bytecodes via `HCodeBuffer`
(`vm/interpreter/hCodeBuffer.hpp`, `hCodeBuffer.cpp`):

```cpp
class HCodeBuffer : public ResourceObj {
  GrowableArray<uintptr_t>* _bytes;
  GrowableArray<oop>*       _oops;
public:
  void pushByte(unsigned char op);
  void pushOop(oop arg);
  byteArrayOop bytes();      // final code byte array
  objArrayOop  oops();       // oop array (for GC)
};
```

- `pushByte(op)`: if the buffer is already word-aligned *before* the push, a
  `0` placeholder is appended to `_oops` (marking "no oop at this aligned
  slot"); then the byte is appended.
- `pushOop(arg)`: pads with `0xFF` bytes to the word boundary, reserves
  `oopSize` zero bytes in `_bytes`, and records the oop itself in `_oops`.
- The `0xFF` padding bytes are `halt` opcodes — a mis-decoded stream halts
  safely instead of executing garbage.

The compiled `bytes()` array plus `oops()` table are the two halves of a
method's bytecode section; oop operands (literals, selectors, caches) live by
index into `oops()`.

---

## 9. Developer guide: adding or modifying a bytecode

### 9.1 Add an enum value

`vm/interpreter/bytecodes.hpp` — declare the code in the `enum Code` (the
numeric position is implied by declaration order; reserved gaps are skipped via
`unimplemented_*` entries).

### 9.2 Register metadata with `def()`

`vm/interpreter/bytecodes.cpp`, inside `Bytecodes::init()`. There are four
overloads:

```cpp
def(code);                                        // undefined/reserved slot
def(code, name, format, codeType, single_step, pop_tos);
def(code, name, format, argSpec, sendType, pop_tos);   // → message_send
def(code, name, format, codeType, single_step, argSpec, sendType, pop_tos);
```

Assertions (in `#ifdef ASSERT`) enforce the naming conventions:
- `pop_tos` codes must not start with `push_` (and should end `_pop`).
- `argument_spec != no_args` implies a real `send_type`.
- `control_struc` codes must not be single-stepped.
- `float_operation` codes must start with `float_`.

### 9.3 Generate the handler

`vm/interpreter/interpreter.cpp`, `InterpreterGenerator` — add a method that
returns the handler's entry point: generate the operation, then compute the
next instruction pointer (skipping the opcode's operands) and `jump_ebx()` /
`jump_edi()`. Wire it into `generate_all()` and register it with
`Bytecodes::set_entry_point(code, entry)`.

For a **send**, the handler must also install the inline cache layout from §5.2
and include the transition/miss paths from §5.3 (share `icmiss_handler`, SMI
miss handler, etc.).

### 9.4 Compiler support (optional)

If the new bytecode is to be *generated* by the compiler:
- `vm/compiler/...` — the compiler emits bytecodes through `HCodeBuffer`
  (§8); the Smalltalk-side HCode compiler (source in `StrongtalkSource/`) is
  generated from `generate_HCode_methods()` (`bytecodes.cpp`, `+GenerateHCode`)
  which emits one `gen_<name>` method per defined bytecode.
- `generate_codeForPrimitive_method()` lists which primitives map to inline
  primitive-call bytecodes.

### 9.5 HTML doc refresh

Run the debug VM with `+GenerateHTML` (see §10) to regenerate the bytecode
table HTML; the `GenerateHTML` path lives under `#ifndef PRODUCT` and exits
after writing the table.

### 9.6 Not yet implemented

The following codes are **defined but return `NULL` from the generator**
(they are legal as bytecode-table entries but cannot be dispatched):
`return_instVar_name`, `push_instVar_name`, `store_instVar_pop_name`,
`store_instVar_name`, `push_classVar_name`, `store_classVar_pop_name`,
`store_classVar_name`, `smi_div`, `smi_mod`, `smi_create_point`,
`objArray_at`, `objArray_at_put`. Implement them by filling in the generator
case + any primitive glue.

---

## 10. Regenerating the HTML reference

`bytecodes.cpp` ships a `generate_HTML_docu()` path (under `#ifndef PRODUCT`)
that emits the bytecode table as HTML. The Makefile target rebuilds the VM if
needed and regenerates the reference:

```sh
make docs
```

(The underlying command is the debug VM invoked with `+GenerateHTML`,
redirected to `documentation/internal/vm/bytecodes.html`; it works on all build
configs — arm64/x86-64 macOS and Docker/Linux.)

The comment `<!-- do not modify - use delta +GenerateHTML to generate -->` in
the output marks it as generated. The generated tables are grouped by
`CodeType`; send tables add the "Send type" and "Arguments" columns.

---

## Reference

- `vm/interpreter/bytecodes.hpp` / `.cpp` — opcode definitions, formats, transitions
- `vm/interpreter/interpreter.cpp` — generated handlers, register conventions
- `vm/interpreter/dispatchTable.hpp` / `.cpp` — dispatch table, step modes
- `vm/interpreter/hCodeBuffer.hpp` / `.cpp` — emission buffer
- `vm/interpreter/floats.hpp` — float function codes
- `vm/interpreter/codeIterator.hpp` — bytecode scanning for the framework
- `vm/lookup/lookupCache.cpp` — megamorphic send lookup caches
- `documentation/internal/vm/bctable.pdf` — original Digitalk bytecode table