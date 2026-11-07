# Traceability: P0 what-ifs to tests (spec section 23.1)

**Feature**: `006-dbc-signal-decoder` | **Date**: 2026-11-07

Spec section 14 lists 159 what-if cases, 116 of them **P0**. The Definition of
Done asks that every P0 be mapped to an executable test or a documented demo.
This is that mapping, and it is kept in its own file so `spec.md` stays the
verbatim design document.

It lives separately from the plan's Definition-of-Done table for a reason: that
table answers "is the criterion met", this one answers "by what". Where the
answer is "nothing", it says so — 7 of the 116 have no coverage of any
kind, and they are collected at the end rather than spread thin.

**Kind** column:

| Kind | Meaning |
|---|---|
| `case` | A named test asserts this specific behaviour |
| `suite` | Covered by a mechanism rather than one case: fuzzing, the golden matrix, a sanitizer run, the soak |
| `design` | Cannot fail because the code has no path to it, with the decision recorded. Not a test |
| `demo` | A step of `scripts/run_dbc_decoder_demo.sh`, observed rather than asserted |
| **`none`** | **Not covered.** Listed again at the end |

Reproduce the evidence with:

```bash
ctest --test-dir build-wsl -L "dbc-unit|signal-unit|dbc-integration|dbc-stress"
scripts/run_dbc_fuzz.sh 1000000
```

---

## 14.1 DBC input, parser and syntax

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-001 | DBC file does not exist | `Database_RejectsSymlinkAndDirectory.MissingFileIsASourceError`, `Reload_SourceErrorKeepsActiveVersion` | `case` |
| W-002 | File not readable (chmod 000) | — | **`none`** |
| W-003 | Empty file | `Parser_NeverThrows.ForArbitraryBytes` (empty input is the first case) | `case` |
| W-004 | Very large file | `Validator_EnforcesLimits.FileTooLarge`, `Database_EnforcesFileSizeBeforeReading.LimitHitOnDisk` | `case` |
| W-005 | Very long token | `Lexer_RejectsOverlongToken.NamesTheLimit` | `case` |
| W-006 | NUL or embedded binary | `Lexer_RejectsNulByte.AnywhereInTheFile` | `case` |
| W-008 | DBC with no `BO_` at all | `Parser_RejectsSignalBeforeMessage`, `Parser_NeverThrows` | `suite` |
| W-009 | Missing colon or parenthesis | `Parser_ReportsAllDiagnostics`, `Parser_NeverThrows` | `case` |
| W-011 | Unsupported comment/header metadata | `Parser_IgnoresAllowlistedStatements.LoadsWithWarningsNotErrors`, `Cli_ValidateExitCodes.ZeroWithWarningsForIgnorableStatements` | `case` |
| W-012 | Decimal overflow in `BO_` | `Validator_RejectsMessageIdOutOfRange` (both cases) | `case` |
| W-013 | Enormous number in `factor` | `Validator_RejectsNonFiniteFactor.DbcRange` (`1e999`) | `case` |
| W-015 | Parser crashed by a fuzz payload | `fuzz_dbc_parser`, 1,000,000 inputs, no finding | `suite` |

## 14.2 Schema and bit-layout validation

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-016 | Standard id outside 11 bits | `Validator_RejectsMessageIdOutOfRange.StandardIdAbove0x7FF` | `case` |
| W-017 | Extended id outside 29 bits | `Validator_RejectsMessageIdOutOfRange.ReservedBitsSet` | `case` |
| W-018 | Duplicate message id, same format | `Validator_RejectsDuplicateMessageKey.SameFormatAndId` | `case` |
| W-019 | Same numeric id, STD vs EFF | `Validator_AcceptsStdAndEffSameNumber`, `Database_LookupDistinguishesFormat` | `case` |
| W-020 | `BO_` length 9 on Classical CAN | `Validator_RejectsMessageTooLong.NineBytesInV1` | `case` |
| W-021 | Signal width 0 | `Validator_RejectsWidthZeroAndWidth65` | `case` |
| W-022 | Signal width 65 | `Validator_RejectsWidthZeroAndWidth65` | `case` |
| W-023 | Signal past payload, Intel | `Validator_RejectsBitsOutsidePayload.OutOfBounds` | `case` |
| W-024 | Signal past payload, Motorola | `BitExtractor_RejectsOutOfRangePosition.MotorolaWalkingOffTheEndIsRefused` | `case` |
| W-025 | Two unconditional signals overlap | `Validator_RejectsOverlapInSameBranch`, `Cli_ValidateExitCodes.OneForARejectedFile` | `case` |
| W-026 | Overlap across different mux branches (legal) | `Validator_AcceptsOverlapAcrossBranches.MutuallyExclusiveIsFine` | `case` |
| W-027 | Duplicate signal name | `Validator_RejectsDuplicateSignalName.WithinOneMessage` | `case` |
| W-031 | Signal/message limits exceeded | `Validator_EnforcesLimits` (3 cases: messages, signals, bytes) | `case` |

