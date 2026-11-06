# Contract: The DBC Subset and Its Validation

**Feature**: `006-dbc-signal-decoder` | **Spec**: [../spec.md](../spec.md) sections 6 and 7

This is the authoritative statement of what `tcu_dbc` accepts, what it ignores and what it rejects.
A file is either fully loaded or fully rejected; there is no partial database (spec section 6.3).

## 1. Accepted grammar

```text
file        := statement*
statement   := version | ns_block | bs_decl | nodes | message | signal | value_desc | ignorable
version     := 'VERSION' string
ns_block    := 'NS_' ':' (identifier)*            # content not interpreted
bs_decl     := 'BS_' ':'                          # optional baudrate section, content ignored
nodes       := 'BU_' ':' identifier*
message     := 'BO_' uint name ':' uint identifier
signal      := 'SG_' name [mux_ind] ':' uint '|' uint '@' ('0'|'1') ('+'|'-')
                  '(' number ',' number ')' '[' number '|' number ']' string identifier_list
mux_ind     := 'M' | 'm' uint
value_desc  := 'VAL_' uint name (int string)* ';'
```

- Signal statements belong to the most recent `BO_`; an `SG_` before any `BO_` is `DBC_SYNTAX`.
- `@1` is Intel / little-endian, `@0` is Motorola / big-endian. `+` unsigned, `-` signed.
- CAN ids in `BO_` and `VAL_` are decimal. Bit 31 set means an extended identifier.
- Encoding: UTF-8 or ASCII. A NUL byte anywhere, or an invalid UTF-8 sequence, is `DBC_SYNTAX`.
- Line endings LF or CRLF. Column numbers in diagnostics are 1-based over bytes.

## 2. Ignorable statements (allowlist, research R-02)

`CM_`, `BA_DEF_`, `BA_DEF_DEF_`, `BA_`, `VAL_TABLE_`, `SIG_GROUP_`, `EV_`.

Each is skipped to its terminating `;` or end of line, counted in `CompiledDbc::stats`, and reported
once per kind as a `Severity::Warning` diagnostic. No attribute value is ever interpreted — in
particular `GenMsgCycleTime` does **not** become a timeout (research R-03).

## 3. Rejected constructs

| Construct | Code | Why it cannot be ignored |
|---|---|---|
| `SIG_VALTYPE_` | `UNSUPPORTED_FEATURE` | changes the meaning of bits a supported `SG_` already covers |
| `SG_MUL_VAL_` | `UNSUPPORTED_FEATURE` | extended/nested multiplexing changes which signals are active |
| `BO_TX_BU_` | `UNSUPPORTED_FEATURE` | transmitter rewriting is out of scope and implies behaviour |
| unknown token at statement position | `DBC_SYNTAX` | may be decode-affecting; the allowlist is the only exemption |
| `VAL_` naming a value table instead of inline pairs | `DBC_ENUM` | V1 resolves no table references |

## 4. Validation checks

Spec section 6.2's eleven classes, with the data-model invariant that implements each.

| Class | Check | Code | Invariant |
|---|---|---|---|
| File | size over `max_dbc_bytes`, bad encoding, NUL byte, unreadable, not a regular file | `DBC_LIMIT`, `DBC_SYNTAX`, `DBC_SOURCE` | D-12, R-08 |
| Message | id out of range for its format, reserved bits set, duplicate `(bus, format, id)`, duplicate name, `length > 8` | `DBC_MESSAGE` | D-04, D-05 |
| Signal | duplicate name in message, `width == 0` or `width > 64`, malformed start bit, non-finite factor/offset | `DBC_SIGNAL`, `DBC_RANGE` | D-06, D-07, D-10 |
| Bit layout | any bit of the signal outside `length * 8` | `DBC_OUT_OF_BOUNDS` | D-07 |
| Overlap | two co-active signals sharing a bit | `DBC_OVERLAP` | D-09 |
| Multiplexing | more than one selector, branch without selector, duplicate branch value | `DBC_MUX` | D-08 |
| Enumerations | duplicate number or label within one signal | `DBC_ENUM` | D-11 |
| Min/max | `min > max`, non-finite bound, invalid unit encoding | `DBC_RANGE` | D-10 |
| Resource | message, signal, token-length or statement-count limit exceeded | `DBC_LIMIT` | D-12, D-13 |
| Attributes | a rejected construct from section 3 | `UNSUPPORTED_FEATURE` | R-02 |
| Consistency | `VAL_` referencing an unknown message or signal | `DBC_ENUM` | D-11 |

**Overlap nuance** (spec section 6.2): branches `m1` and `m2` may share bits; two signals in the
same branch may not; an unconditional signal may not overlap anything. B-18 and B-19 are the tests.

**Range nuance** (spec section 6.2, research R-04): `[min|max]` is retained as metadata. It becomes
a validity criterion only for signals listed in `range_enforcement`. `[0|0]` is never read as
"only zero is legal".

## 5. Diagnostics

```text
virtual_tcu.dbc:42:3: DBC_OVERLAP: signal 'MotorTemp' shares bits 16..31 with 'BatteryVoltage' in branch m1
virtual_tcu.dbc:51:24: DBC_RANGE: factor is not finite
virtual_tcu.dbc:7:1: UNSUPPORTED_FEATURE: SIG_VALTYPE_ is not supported in v1
```

- Every error diagnostic has file, line, column, code and message, in that order (FR-15, D-01).
- `loadInitial` and `reload` return **all** diagnostics for the file, not only the first, so one
  `dbc_validate` run fixes many problems (spec section 6.3).
- A message never quotes more than 64 characters of input and never quotes payload bytes (D-02).

## 6. Bit extraction contract

The reference loop of spec section 7.1 and the sign extension of section 7.2 are normative. Two
restatements, because they are the off-by-one traps:

- Intel: raw bit `i` comes from payload bit `start + i`, counting bit 0 as the LSB of byte 0.
- Motorola: reading begins at `start`, which is the signal's **MSB**, and the next position after a
  position `p` with `p % 8 == 0` is `p + 15`; otherwise it is `p - 1`.

Position validity is checked against `frame.size * 8` **before** each access (P-02). The
hand-computed vectors of spec section 7.3 are implemented verbatim as the first six unit tests of
`bit_extractor_test.cpp`, before any generated vector is used.

## 7. Not in v1

CAN FD lengths and the FD DLC table; float signals; extended and nested multiplexing; value table
references; attribute interpretation of any kind; J1939 PGN decomposition; encoding (signal to
frame) in any direction; writing or rewriting a DBC file.