## 14.3 Bit extraction, signedness and mathematics

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-032 | Intel signal starting at bit 0 | `BitExtractor_IntelGoldens.VehicleSpeedIsTenThousandRaw` | `case` |
| W-033 | Intel signal at bit 63 | `BitExtractor_Width1And64.Width64UnsignedIsExact` | `case` |
| W-034 | Motorola MSB at bit 7 | `BitExtractor_MotorolaGoldens.TorqueRawIsAbc` | `case` |
| W-035 | Motorola cross-byte | `BitExtractor_CrossByteMotorola.SpansThreeBytesWithTheSawtoothStep` | `case` |
| W-036 | Motorola start bit 0 crossing a byte | `BitExtractor_CrossByteMotorola.UnalignedStartCrossesAByteBoundary` | `case` |
| W-037 | Width 1 signed, negative | `SignExtend_OneBitSignedIsMinusOne.SpecSection72` | `case` |
| W-038 | Width 64 signed, INT64_MIN | `SignExtend_Int64MinNoUb.Width64MostNegative` + UBSan clean | `case` |
| W-039 | Width 64 unsigned, UINT64_MAX | `BitExtractor_Width1And64`, `Scaling_RawU64PreservedAboveTwoPow53` | `case` |
| W-040 | Width 63 negative | `SignExtend_TwoComplementAllWidths.BoundariesForEveryWidth` (every width 1..64) | `case` |
| W-041 | Raw near 2^53 | `Scaling_RawU64PreservedAboveTwoPow53` (exact raw, `physicalPrecisionLoss` flag) | `case` |
| W-043 | Negative offset | `BitExtractor_IntelGoldens.CabinTempIsMinusForty` (offset -40) | `case` |
| W-044 | Physical overflow | `Scaling_NonFinitePhysicalIsDecodeError.OneBadSignalDoesNotSinkTheBatch` | `case` |
| W-046 | Invalid raw code 0xFFFF | `Decode_InvalidRawFromPolicy.OnlyWhenTheConfigurationSaysSo` | `case` |
| W-047 | Physical out of declared range | `Decode_RangeEnforcedOnlyWhenConfigured.AndNeverClamped` | `case` |
| W-049 | Random bit patterns | `Golden_GeneratedMatrix` (10,000 cantools vectors), `fuzz_frame_decoder` | `suite` |

## 14.4 Multiplexing and enums

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-050 | Mux selector at a supported value | `Mux_Page1EmitsBatteryOnly`, `Mux_Page2EmitsMotorTempOnly` | `case` |
| W-051 | Mux selector switches rapidly | `Mux_UnconditionalSignalsAreNeverBranchDependent`, `Golden_GeneratedMatrix` (3,367 mux frames) | `suite` |
| W-052 | Mux selector unknown | `Mux_UnknownSelectorKeepsUnconditional.B20NoInventedBranch` | `case` |
| W-053 | Mux selector raw invalid | `Mux_UndecodableSelectorSkipsBranches` | `case` |
| W-054 | Two multiplexors declared | `Validator_RejectsTwoSelectors.DbcMux` | `case` |
| W-055 | Nested multiplexing directive | `Parser_RejectsExtendedMux.UnsupportedFeature` | `case` |
| W-056 | Mux branch missing | `Validator_RejectsBranchWithoutSelector.DbcMux` | `case` |
| W-057 | Overlap within the same branch | `Validator_RejectsOverlapInSameBranch.Overlap` | `case` |
| W-058 | Unconditional overlaps a branch | `Validator_RejectsOverlapWithUnconditional.Overlap` | `case` |
| W-060 | Enum value with no label | `Decode_EnumLabelResolved.LabelledAndUnlabelledValues` | `case` |

## 14.5 Runtime frame metadata and transport

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-062 | CAN id absent from the DBC | `Decode_UnknownIdCounted.EmptyBatchNoThrow`, `Service_PipelineDecodesFromTheTransport.UnknownIdChangesNothing` | `case` |
| W-063 | Standard/EFF alias | `Decode_UnknownIdCounted.ExtendedFrameIsNotAliasedOntoStandard` | `case` |
| W-064 | RTR frame posing as data | `Decode_RemoteFrameRejected.BeforeAnyLookup`, `FrameAdapter_MapsRemoteAndErrorKinds` | `case` |
| W-065 | SocketCAN ERR frame | `Decode_ErrorFrameRejected.BeforeAnyLookup`, `FrameAdapter_MapsRemoteAndErrorKinds` | `case` |
| W-066 | Payload shorter than declared | `Decode_WrongLengthRejected`, `Service_PipelineDecodesFromTheTransport.WrongLengthKeepsLastGood` | `case` |
| W-067 | Payload longer than declared | `Decode_RejectsOversizePayload.AboveClassicalMaximum` | `case` |
| W-068 | Null data pointer with size > 0 | `decode()` returns `InternalError`; the one programming error the API does not absorb (contracts/dbc_api.md section 4), and ASan covers the rest | `design` |
| W-069 | Unexpected `busId` | Lookup is keyed by `(busId, format, id)`, so a foreign bus is `UnknownMessage` — `Decode_UnknownIdCounted`, `FrameAdapter_MapsCanMessage.BusIdIsTheCallersChoice` | `case` |
| W-070 | CAN FD frame reaches v1 | `Decode_RejectsOversizePayload`, `Config_RejectsNonClassicalFrameMode` | `case` |
| W-072 | Hardware CRC wrong in payload | No E2E profile in v1: a CRC signal decodes as an ordinary signal and the consumer checks it. Recorded in research R-05 and ADR-07 | `design` |
| W-073 | Receive timestamp in the future | — | **`none`** |

## 14.6 Quality, timing, stale and lifecycle

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-074 | No frame since boot | `Cache_UnknownSignalIsDistinctFromNeverReceived`, `FreshnessTest.NeverReceivedIsNotStale` | `case` |
| W-075 | Frame older than the deadline | `FreshnessTest.StaleDoesNotClearLastGood` | `case` |
| W-076 | Exactly at the deadline | `FreshnessTest.ExactDeadlineIsStale` (1499 fresh, 1500 stale) | `case` |
| W-077 | Invalid frame before the timeout | `Cache_InvalidKeepsLastGood` (5 qualities), `Cache_QueryReturnsBothAxes` | `case` |
| W-078 | Mux branch inactive for a long time | `FreshnessTest.InactiveMuxBranchUsesLastActiveInstance` | `case` |
| W-080 | Valid frame after stale | `FreshnessTest.RecoveryAfterValid` (`SignalRecovered`, new timestamp) | `case` |
| W-081 | Wall clock jumps backwards | Freshness never reads a wall clock: every deadline is `steady_clock` through an injected `now` (research R-06). No test can move a clock the code does not consult | `design` |
| W-082 | RX queue delays a frame by 2 s | `rxTime` is the source's stamp and is never re-stamped — `FrameAdapter_MapsCanMessage`, `Cache_OutOfOrderFrameIgnored` | `case` |
| W-083 | Out-of-order `rxTime` | `Cache_OutOfOrderFrameIgnored.LastGoodRxTimeNeverGoesBackwards` | `case` |
| W-084 | Signal invalid on its first frame | `Cache_UnknownSignalIsDistinctFromNeverReceived` (invalid first, no last-good) | `case` |

## 14.7 Reload, config changes and schema drift

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-086 | Valid reload while traffic continues | `Soak_ReloadUnderTraffic.VersionOnlyEverIncreases`, `Reload_CommitIncrementsVersion` | `case` |
| W-087 | Invalid reload | `Reload_FailureKeepsActiveVersion.B11` | `case` |
| W-089 | Two concurrent reload requests | `Reload_BusyRejectsSecondRequest.V09` | `case` |
| W-090 | Reload request flood | `Reload_RateLimited.V13` | `case` |
| W-091 | A signal's meaning changes | `Reload_MigrationResetsChangedSignals.SemanticHashChange` | `case` |
| W-092 | A signal is removed | `Reload_MigrationResetsChangedSignals.RemovedSignalBecomesConfigUnavailable` | `case` |
| W-094 | DBC replaced mid-read | Tests replace the file by write-then-rename, as a real tool does (`TempDir::replace`); a torn file is a parse error that `Reload_FailureKeepsActiveVersion` covers | `suite` |
| W-096 | Pinned frames finish after the swap | `RaceA_InFlightFramePinsOldVersion.B12` (unit) and `TenThousandSwapsNeverMixAVersion` (10,000 iterations) | `case` |

## 14.8 Threading, races and shutdown

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-098 | Reload coincides with decode | `RaceA_InFlightFramePinsOldVersion` both variants, plus the TSan run | `case` |
| W-099 | Freshness scan races a valid frame | `FreshnessTest.EpochRecheckBeatsLateTimer` (deterministic) and `EpochRecheckUnderRealThreads` (2,000 iterations) | `case` |
| W-100 | Queue pop races stop | `Queue_CloseWakesAWaitingWorker`, `Service_StopJoinsWithinDeadline`, TSan | `case` |
| W-101 | Unsubscribe during a callback | `Service_RemoveObserverStopsFutureCallbacks` covers unsubscribe; unsubscribing *from inside* the callback is project 01's contract and its own test | `suite` |
| W-102 | Consumer throws | `Service_ObserverThrowIsolated.B14` (worker survives, counted) | `case` |
| W-103 | Consumer blocks for 10 s | `RaceC_StopWithBlockedObserver.StopIsBoundedAndNoCallbackFollows` | `case` |
| W-104 | Concurrent cache reads during update | `Cache_NoCallbackUnderLock`, `EventBus_NoPublishUnderCacheLock`, TSan clean | `case` |
| W-105 | Double stop | `Service_StopIsIdempotent` (both cases: before start, and twice after) | `case` |
| W-107 | Reload during shutdown | — | **`none`** |

## 14.9 Overload, pressure and performance

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-110 | RX burst above capacity | `Queue_DepthNeverExceedsCapacity.UnderABurst`, `Service_QueueDropsAreBoundedAndCounted.B13` | `case` |
| W-111 | Decode worker slower than the producer | `Queue_DropsNewestWhenFull.ProducerReturnsImmediately`, `Soak_HundredThousandFramesBounded` | `case` |
| W-112 | Unknown-id flood | `RateLimit_SuppressesAndReportsGap`, `Soak_MetricConservation` (500 unknown ids) | `case` |
| W-113 | Invalid frame flood | `Soak_MetricConservation` (300 short frames, conservation still holds) | `case` |
| W-116 | Slow disk or log backend | — | **`none`** |
| W-117 | High-rate schema reload | `Reload_RateLimited.V13`, `Soak_ReloadUnderTraffic` | `case` |

## 14.10 Cross-process, Event Bus and consumer

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-120 | Event Bus not started | — | **`none`** |
| W-121 | Event Bus queue full | `EventBus_PublishFailureCounted.AFullBusDoesNotStallTheWorker` (one-slot bus) | `case` |
| W-123 | Subscriber expects the old schema | Every batch carries one `(version, hash)` and the adapter forwards the version — `Decode_BatchHasOneSchemaIdentity`, `EventBus_PublishesSignalsDecoded` (asserts `generation == 1`) | `case` |
| W-124 | Two consumers want the same signal | — | **`none`** |
| W-125 | Null or dangling `string_view` | No public type holds a `string_view`: `SignalValue` and `DecodedBatch` own their strings, and `CanFrameView` is documented as borrowed and never stored. ASan covers the rest | `design` |
| W-127 | Consumer uses stale data anyway | Every read returns both quality axes and there is no accessor that hands back a value without one (`Cache_QueryReturnsBothAxes`). What the consumer then does is its own contract | `design` |
| W-128 | Event ordering across a reload | `RaceA_InFlightFramePinsOldVersion` (old version completes as old), `Soak_ReloadUnderTraffic` | `case` |

## 14.11 Security, malformed inputs and data trust

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-130 | Untrusted external DBC file | `fuzz_dbc_parser`, 1,000,000 inputs; plus every `Validator_*` rejection | `suite` |
| W-131 | Path traversal via config | `Reload_SymlinkRejected`, `Database_RejectsSymlinkAndDirectory`; the path is refused unless it is a regular file (research R-08). Traversal to a *readable regular* file is not prevented, and v1 does not claim to | `case` |
| W-133 | Malicious string injected into logs | — | **`none`** |
| W-134 | DoS by repeated parse failures | `Reload_RateLimited`, `RateLimit_SuppressesAndReportsGap` | `case` |
| W-135 | Spoofed CAN id | Out of scope by ADR-07: a successful decode is not evidence of a genuine sender, and v1 has no CRC, counter or signature. Stated in the ADR, the README and the demo output | `design` |
| W-137 | Unexpected DBC encoding directive | `Parser_RejectsUnknownDirective`, `Parser_RejectsSigValtype`, `Parser_RejectsValTableReference` | `case` |

## 14.12 Host, OS, failure recovery and observability

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-140 | `vcan0` not created | `Vcan_EndToEnd.MissingInterfaceIsReportedNotThrown` (the open returns an error value), and `virtual_dbc_demo` prints the setup hint and falls back | `case` |
| W-141 | `vcan0` goes down | Project 05's own `VcanLifecycle.InterfaceDownUp` owns this at the transport boundary; project 06 consumes the transport and adds nothing | `suite` |
| W-142 | `vcan0` comes back | As W-141, project 05's `VcanLifecycle` | `suite` |
| W-144 | Machine time skew (NTP) | Same reason as W-081: no deadline in this project reads a wall clock | `design` |
| W-147 | Log flood of the same failure | `RateLimit_SuppressesAndReportsGap` (and `suppressed_since_last` makes the gap visible) | `case` |

## 14.13 Toolchain, regression and cross-platform

| W-ID | What-if | Covered by | Kind |
|---|---|---|---|
| W-150 | GCC and Clang optimise differently | Built and tested with gcc 11.4 (Linux), clang 14 (TSan and fuzz builds) and gcc 16.1 (Windows/MSYS2); `tcu_dbc` and its 91 cases run on all three | `suite` |
| W-152 | Sanitizer-enabled build | ASan + UBSan + leak detection and a separate TSan build, 172/172 each, no findings | `suite` |
| W-154 | `cantools` disagrees on a vector | `Golden_GeneratedMatrix.EveryCantoolsVectorAgrees` — 10,000 frames, 23,222 signals, and a disagreement fails with the payload printed | `case` |
| W-155 | Parser test nondeterminism | `Cli_DecodeOutputStable` (byte-identical twice), `Pipeline_DeterministicForFixedSequence` | `case` |

---

## The 7 P0 cases that are NOT covered

Listed rather than buried, because an unmapped P0 is the thing this document
exists to surface. None is a silent gap: each is a decision or a missing seam.

| W-ID | What-if | Why not, and what it would take |
|---|---|---|
| W-002 | DBC file not readable (`chmod 000`) | Project 06 has no `FileOps` seam to inject `EACCES`, unlike project 04. The code path returns `DBC_SOURCE` for any open failure, so the behaviour is almost certainly right — but "almost certainly" is not a test. Cheapest fix: a test that `chmod 000`s a temp file, skipped when running as root |
| W-073 | Receive timestamp in the future | `rxTime` is taken from the source and never validated against `now`. A future stamp would make a signal look fresh for as long as the skew. Needs a decision first: clamp, reject, or accept and document |
| W-107 | Reload during shutdown | `stop()` cancels the scheduler jobs, so a queued reload should never run — but nothing asserts it, and the window between `requestReload()` and the swap is exactly where it would go wrong |
| W-116 | Slow disk or log backend | Needs a slow `ILogSink`, which is project 04's seam, not one project 06 exposes |
| W-120 | Event Bus not started | The adapter lives in the demo, so a not-started bus is a demo-level concern; `publish` would return non-Accepted and be counted, which `EventBus_PublishFailureCounted` covers for a *full* bus but not an unstarted one |
| W-124 | Two consumers want the same signal | Only one subscriber is ever registered in the tests. The registry is a vector walked under no lock during dispatch, so two observers is a different path from one |
| W-133 | Malicious string injected into logs | Invariant D-02 (never quote more than 64 characters, never quote payload) is implemented and reviewed but not asserted. A test feeding a DBC with a 10 KB signal name and grepping the diagnostic would close it |

Seven more are marked `design` rather than `none` — W-068, W-072, W-081, W-125,
W-127, W-135 and W-144 — because the code has no path to the failure and the
reasoning is recorded next to it. They are **not** tests and are not counted as
coverage; a reader who disagrees with one of those arguments should read it as a
gap.

**Count**, taken from this file rather than estimated: 116 P0 cases, each
appearing exactly once.

| Kind | Count |
|---|---|
| `case` — a named test asserts it | 91 |
| `suite` — fuzzing, golden matrix, sanitizers, soak, or project 05's own tests | 11 |
| `design` — no code path to the failure, reasoning recorded | 7 |
| **`none` — no coverage** | **7** |

So **94 % of P0 cases have test or mechanism coverage**, and the 7 that do not
are named above with what it would take to close each.
