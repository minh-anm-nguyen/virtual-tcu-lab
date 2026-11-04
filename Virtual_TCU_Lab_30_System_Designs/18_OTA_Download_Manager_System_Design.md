# P18 — OTA Download Manager (ODM)

**Virtual TCU Lab | System Design + Operational Behaviors + C++17 Interfaces + Exhaustive-oriented What-if / Multi-fault Test Plan**

| Thuộc tính | Giá trị |
|---|---|
| Dự án | **18 / 30 — OTA Download Manager** |
| Version | **1.0 — bản thiết kế và test plan, chưa implement** |
| Ngày | 2026-10-30 |
| Đối tượng | Middle C++ Developer trong Automotive TCU; Integration & Test Engineer |
| Lab | Ubuntu Linux, C++17, CMake, libcurl, OpenSSL, SQLite, Google Test/Mock, Python HTTPS fixture, network namespaces, `tc netem` |
| Đầu ra | `libtcu_ota_download`, `tcu-otad`, `ota-test-server`, `ota-fixture-generator`, `ota-fault-runner`, evidence reports |
| Phụ thuộc | P01 Event Bus, P02 IPC, P03 Timer, P04 Config/Logging, P10 Connectivity, P11 Fault Injection, P13 MQTT, P14 Storage, P15 Cloud, P17 Health Monitoring; P19 nhận artifact đã verified |
| Phạm vi | Download, resume, xác minh signed metadata/image, durable staging, handoff **không thực hiện install/flash** |
| Ranh giới an toàn | Dùng firmware **giả** và endpoint trong lab; không flash ECU/xe thật. Không tự tuyên bố tuân thủ Uptane/ISO 26262/ISO 21434 |

> **Mục tiêu:** Một OTA Download Manager chạy lâu dài trên TCU, tải gói firmware giả qua HTTPS, chịu được mất mạng, power cut, storage pressure, server trả dữ liệu không nhất quán, metadata lỗi và nhiều race condition; tuyệt đối **không** cung cấp file chưa được xác minh cho installer.

## 0. Executive summary & quyết định thiết kế

1. **Metadata được xác thực trước khi tin tưởng.** Giả định một `SignedUpdateManifest` có signature hợp lệ từ khóa public tin cậy provision sẵn. URL, kích thước, hash, target ECU/version, expiration và policy chỉ được tin sau khi verify; hash do server tự gửi qua HTTP không phải root of trust.
2. **HTTP Range không phải bảo đảm resume.** Resume chỉ hợp lệ khi dữ liệu cũ và mới cùng representation. `Range` + strong `If-Range` ETag là cơ chế hỗ trợ; luôn kiểm tra `206`, `Content-Range` và offset. `200` trong lúc resume bắt buộc restart sạch; `416` không tự đồng nghĩa hoàn tất.
3. **File chưa verified phải ở vùng cách ly.** `.part` / staging khác namespace với `.verified`; installer chỉ thấy artifact đã kiểm tra hash + signed metadata + policy.
4. **Lưu bền vững phải có điểm checkpoint rõ ràng.** Một byte chỉ được gọi là `durable_offset` sau khi flush/fsync dữ liệu tương ứng và persist checkpoint đáng tin; sau crash có thể truncate về offset đã commit. Không quảng cáo `write()` là durable.
5. **SQLite và filesystem không có transaction nguyên tử chung.** Áp dụng protocol `PREPARED → FSYNCED → RENAMED → COMMITTED` cộng boot-time reconciliation. Không được khẳng định đã verify chỉ dựa vào DB row hoặc filename.
6. **At-most-one local owner** cho `(device, target_ecu, artifact_id)` qua serialized coordinator và OS process lock. Candidate thay đổi là một job generation mới; callback cũ không thể ghi lên job mới.
7. **Bounded resources:** 1 transfer/job mặc định, max 2 concurrent global trong lab; quota files, DB, callbacks, retry timers, payload, headers và redirect count. Khi thiếu storage → `WAITING_STORAGE` hoặc failure có lý do, không xóa image đang hoạt động.
8. **Monotonic time** cho retry/deadline/progress; secure UTC/attestation bắt buộc cho metadata expiration/anti-freeze ở profile security cao. RTC không tin cậy → `WAITING_TRUSTED_TIME`, không tự bypass.
9. **Retry phân loại:** retry có backoff cho network/5xx/429 theo policy; các lỗi `AUTH`, signature, hash mismatch, target mismatch, rollback, TLS validation là hard failure cần manifest mới hoặc can thiệp có kiểm soát.
10. **Không gọi download success là update success.** P18 phát `ArtifactVerified`/`ReadyForHandoff` với ID/hash; P19 riêng quản lý install/A-B slot/rollback. P18 không quyết định xe đủ điều kiện flash.
11. **Không lộ secrets:** bearer token không ghi log; redirects không chuyển credentials sang origin không tin cậy; certificate/hostname verification luôn bật ở production profile.
12. **Fault-injection evidence:** một test chỉ PASS nếu bằng chứng xác minh fault thật sự xảy ra *và* invariants của hệ thống đúng. `INCONCLUSIVE` nếu inject thiếu hoặc oracle thiếu.

### 0.1 Invariants bất biến (đánh số để trace)

| ID | Invariant / Must never happen | Kiểm chứng |
|---|---|---|
| INV-01 | Không có artifact `READY` nếu signed manifest không hợp lệ/hết hạn/không đúng target | hash/signature/metadata oracle |
| INV-02 | Không có artifact `READY` nếu final size hoặc SHA-256 khác authenticated manifest | rehash độc lập + manifest oracle |
| INV-03 | `durable_offset` không được lớn hơn bytes thật có thể phục hồi an toàn | crashpoint + fsync fake |
| INV-04 | Resume không append response `200` lên phần file cũ | download trace + content hash |
| INV-05 | Response `206` thiếu/sai `Content-Range` phải bị reject trước append | mock HTTP assertions |
| INV-06 | Không có hai jobs cùng ghi một staging path/job generation | lock + trace writer ownership |
| INV-07 | Callback từ generation cũ không thể chuyển job mới sang trạng thái thành công | epoch fencing tests |
| INV-08 | Handoff chỉ được thực hiện sau khi verified file đã flush và được publish an toàn | crash/rename matrix |
| INV-09 | TLS verify fail không thể fallback sang HTTP, insecure mode hay mirror không được phép | negative TLS tests |
| INV-10 | Runtime RAM, disk, FD, threads, retries phải bounded theo config | soak + pressure metrics |
| INV-11 | Một job có không quá một terminal outcome cho mỗi generation | deterministic races |
| INV-12 | Không xóa/corrupt artifact đã verified của job khác khi cleanup | cross-job fault tests |
| INV-13 | Chính sách power/metered network/pause được thực thi trước resume hoặc download mới | policy timeline |
| INV-14 | Timeout/cancel/shutdown không tạo use-after-free, deadlock hoặc overwrite trạng thái sau terminal | sanitizers + stress |
| INV-15 | Khi không xác minh đủ bằng chứng, công bố `UNKNOWN/FAILED/WAITING`, tuyệt đối không `SUCCESS` giả | fail-closed suite |
| INV-16 | P18 không tự gửi thao tác flash hoặc tác động actuator | no-side-effect oracle |

### 0.2 SLO gợi ý cho lab — không phải guarantee/OEM requirement

| ID | Mục tiêu thử nghiệm | Điều kiện |
|---|---|---|
| SLO-01 | 0 false READY qua toàn bộ corrupted/unsigned corpus | independent verifier |
| SLO-02 | `READY` file có size và SHA-256 khớp 100% | test package 1 MiB–128 MiB |
| SLO-03 | recovery sau SIGKILL không mất trạng thái job đã checkpoint (modulo crash window được định nghĩa) | 100 crashpoints |
| SLO-04 | Resume bytes đã commit chính xác; không trộn image revisions | HTTP fixture / trace |
| SLO-05 | Queue <= 128 jobs, in-flight <= 2, log queue bounded | stress fixture |
| SLO-06 | Retry không quá 1 lần/threshold window mỗi job ngoài policy | fake clock |
| SLO-07 | Stop với bounded deadline nếu IO giả lập hỗ trợ cancellation | shutdown test |
| SLO-08 | 24h soak không tăng RSS/FD/threads unbounded | perf & sanitizer |
| SLO-09 | Test 100 random seeds cho cùng case cho kết quả logic nhất quán | deterministic runner |
| SLO-10 | 0 secret/URL token trong logs/evidence | secret scanner |

## 1. Scope và requirements

### 1.1 Functional Requirements

| ID | Feature | Priority | Verification |
|---|---|---|---|
| FR-01 | Nhận update offer có `job_id`, `manifest_id`, `target_ecu`, `version` | P0 | API contract |
| FR-02 | Verify signature, key trust, target binding, size/hash, expiry trước transfer | P0 | negative manifest corpus |
| FR-03 | Quản lý lifecycle async có trạng thái inspect được | P0 | state transition tests |
| FR-04 | Download HTTPS với connect/read/low-speed/overall timeout | P0 | server stall tests |
| FR-05 | Download `Range` + strong ETag `If-Range` và validate partial response | P0 | 200/206/416 matrix |
| FR-06 | Checkpoint durable offset + recover after crash | P0 | kill-at-step matrix |
| FR-07 | Retry jitter/backoff, retry budget, retry classification | P0 | fake clock |
| FR-08 | Tạm dừng theo Power Manager/Network policy | P0 | policy integration |
| FR-09 | Hạn mức disk/memory/FD/bandwidth/concurrency | P0 | resource saturation |
| FR-10 | Final SHA-256, size, signed metadata verify trước publish | P0 | crypto golden vectors |
| FR-11 | Atomic-ish staging handoff và reconciliation giữa DB/filesystem | P0 | crash state enumeration |
| FR-12 | Cancel/Pause/Resume, idempotent operation IDs | P0 | duplicate API calls |
| FR-13 | Telemetry trạng thái không đưa package contents hoặc tokens | P0 | log audit |
| FR-14 | Export metrics+evidence+failure reason | P0 | observability tests |
| FR-15 | Nhiều jobs, priority, one writer per target, supersession policy | P1 | arbitration tests |
| FR-16 | Hỗ trợ mirror allowlist/mTLS trong profile OEM mock | P1 | redirect/credential tests |
| FR-17 | Chunks/parallel ranged download (optional), vẫn phải verify full image | P2 | reassembly tests |
| FR-18 | Handoff interface cho P19 installer mà không install | P0 | integration contract |

### 1.2 Non-functional requirements

- **Security:** signed manifest, trusted key provisioning, TLS peer/host verification, anti-rollback metadata policy, anti-freeze secure time; không parse dữ liệu package như lệnh.
- **Robustness:** cancellation-aware IO, bounded retry, crash recovery, deterministic state machine, no silent truncation.
- **Maintainability:** C++17 interfaces testable với fake HTTP/filesystem/clock/crypto/metadata, RAII ownership, ABI boundaries.
- **Integrity:** immutable verified artifact ID; một consumer không được ghi vào file trong verified store.
- **Observability:** mỗi event mang `job_id`, `attempt`, `generation`, `boot_id`, `trace_id`; monotonic durations.

### 1.3 Out of scope

Flashing thật, A/B slot activation, bootloader recovery, rollback install, ignition inhibit, UDS firmware update thật, key provisioning production, HSM certification, complete Uptane-conformant client. P19 sẽ giả lập install/rollback bằng file slots, nhưng P18 chỉ handoff **đã verified**.

## 2. Architecture

```mermaid
flowchart TD
  OFFER[P15 Cloud / Update Offer] --> VERIFY[Signed Manifest Verifier]
  TRUST[Provisioned Trust Roots + Trusted Time] --> VERIFY
  VERIFY --> COORD[OTA Job Coordinator]
  POLICY[P10 Connectivity + P22 Power Policy] --> COORD
  COORD --> STATE[State Machine / Retry Scheduler P03]
  STATE --> HTTP[libcurl HTTPS Adapter]
  HTTP <--> SERVER[Python HTTPS OTA Fixture]
  HTTP --> FILE[Staging .part + durable checkpoints]
  FILE --> HASH[Whole-image SHA256 + Manifest Policy Check]
  HASH --> ATOM[Publish Verified Artifact]
  ATOM --> P19[Read-only Artifact Handoff to P19]
  COORD <--> DB[(SQLite Jobs + Checkpoints)]
  STATE --> OBS[P04 Logging + Metrics / P17 Health]
  NET[P11 Fault-injection Harness] -.-> HTTP
  FAKE[Fake FS / Clock / Transport] -.-> COORD
```

### 2.1 Ownership và interface boundaries

| Component | Owns | Không làm |
|---|---|---|
| `ManifestVerifier` | sigs, manifest schema, trust roots, target/expiry/policy | tin unsigned URL/hash |
| `JobCoordinator` | admission, job generation, priority, terminal commit, pause/cancel | blocking HTTP trong lock |
| `DownloadStateMachine` | legal transitions, retry classification, timers | tự kiểm tra certificate bằng chuỗi custom |
| `HttpDownloadTransport` | HTTP/TLS, response headers, stream callbacks, cancellation | tự quyết READY |
| `ResumeValidator` | HTTP 206/200/416, ETag, Content-Range, content encoding | append khi không chứng minh cùng revision |
| `StagingStore` | file open flags, write, fsync, truncate, rename, quotas | delete bất kỳ path user-supplied |
| `JobRepository` | SQLite transactions, durable checkpoint, revisions | giả định SQLite+FS atomic |
| `PayloadVerifier` | verify final byte count, SHA-256, target & signed metadata | dùng CRC như crypto |
| `ArtifactPublisher` | verified immutable content-addressed storage, handoff record | flash trực tiếp |
| `PolicyGate` | network/power/metered/time availability, generation rules | override vehicle safety policy |
| `OtaStatusReporter` | state/metrics/cloud notification | gọi broker PUBACK là installer success |
| `FaultHarness` | local fault events và oracle | inject vào mạng máy thật |

### 2.2 Process topology

- **MVP:** `tcu-otad` là một process, `libtcu_ota_download` tách libraries; SQLite 1 writer, worker HTTP async/isolated. `ota-test-server` là process Python trong network namespace hoặc Docker lab.
- **Production-like lab:** status qua P02 IPC; update offers từ P13/P15 MQTT; share P04 Config/Logging, P03 Scheduler, P10 Connectivity State Machine, P11 Fault Injector.
- **Trust boundary:** JSON manifest, HTTP headers, redirects, server payload, test control API, filesystem và database đều có thể trở thành untrusted input; crypto verify bắt buộc trước `READY`.

### 2.3 Sequence: normal download

```mermaid
sequenceDiagram
  participant Cloud as Mock Cloud
  participant O as OTA Coordinator
  participant V as ManifestVerifier
  participant H as HTTPS Server
  participant F as StagingStore
  participant D as SQLite
  participant P as P19 Installer Mock
  Cloud->>O: offer(signed manifest)
  O->>V: Verify(manifest, trusted key/time, target)
  V-->>O: Valid identity/hash/size
  O->>D: Commit job RECEIVED -> QUEUED
  O->>H: GET image (HTTPS)
  H-->>O: 200 + bytes
  O->>F: write .part; fsync durable chunks
  O->>D: save durable_offset
  O->>F: final fsync + calculate hash
  O->>V: verify size/hash + metadata still acceptable
  O->>F: publish immutable verified artifact
  O->>D: Commit READY + handoff ID
  O-->>P: ArtifactVerified(id, path, digest)
```

### 2.4 Sequence: interrupted download and resumed transfer

```mermaid
sequenceDiagram
  participant O as OTA Download Manager
  participant S as HTTPS Fixture
  participant F as StagingStore
  participant D as SQLite
  O->>S: GET /image
  S-->>O: 200 ETag strong "v9" + prefix
  O->>F: fsync prefix at offset 4MiB
  O->>D: COMMIT durable_offset = 4MiB
  Note over O,S: Network lost / process killed
  O->>D: Recovery job checkpoint
  O->>F: inspect file and truncate to trusted durable_offset
  O->>S: GET Range: bytes=4194304-; If-Range: "v9"
  S-->>O: 206 Content-Range: bytes 4194304-N/T
  O->>O: verify status, ETag, range, total length, identity encoding
  O->>F: append from verified boundary
  O->>O: final complete-file SHA256 against trusted manifest
```

### 2.5 Sequence: final file/DB crash consistency

```mermaid
sequenceDiagram
  participant O as Coordinator
  participant F as Filesystem
  participant D as SQLite
  O->>F: fsync(staging file)
  O->>D: COMMIT phase=VERIFIED_PENDING_PUBLISH
  O->>F: rename staging -> content-addressed verified path
  O->>F: fsync(parent directory)
  O->>D: COMMIT phase=READY, artifact reference
  Note over O,D: Crash possible at every boundary
  O->>O: On reboot: reconcile DB + file + complete hash + trust metadata
```

## 3. States & transition rules

```mermaid
stateDiagram-v2
  [*] --> RECEIVED
  RECEIVED --> REJECTED: invalid metadata/policy
  RECEIVED --> QUEUED: authenticated
  QUEUED --> WAITING_POLICY: power/network/time quota
  WAITING_POLICY --> QUEUED: policy permits
  QUEUED --> DOWNLOADING: admission granted
  DOWNLOADING --> PAUSING: pause request
  PAUSING --> PAUSED: checkpoint persisted
  PAUSED --> QUEUED: resume
  DOWNLOADING --> WAITING_RETRY: recoverable failure
  WAITING_RETRY --> QUEUED: timer+budget+policy
  DOWNLOADING --> VERIFYING: full size reached
  VERIFYING --> READY: verify+durable publish
  DOWNLOADING --> FAILED: terminal failure
  VERIFYING --> FAILED: hash/policy failure
  WAITING_RETRY --> FAILED: exhausted
  QUEUED --> CANCELLED: cancel
  PAUSED --> CANCELLED: cancel
  DOWNLOADING --> CANCELLING: cancel
  CANCELLING --> CANCELLED: safe stop+cleanup
  READY --> [*]
  FAILED --> [*]
  CANCELLED --> [*]
  REJECTED --> [*]
```

**Trạng thái chi tiết:** `RECEIVED`, `REJECTED`, `QUEUED`, `WAITING_POLICY`, `DOWNLOADING`, `PAUSING`, `PAUSED`, `WAITING_RETRY`, `VERIFYING`, `PUBLISHING`, `READY`, `CANCELLING`, `CANCELLED`, `FAILED`, `RECOVERING`, `UNKNOWN_REPAIR_REQUIRED`. `PUBLISHING`/`RECOVERING` là trạng thái persisted được reconcile lúc start, không coi là success từ DB một mình.

| Event | Điều kiện | Transition/side effect |
|---|---|---|
| `OfferAccepted` | Signature+target+version+size valid | RECEIVED→QUEUED, persist intent |
| `ConnectivityLost` | active job | DOWNLOADING→WAITING_RETRY, checkpoint durable prefix |
| `PolicyDenied` | low-battery/metered/time | QUEUED/DOWNLOADING→WAITING_POLICY/PAUSED (quiesce) |
| `RangeAccepted` | exactly validated 206 | append only from committed offset |
| `RangeIgnored` | 200 instead of 206 | truncate/recreate staging, restart at zero |
| `RangeInvalid` | wrong 206/Content-Range | fail attempt; never write response body |
| `FullReceived` | file length exactly signed size | VERIFYING; rehash entire file |
| `VerifySucceeded` | valid signature/metadata/hash/target | durable publish protocol, READY |
| `VerifyFailed` | any mismatch | FAILED + quarantine, no handoff |
| `CancelRequested` | any nonterminal | generation bump, stop IO, durable cancellation |
| `LateCallback` | epoch/generation mismatch | ignored, metrics increment |
| `ProcessRestart` | persisted nonterminal | reconcile DB & filesystem, re-verify READY |

### 3.1 Retry classification and response matrix

| Class | Examples | Policy |
|---|---|---|
| Transient | timeout, DNS unavailable, 502/503/504, HTTP 429 `Retry-After` | bounded exponential backoff + jitter + deadline |
| Network-policy wait | metered/low battery, no bearer, untrusted time | WAITING_POLICY, no repeated network call |
| Revalidate/restart | unexpected `200` for ranged request, changed strong ETag | discard old prefix and restart **only** if new bytes still fit signed hash manifest; otherwise fail |
| Terminal security | certificate/hostname failure, invalid signature, expired metadata, target mismatch, revoked key | fail closed, no insecure fallback |
| Terminal payload | verified size/hash mismatch, untrusted redirect, forbidden encoding | quarantine + require new manifest/job |
| Storage pressure | ENOSPC, EDQUOT, EROFS | WAITING_STORAGE/FAILED with bounded cleanup, never mark READY |
| Potentially recoverable local | EINTR, SQLITE_BUSY, transient IO | bounded retry per operation; no duplicate promotion |

## 4. APIs & C++17 contracts (illustrative, not compilable implementation)

```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tcu::ota {
struct JobId { std::string value; };
struct TargetId { std::string ecu_id; std::string hardware_rev; };
struct SignedManifest {
  std::string manifest_id;       // authenticated identity after verification
  TargetId target;
  std::string version;
  std::string https_url;
  std::uint64_t size_bytes{0};
  std::string sha256_hex;
  std::vector<std::uint8_t> signed_bytes;  // canonicalized bytes in protocol
  std::vector<std::uint8_t> signature;
  std::string key_id;
};
enum class JobState {
  RECEIVED, REJECTED, QUEUED, WAITING_POLICY, DOWNLOADING,
  PAUSING, PAUSED, WAITING_RETRY, WAITING_STORAGE, VERIFYING,
  PUBLISHING, READY, CANCELLING, CANCELLED, FAILED,
  RECOVERING, UNKNOWN_REPAIR_REQUIRED
};
struct JobSnapshot {
  JobId id;
  JobState state;
  std::uint64_t generation{0};
  std::uint64_t received_bytes{0};
  std::uint64_t durable_offset{0};
  std::optional<std::string> verified_artifact_id;
  std::string reason_code;
};
struct EnqueueReply { bool accepted; JobId id; std::string reason; };
class IOtaDownloadService {
public:
  virtual ~IOtaDownloadService() = default;
  virtual EnqueueReply enqueue(const SignedManifest& offer,
                               const std::string& request_id) = 0;
  virtual bool pause(const JobId& id, const std::string& request_id) = 0;
  virtual bool resume(const JobId& id, const std::string& request_id) = 0;
  virtual bool cancel(const JobId& id, const std::string& request_id) = 0;
  virtual std::optional<JobSnapshot> inspect(const JobId& id) const = 0;
};
struct HttpResponseMeta {
  long status{0};
  std::string strong_etag;
  std::string content_range;
  std::string content_encoding;
  std::string effective_url;
};
class IHttpTransfer {
public:
  virtual ~IHttpTransfer() = default;
  // callback can abort; never call application lock while calling user callback.
  virtual void fetch(const SignedManifest& trusted_offer,
                     std::uint64_t requested_offset,
                     std::function<bool(const HttpResponseMeta&)> on_headers,
                     std::function<bool(const std::uint8_t*, std::size_t)> on_body) = 0;
  virtual void cancel() noexcept = 0;
};
class IClock {
public:
  virtual ~IClock() = default;
  virtual std::chrono::steady_clock::time_point now() const = 0;
};
class ITrustedTime {
public:
  virtual ~ITrustedTime() = default;
  virtual bool isTrustworthy() const = 0;
};
class IManifestVerifier { public: virtual ~IManifestVerifier() = default;
  virtual bool verify(const SignedManifest& m, const TargetId& target) const = 0;
};
class IStagingStore { public: virtual ~IStagingStore() = default;
  virtual std::uint64_t durableOffset(const JobId& id) const = 0;
  virtual bool truncateTo(const JobId& id, std::uint64_t offset) = 0;
  virtual bool finalizeVerified(const JobId& id, const std::string& sha256) = 0;
};
} // namespace tcu::ota
```

### 4.1 API behavioral contracts

- `enqueue()` idempotent theo `(request_id, manifest_id, target)`; cùng request ID với payload khác trả `CONFLICT`.
- `pause()` chỉ trả `PAUSED` sau khi IO stop và state + checkpoint ổn định; asynchronous API nên trả `PAUSE_ACCEPTED` nếu quiesce chưa hoàn thành.
- `cancel()` không thể thu hồi artifact đã handoff nếu consumer P19 đã nhận; phải định nghĩa rõ cancellation boundary.
- `inspect()` trả immutable snapshot với generation; `READY` kèm cryptographic artifact identity, không chỉ path.
- Callback từ HTTP adapter có thể đến từ IO thread; user không được giữ mutex toàn cục trong callback.
- `IManifestVerifier` dùng canonical/signature scheme do manifest protocol quy định; không tự concatenate JSON fields rồi verify nếu protocol chưa định nghĩa canonicalization.

## 5. Data model & SQLite durability

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 3000;

CREATE TABLE IF NOT EXISTS ota_jobs (
  job_id TEXT PRIMARY KEY,
  request_id TEXT NOT NULL UNIQUE,
  manifest_id TEXT NOT NULL,
  target_ecu TEXT NOT NULL,
  hardware_rev TEXT NOT NULL,
  version TEXT NOT NULL,
  trusted_hash TEXT NOT NULL,
  expected_size INTEGER NOT NULL CHECK (expected_size > 0),
  state TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK (generation >= 1),
  durable_offset INTEGER NOT NULL DEFAULT 0 CHECK (durable_offset >= 0),
  etag TEXT,
  staging_name TEXT NOT NULL,
  artifact_id TEXT,
  error_code TEXT,
  attempt INTEGER NOT NULL DEFAULT 0 CHECK (attempt >= 0),
  state_revision INTEGER NOT NULL DEFAULT 1,
  CHECK (durable_offset <= expected_size)
);
CREATE INDEX IF NOT EXISTS ota_job_state_idx ON ota_jobs(state, target_ecu);
CREATE TABLE IF NOT EXISTS ota_artifacts (
  artifact_id TEXT PRIMARY KEY,
  job_id TEXT NOT NULL REFERENCES ota_jobs(job_id),
  sha256 TEXT NOT NULL,
  size_bytes INTEGER NOT NULL CHECK (size_bytes > 0),
  verified_name TEXT NOT NULL UNIQUE,
  promotion_phase TEXT NOT NULL,
  metadata_revision TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS ota_transitions (
  event_id TEXT PRIMARY KEY,
  job_id TEXT NOT NULL REFERENCES ota_jobs(job_id),
  generation INTEGER NOT NULL,
  prior_state TEXT NOT NULL,
  next_state TEXT NOT NULL,
  reason TEXT NOT NULL
);
```

**Cảnh báo:** schema là baseline lab, chưa lưu signed manifest bytes và trust snapshot đầy đủ; implementation phải lưu/định danh metadata đã verified để re-verify sau reboot. `synchronous=FULL` và WAL không tự bảo đảm durability nếu storage hoặc filesystem nói dối về flush; cần test fault model và dùng SQLite phiên bản đã vá lỗi. Việc DB commit và file rename **không** tạo transaction xuyên cả hai.

### 5.1 On-disk layout, permissions, recovery

```text
/var/lib/tcu-ota/                       # lab-only
  db/jobs.sqlite3
  incoming/<safe_job_id>.part           # never remotely supplied path
  verified/sha256-<64hex>.img          # immutable, read-only to consumers
  quarantine/<safe_job_id>.<reason>     # never executable
  locks/otad.lock
```

- Tạo file bằng `openat()` vào directory handle do process sở hữu, `O_NOFOLLOW|O_CLOEXEC|O_CREAT|O_EXCL` khi phù hợp, permission 0600; không cho server chọn path.
- Snapshot `durable_offset` là prefix thực sự đã flush, không phải `received_bytes`. Kiểm tra file actual size >= offset; truncate bytes vượt offset sau crash và revalidate identity.
- `fsync(file)` trước checkpoint; `fsync(directory)` sau rename; nếu fsync fail, không công bố READY. Nếu platform có semantics khác cần adapter/test riêng.
- Quarantine corrupted payload và cleanup theo quota/retention policy; không tự xóa artifact tốt cho target khác.
- Khởi động: scan DB, validate path/symlink, reconcile `PUBLISHING`, kiểm tra size/hash metadata của artifact, release stale locks theo process ownership.

## 6. HTTP Range/validator protocol contract

1. Nếu offset=0: GET toàn bộ, chỉ chấp nhận `200 OK` với bounded headers; Content-Length khi được cung cấp không vượt signed size; stream chỉ ghi tối đa signed size.
2. Nếu offset>0: gửi `Range: bytes=<offset>-` và `If-Range: <strong ETag>` khi có strong validator; nếu không có strong validator, lựa chọn an toàn của lab là **restart tại 0**. Không dùng weak ETag như strong `If-Range`.
3. `206`: reject nếu `Content-Range` thiếu, sai start/end/total, total không khớp signed size, overflow integer, content-encoding khác identity; không ghi byte trước header validation.
4. `200` khi offset>0: server đã bỏ qua range/validator không match → **discard/truncate prefix** và xem như full download mới. Không append; vẫn verify toàn bộ theo signed manifest.
5. `416`: chỉ cân nhắc rehash staged file khi đủ expected size và header hợp lệ; nếu khác, restart hoặc terminal fail. `416` không phải bằng chứng hoàn tất.
6. `3xx`: redirect URL phải theo allowlist, vẫn HTTPS, bounded redirect count, không leak Authorization header sang origin khác, không follow local/file schemes.
7. Unknown-length/chunked: stream bounded bằng signed expected size; nhận ít hơn hoặc nhiều hơn đều fail verify.
8. `Content-Encoding`: bắt buộc identity cho byte-exact OTA image; không để libcurl tự decompress làm sai offset/digest.
9. Tất cả HTTP/TLS errors được map vào `FailureClass` stable; không sử dụng status code như bằng chứng security.
10. Signed manifest là nguồn size/hash chuẩn; ETag là validator hỗ trợ transfer consistency, **không thay thế cryptographic verification**.

## 7. Retry, bandwidth & power policy

- Backoff `min(base*2^attempt, cap)+bounded jitter`; saturating arithmetic, fake random seed cho test; bảo đảm shutdown cancel mọi timer.
- Trạng thái `WAITING_POLICY` và `WAITING_STORAGE` không tiêu thụ retry network budget nếu chưa phát HTTP request.
- Retry budget kết hợp `max_attempts`, total job deadline và minimum low-speed threshold (để chống endless-data/slow retrieval), timeouts monotonic.
- P22 Power Manager (nếu chưa có thì fake) phát `ALLOW`, `PAUSE`, `DENY`: P18 không tự đánh giá tình trạng xe có cho phép install; chỉ hạn chế download theo chính sách của lab.
- Nếu chuyển bearer, callback cũ phải gắn `connection_epoch` và `job_generation`, không cho ghi vào staging sau khi resume phiên mới.
- Candidate supersession chỉ được thực hiện sau verify signed metadata/policy; không hủy job cũ chỉ vì untrusted offer đến.

## 8. Concurrency & lifecycle

- Một `Coordinator` serialize transition của mỗi job; IO worker không mutate DB trực tiếp, gửi completion tới event loop có generation.
- `StagingStore` là single-writer per staging file. Bất kỳ task asynchronous nào giữ RAII handle; `cancel()` đặt token và chờ drain theo deadline.
- Không giữ DB mutex trong `curl_multi_perform()` hoặc fsync; IO completion qua bounded channel.
- `Stop(DRAIN)`: ngừng nhận job mới, chờ chunk/checkpoint ổn định trong deadline; quá deadline → force pause/fail recovery-safe, không `READY` giả.
- `Stop(DISCARD)`: cancel IO, giữ durable prefix, persist resumable state; không xóa dữ liệu còn hợp lệ mặc định.
- Handoff P19 phải có ownership contract: immutable verified file path/descriptor/hash; P19 phải verify lại trước dùng tùy threat model.
- Boot-time reconciliation phải xử lý SIGKILL tại mọi transition; không rely destructors chạy sau kill.

## 9. Operational behavior (Given / When / Then)

| ID | Given / When | Then / Expected |
|---|---|---|
| BC-01 | Signed manifest hợp lệ, online, đủ quota → enqueue | Job được persist, transfer bắt đầu đúng URL |
| BC-02 | Unsigned/expired/wrong-target manifest → enqueue | REJECTED, 0 HTTP GET và 0 staged bytes |
| BC-03 | 4 MiB đã fsync và checkpoint; network mất | WAITING_RETRY; reboot nối từ durable offset sau validation |
| BC-04 | HTTP server trả `200` dù client gửi Range | Không append; restart sạch từ byte 0 |
| BC-05 | Server trả `206` nhưng `Content-Range` sai | Reject response trước khi ghi |
| BC-06 | Response đủ bytes nhưng SHA-256 mismatch | FAILED/QUARANTINE, không handoff |
| BC-07 | Receive pause khi đang write | Đạt checkpoint và PAUSED; không phát requests mới |
| BC-08 | Cancel sau khi download nhưng trước publish | CANCELLED, không publish artifact mới |
| BC-09 | SIGKILL giữa rename và DB READY | Boot reconcile bằng file/hash/metadata và finalize hoặc quarantine |
| BC-10 | Disk full giữa checkpoint | Không commit offset vượt phần durable, no READY |
| BC-11 | TLS cert invalid hoặc hostname mismatch | Fail closed; không dùng HTTP fallback |
| BC-12 | Incoming duplicate offer | Cùng ID → same job; conflict payload → reject |
| BC-13 | Policy chuyển DENY khi đang transfer | Quiesce IO, checkpoint safe prefix, chờ policy |
| BC-14 | Offer manifest mới giữa transfer cũ | Chỉ áp dụng supersession theo authenticated policy |
| BC-15 | Callback từ attempt cũ đến sau cancel/restart | Ignore, không mutate new generation |
| BC-16 | Server gửi > signed size | Abort, không disk growth vượt quota |
| BC-17 | RTC nhảy lùi / secure time mất | Không dùng thời gian chưa tin cậy để chấp nhận metadata expired |
| BC-18 | Restart với DB có READY nhưng file mất | State UNKNOWN_REPAIR_REQUIRED, không handoff |
| BC-19 | Retry budget exhausted | FAILED với reason và metrics, không busy retry |
| BC-20 | Shutdown + download complete cùng thời điểm | Có duy nhất một terminal state, không deadlock |

## 10. Fault injection architecture & pass/fail oracle

```mermaid
flowchart LR
  SCRIPT[Scenario YAML + deterministic seed] --> RUN[Scenario Runner]
  RUN --> NET[P11 netns + tc netem / mock DNS]
  RUN --> HTTP[HTTPS server response scripting]
  RUN --> FS[Fake filesystem / ENOSPC / fsync faults]
  RUN --> DB[SQLite busy/corrupt/crashpoints]
  RUN --> TIME[Fake monotonic clock & trusted-time adapter]
  RUN --> PROC[SIGKILL / service restart]
  NET --> DUT[tcu-otad]
  HTTP --> DUT
  FS --> DUT
  DB --> DUT
  TIME --> DUT
  PROC --> DUT
  DUT --> EVID[Independent file/hash/DB/trace oracle]
  EVID --> RESULT[PASS / FAIL / INCONCLUSIVE / SKIPPED]
```

**Run result meanings:**

- `PASS`: Fault injected and observed as specified; external oracle verifies every applicable invariant and timing/resource gates.
- `FAIL`: Fault injected; expected invariant/behavior fails or telemetry contradicts actual side effects.
- `INCONCLUSIVE`: Không xác nhận được injection, thiếu evidence, test fixture lỗi, timing không khớp.
- `SKIPPED`: Gated by missing prerequisite and reported transparently, not counted as PASS.

**Evidence bắt buộc:** `scenario_id`, `seed`, `manifest_id`, `job_id`, `generation`, `HTTP request/response metadata` (redacted), `injection receipts`, SHA-256 stage/final, SQLite snapshot, state transition trace, resource watermarks, verdict + reason, timestamps monotonic/UTC label. Không lưu private key hoặc bearer token.
## 11. Catalog 300 What-if scenarios — 30 nhóm

**Mục đích:** tìm những cách hệ thống có thể hỏng, không phải tuyên bố bao phủ toán học toàn bộ mọi lỗi. Mỗi case được mô tả độc lập theo bốn trường: **What if / cách inject / expected behavior / bằng chứng**; `P0` là lỗi có thể gây sai security/integrity/durability, còn `P1` là reliability/operability. ID cố định để tạo test name. **Điều kiện PASS:** injection receipt có thật + independent oracle đồng ý + các invariants liên quan giữ đúng. Không có bằng chứng: `INCONCLUSIVE`.

**Quy ước để tự động hóa:** dùng `FakeClock`, `FakeHttpTransfer`, `FaultyFileOps`, `FaultySqliteVfs`, `ProcessKiller`, P11 `netem` và verifier bên ngoài; phần yêu cầu crash/powercut phải chạy subprocess hoặc storage fault model thay vì chỉ dùng exception. Tất cả string/size limits là policy của lab, không mặc định là quy định OEM.

### 01. Offer ingestion, idempotency & ordering

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-01-01` | Offer có request_id trùng và manifest giống | Gửi hai offers giống nhau đồng thời | Trả cùng job; không tạo hai downloads | UNIQUE request_id và request log | P0 |
| `P18-WIF-01-02` | Request ID trùng nhưng hash khác | Cố ý thay manifest bytes trong retry | CONFLICT; không đổi job đang chạy | DB snapshot và rejection | P0 |
| `P18-WIF-01-03` | Hai offers khác ID cùng target/version | Bắn đồng thời qua IPC và MQTT | Arbitration deterministic theo policy; một owner ghi | Lock-owner trace | P0 |
| `P18-WIF-01-04` | Offer đến khi service STARTING | Submit trước recovery xong | Queue bounded hoặc reject SERVICE_NOT_READY; không lách recovery | State transitions | P0 |
| `P18-WIF-01-05` | Offer tới sau Stop admission | Inject offer giữa shutdown | Không tạo job/IO mới; phản hồi SERVICE_STOPPING | HTTP GET count | P0 |
| `P18-WIF-01-06` | Manifest hợp lệ đến khi storage read-only | Chmod fixture path trước enqueue | WAITING_STORAGE/FAILED, không tự cho READY | Error code và file list | P1 |
| `P18-WIF-01-07` | Offer malformed nhưng request_id hợp lệ | Payload sai format và repeated IDs | Không chiếm idempotency key của offer tốt | DB audit | P1 |
| `P18-WIF-01-08` | Offer quá hạn ngay trước admission | Advance secure time tại ranh giới | Re-check policy trước network/handoff | Trusted-time trace | P1 |
| `P18-WIF-01-09` | Hai tenants/device dùng cùng logical job ID | Giả lập device scoped submit | Không cross-device job lookup/handoff | Principal and job scope logs | P1 |
| `P18-WIF-01-10` | Flood hàng triệu offers duy nhất | Bắn offers vượt max queue | Admission bounded; error có lý do; không OOM | RSS, queue depth, rejected metrics | P0 |

### 02. Signed manifest parsing & schema validation

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-02-01` | Manifest thiếu sha256 | Bỏ thuộc tính bắt buộc | Reject trước GET và không tạo artifact | Validator report + GET count | P0 |
| `P18-WIF-02-02` | Manifest có size=0 | Thay expected size thành zero | Reject trong schema stage | REJECTED + DB state | P0 |
| `P18-WIF-02-03` | Manifest size vượt MAX_IMAGE_BYTES | Dùng unsigned 64-bit max | Reject trước cấp quota/file | Zero allocation evidence | P0 |
| `P18-WIF-02-04` | Manifest JSON duplicate keys | Đặt hai trường hash khác nhau | Reject ambiguous representation | Parser verdict | P0 |
| `P18-WIF-02-05` | Canonical bytes bị thay sau signature check | Mutate buffer/copy trong callback | Chỉ dùng immutable verified snapshot | Snapshot hash log | P0 |
| `P18-WIF-02-06` | Manifest nesting sâu bất thường | Fuzzer tạo deep nested arrays | Parser fail bounded CPU/stack | CPU/stack limits | P0 |
| `P18-WIF-02-07` | Manifest URL chứa newline/NUL | Embed escape/control chars | Reject, không header injection | Parsed URL + no external request | P0 |
| `P18-WIF-02-08` | Manifest dùng path traversal trong target filename | `../../verified/file` trong metadata | Không dùng tên remote làm file path | Canonical path audit | P0 |
| `P18-WIF-02-09` | Manifest chưa biết algorithm | `sha1`/unknown signature algorithm | Reject không fallback thuật toán yếu | Verify failure reason | P0 |
| `P18-WIF-02-10` | Manifest có unknown critical extension | Thêm field critical chưa hỗ trợ | Fail closed, không silently ignore | Extension negotiation trace | P0 |

### 03. Manifest trust, signature & key lifecycle

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-03-01` | Signature bit flip | Thay 1 byte chữ ký | Reject trước network GET | Crypto oracle and requests | P0 |
| `P18-WIF-03-02` | Public key không nằm trong trust store | Ký với key lạ | Reject UNKNOWN_KEY | Key identifier audit | P0 |
| `P18-WIF-03-03` | Key bị revoke trong lúc tải | Reload revoked trust epoch | Handoff revalidation fail/pause theo policy | Epoch and publish trace | P0 |
| `P18-WIF-03-04` | Key rotation có hai trusted keys | Cấp key mới hợp lệ cùng key cũ | Chấp nhận đúng key và recorded trust generation | Key/trust snapshot | P0 |
| `P18-WIF-03-05` | Threshold signatures không đủ | Manifest ký 1/2 threshold | Reject nếu profile yêu cầu 2/2 | Independent verifier | P0 |
| `P18-WIF-03-06` | Metadata bị rollback revision | Offer revision nhỏ hơn persisted floor | Reject dù signature hợp lệ | Anti-rollback store | P0 |
| `P18-WIF-03-07` | Hash metadata signature OK nhưng payload giả | Serve altered blob | Không READY, quarantine | Full SHA-256 | P0 |
| `P18-WIF-03-08` | Signed manifest cho ECU khác | Đổi target ECU trong offer | Reject target binding | Device inventory oracle | P0 |
| `P18-WIF-03-09` | Store trust key bị corrupt trên reboot | Inject bitflip trust file | REPAIR_REQUIRED không fail-open | Startup status | P0 |
| `P18-WIF-03-10` | Crypto verification cực chậm/hang | Mock verifier stall/timeout | Timeout hoặc watchdog; không báo verified | Watchdog + bounded queues | P0 |

### 04. Target, version & release policy

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-04-01` | ECU revision không tương thích | Set incompatible hardware_rev | Reject before HTTP GET | Target policy log | P0 |
| `P18-WIF-04-02` | Installed version mới hơn target | Set release lower than version floor | Deny rollback per policy | Version snapshot | P0 |
| `P18-WIF-04-03` | Same version already installed | Repeat offer target same version | Dedup or NO_UPDATE; no redundant transfer unless allowed | Job disposition | P0 |
| `P18-WIF-04-04` | Wrong VIN/device binding | Offer signed for other device | Reject before staging | Authenticated target evidence | P0 |
| `P18-WIF-04-05` | Blacklisted release | Add signed release to denylist | Không download/handoff bị cấm | Policy generation | P0 |
| `P18-WIF-04-06` | Release bị thu hồi trong VERIFYING | Revoke before promotion | Không chuyển READY sau revocation | Handoff absence | P0 |
| `P18-WIF-04-07` | Dependencies không đáp ứng | Missing prerequisite release | WAITING_POLICY/REJECTED | Dependency evaluation | P0 |
| `P18-WIF-04-08` | Manifest yêu cầu min bootloader cao | Configure mock bootloader lower | Reject compatibility | Compatibility oracle | P0 |
| `P18-WIF-04-09` | Conflicting release order | Submit older and newer simultaneously | Stable admission; không overwrite newer by older | Arbitration trace | P0 |
| `P18-WIF-04-10` | Offer supersede chưa verified | Gửi newer unsigned offer trong download | Không hủy valid job đang tải | Job generation unchanged | P0 |

### 05. Trusted time, expiry & anti-freeze

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-05-01` | RTC chậm 1 năm | Set wall clock backwards | Không coi metadata expired là valid nếu time untrusted | Time source audit | P0 |
| `P18-WIF-05-02` | RTC nhảy tới tương lai | Advance wall clock 10 năm | Không delete nhầm verified files; reject/wait policy có giải thích | Retention traces | P0 |
| `P18-WIF-05-03` | Secure time absent khi startup | Mock attestation unavailable | WAITING_TRUSTED_TIME, không bypass expiry | Job state | P0 |
| `P18-WIF-05-04` | Metadata expires giữa download | Fake time crosses expiration | Revalidate at READY; do not handoff expired | Promotion log | P0 |
| `P18-WIF-05-05` | Secure clock rolls backwards | Replay older attestation | Refuse older than trusted floor | Secure-time floor | P0 |
| `P18-WIF-05-06` | Attestation không hợp lệ chữ ký | Flip time proof signature | Mark time untrusted | Time verifier output | P0 |
| `P18-WIF-05-07` | Timezone/DST changes | Change TZ while running | Retry measured monotonic, expiry UTC | Timer samples | P0 |
| `P18-WIF-05-08` | Leap second/past epoch | Inject boundary UTC time | No overflow or invalid comparison | Time fuzz assertions | P0 |
| `P18-WIF-05-09` | Monotonic clock advances while suspend | Fake alternate clock/suspend | Re-evaluate deadlines/policy on resume | Deadline trace | P0 |
| `P18-WIF-05-10` | Metadata timestamp version thấp hơn trước | Replay older signed timestamp | Reject freeze/rollback | Anti-freeze store | P0 |

### 06. TLS handshake, certificates & transport auth

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-06-01` | TLS certificate expired | Serve expired cert | Fail closed; no HTTP fallback | TLS error and HTTP 0 | P0 |
| `P18-WIF-06-02` | Hostname mismatch | Cert for other host | Reject verification | Host verification logs | P0 |
| `P18-WIF-06-03` | Unknown CA | Use self-signed untrusted CA | Reject by default | Trust chain report | P0 |
| `P18-WIF-06-04` | Proxy inserts forged certificate | Use intercepted TLS endpoint | Reject outside explicit lab trusted CA | TLS peer details | P0 |
| `P18-WIF-06-05` | mTLS client key missing | Remove client certificate | Error AUTH, no insecure retry | Auth failure | P0 |
| `P18-WIF-06-06` | Peer forces obsolete TLS version | Mock unsupported ciphers/version | Fail secure handshake | TLS trace | P0 |
| `P18-WIF-06-07` | TLS handshake stalls | Hold handshake socket open | Bounded connect/handshake timeout | Monotonic deadline | P0 |
| `P18-WIF-06-08` | Partial TLS record then EOF | Cut encrypted stream | No corrupted file READY, retry policy | Stage size/hash | P0 |
| `P18-WIF-06-09` | Cert rotates mid-job/reconnect | Swap trusted cert/host and reconnect | Accept valid chain only; no stale identity | Verified peer per attempt | P0 |
| `P18-WIF-06-10` | System CA bundle tampered | Corrupt trust store on disk | Fail closed, alert; never disable verify | Startup/peer audit | P0 |

### 07. DNS, proxies, redirects & URL hardening

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-07-01` | DNS NXDOMAIN | Stub DNS answers NXDOMAIN | Bounded retry/policy reason | DNS error counter | P0 |
| `P18-WIF-07-02` | DNS timeout for one resolver | Drop DNS replies | No stuck worker; timeout and retry | Thread and deadline trace | P0 |
| `P18-WIF-07-03` | DNS switches IP between attempts | Rotate A record | Still verify HTTPS hostname and signed artifact | Peer/hostname + digest | P0 |
| `P18-WIF-07-04` | Redirect HTTPS→HTTP | Send 302 to http URL | Reject insecure scheme | No insecure request | P0 |
| `P18-WIF-07-05` | Redirect to 127.0.0.1 / metadata service | 302 to loopback/link-local | Block SSRF per configured endpoint policy | Egress capture | P0 |
| `P18-WIF-07-06` | Redirect chain longer than cap | 50 nested 302s | Fail bounded redirects | Request count | P0 |
| `P18-WIF-07-07` | Redirect to other hostname with Authorization | 302 cross-origin | Do not forward credentials without explicit policy | Server captured headers | P0 |
| `P18-WIF-07-08` | Proxy returns unauthorized HTML | Return 407/200 HTML proxy response | Reject by trusted size/hash; no READY | Content/hash oracle | P0 |
| `P18-WIF-07-09` | IPv6 reachable, IPv4 blackholed | Inject dual-stack asymmetry | Bounded fallback per network policy | Attempt traces | P0 |
| `P18-WIF-07-10` | DNS rebind after allowlist check | Resolve allowed name to private IP | Enforce egress policy on actual connected IP | Network capture | P0 |

### 08. Range, ETag & resumability semantics

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-08-01` | Range request trả 206 đúng start | Interrupt at 4MiB then resume | Append only validated matching range | Byte-level comparison | P0 |
| `P18-WIF-08-02` | Range bị bỏ qua trả 200 | Mock ignores Range header | Reset file/generation safely, start at byte zero | Part checksum + offset | P0 |
| `P18-WIF-08-03` | 206 Content-Range start thấp hơn requested | Return bytes 0-... | Reject before body append | Write counter zero | P0 |
| `P18-WIF-08-04` | 206 Content-Range start cao hơn requested | Return bytes offset+1 | Reject hole; never READY | Range checker | P0 |
| `P18-WIF-08-05` | 206 total length khác signed size | Return other total | Reject response | Header oracle | P0 |
| `P18-WIF-08-06` | 206 Content-Range bị thiếu | Strip header | Reject before append | Header rejection evidence | P0 |
| `P18-WIF-08-07` | Weak ETag trong If-Range | Serve W/"ver" | Do not use weak ETag for strict resume; restart clean | HTTP request headers | P0 |
| `P18-WIF-08-08` | ETag thay đổi giữa attempts | Return changed strong ETag | Never combine two representations; restart clean | File hash | P0 |
| `P18-WIF-08-09` | 416 ở offset == expected size | Respond 416 bytes */size | Rehash staged file; no blind READY | Digest + 416 trace | P0 |
| `P18-WIF-08-10` | 416 length khác expected | Return 416 */other size | Reject/restart per policy; never assume complete | HTTP trace | P0 |

### 09. HTTP status/header handling

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-09-01` | HTTP 204 without body | Return 204 | Reject transfer as incomplete | Status classification | P0 |
| `P18-WIF-09-02` | HTTP 304 due to stale conditional cache | Return 304 | No appended bytes; retry with correct request policy | Request headers | P0 |
| `P18-WIF-09-03` | HTTP 401 token expired | Return 401 once | Refresh via authorized flow; no tight loop | Auth attempts | P0 |
| `P18-WIF-09-04` | HTTP 403 forbidden | Return 403 | Stop unless newly authorized, no endless retry | State and retry budget | P0 |
| `P18-WIF-09-05` | HTTP 404 artifact removed | Return 404 | Terminal/mirror policy; do not use untrusted mirror | Error class | P0 |
| `P18-WIF-09-06` | HTTP 429 with Retry-After seconds | Return 429 Retry-After: 30 | Honor bounded policy delay | Virtual clock | P0 |
| `P18-WIF-09-07` | HTTP 503 with malformed Retry-After | Inject giant invalid date | Clamp/ignore safely with backoff | Timer schedule | P0 |
| `P18-WIF-09-08` | HTTP 100 Continue / 103 Early Hints | Send informational responses | Do not write provisional headers/body as payload | Parser trace | P0 |
| `P18-WIF-09-09` | Header field count/size huge | Send >configured header budget | Abort bounded memory | RSS and header limit | P0 |
| `P18-WIF-09-10` | Conflicting Content-Length & Transfer-Encoding | Craft malformed HTTP via raw fixture | Reject protocol ambiguity; never READY | Wire capture | P0 |

### 10. HTTP body, chunking, content transforms

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-10-01` | Connection closes mid-file | Truncate HTTP body unexpectedly | Keep safe checkpoint, retry, never READY | Offset and digest | P0 |
| `P18-WIF-10-02` | Server sends extra bytes beyond signed size | Stream +1MiB extra | Abort within bound; do not overshoot quota | File size cap | P0 |
| `P18-WIF-10-03` | Server sends too few bytes but returns 200 | Short body with closed connection | Incomplete error, not READY | Expected vs actual size | P0 |
| `P18-WIF-10-04` | Chunked response ends early | Malformed final chunk | Reject incomplete entity | HTTP parser error | P0 |
| `P18-WIF-10-05` | Content-Encoding gzip instead identity | Return compressed transfer body | Reject incompatible representation for byte-range policy | Encoding log | P0 |
| `P18-WIF-10-06` | Body bytes modified on CDN | Flip one random byte | Full SHA mismatch, quarantine | SHA oracle | P0 |
| `P18-WIF-10-07` | Data contains NUL bytes | Binary fixture with zero bytes | Preserve exact binary bytes | Golden hash | P0 |
| `P18-WIF-10-08` | HTTP stream writes zero bytes repeatedly | Stub callback with zero progress | Low-speed timeout, no busy loop | CPU/time trace | P0 |
| `P18-WIF-10-09` | Body callback returns partial write | Fake storage short-write/EINTR | Retry remaining bytes or fail without offset lie | Byte count ledger | P0 |
| `P18-WIF-10-10` | Body arrives after cancellation | Delay callback across cancel | Ignore stale generation; no write to recycled path | Generation fence trace | P0 |

### 11. Network loss, bearer switches & reconnect

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-11-01` | Link drops after 1 byte | netem drop after first response chunk | Partial durable checkpoint; bounded retry | Bytes persisted | P0 |
| `P18-WIF-11-02` | Link drops at fsync boundary | Cut network during fsync | IO and state remain consistent; resume later | Checkpoint trace | P0 |
| `P18-WIF-11-03` | Wi-Fi→LTE during transfer | Switch network interface route | Abort/rebind or resume as new attempt; never mix invalid content | Connection epoch | P0 |
| `P18-WIF-11-04` | IP address changes | Renew DHCP during GET | No forever-hung socket; retry safely | Curl result/deadline | P0 |
| `P18-WIF-11-05` | LTE signal flaps every second | Toggle bearer up/down 100 times | Backoff prevents retry storm | Attempt per minute | P0 |
| `P18-WIF-11-06` | Captive portal intercepts image URL | Return 200 HTML login page | Hash fails; no READY | Payload bytes/hash | P1 |
| `P18-WIF-11-07` | DNS works but TCP refused | RST SYN attempts | Classify transient per policy, bounded | Error type | P1 |
| `P18-WIF-11-08` | TCP blackhole with no FIN | Drop packets one direction | Low-speed/overall timeout; recover | Deadline trace | P1 |
| `P18-WIF-11-09` | Network reported READY but cloud endpoint down | P10 says online, HTTPS fails | Job waits/retries, not READY | Error and state | P1 |
| `P18-WIF-11-10` | Network comes back during shutdown | Inject connectivity-up at stop | No new transfer after shutdown barrier | HTTP GET count | P0 |

### 12. Retry, timeout, backoff & deadlines

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-12-01` | Repeated 500 status | Respond 500 on all attempts | Stop at retry budget, no infinite loops | Attempt count | P0 |
| `P18-WIF-12-02` | Retry backoff arithmetic overflow | Set attempt=UINT_MAX in fixture | Saturate, no negative delay | Timer assertions | P0 |
| `P18-WIF-12-03` | Jitter source repeats every time | Fixed seeded RNG | Still bounded and deterministic tests | Retry histogram | P0 |
| `P18-WIF-12-04` | Timeout callback arrives after success | Queue old timer late | No success→failure state change | Transition log | P0 |
| `P18-WIF-12-05` | Connect timeout but socket callback late | Delay network completion | Old epoch ignored | Epoch counters | P0 |
| `P18-WIF-12-06` | Retry-After greater than job TTL | Return 429 with huge delay | Honor job deadline; eventually terminal/wait policy | Deadline result | P1 |
| `P18-WIF-12-07` | Successful progress below low-speed min | Drip 1 byte/minute | Abort slow transfer within policy | Throughput monitor | P1 |
| `P18-WIF-12-08` | Retry budget reset on reboot erroneously | Crash at last retry, restart | Persist attempt/deadline policy; no reset storm | DB attempt count | P1 |
| `P18-WIF-12-09` | Concurrent transient errors fire retry twice | Inject DNS failure + timer | Coalesce one retry per generation | Scheduled count | P1 |
| `P18-WIF-12-10` | No network for days | Fake clock advance 10 days | Bounded retries/storage, observable waiting state | DB and CPU metrics | P0 |

### 13. Filesystem staging writes & I/O errors

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-13-01` | write() returns EINTR | Fail first write with EINTR | Retry safely without byte duplication | Stub byte ledger | P0 |
| `P18-WIF-13-02` | write() returns short count | Write only partial buffer | Loop remaining bytes; checkpoint exact bytes | File sha and length | P0 |
| `P18-WIF-13-03` | write() returns EIO | Fault FS write mid-chunk | No durable advancement; fail/wait policy | DB offset vs file | P0 |
| `P18-WIF-13-04` | File descriptor unexpectedly closed | Force EBADF during write | Detect failure and cleanup handles | FD audit | P0 |
| `P18-WIF-13-05` | ftruncate fails on restart | Deny truncate | Do not append to untrusted suffix; repair required | Staging bytes | P0 |
| `P18-WIF-13-06` | fsync file fails | Inject EIO on fsync | Never increase durable_offset | DB checkpoint | P0 |
| `P18-WIF-13-07` | Parent directory fsync fails | Inject dir fsync error on promotion | Never claim durable READY | Promotion stage | P0 |
| `P18-WIF-13-08` | Storage flips EROFS mid-write | Remount fake FS read-only | Pause/fail safely, do not destroy verified store | EROFS state | P0 |
| `P18-WIF-13-09` | Existing stage is symlink | Place symlink before open | Open securely or reject; never write outside staging | Target inode audit | P0 |
| `P18-WIF-13-10` | Same target path hardlinked to verified | Place malicious hardlink | Disallow unsafe shared-inode modification | Verified checksum | P0 |

### 14. SQLite checkpoint, transactions & WAL

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-14-01` | SQLite COMMIT returns BUSY | Hold writer lock beyond timeout | Bounded retry; no false checkpoint | Offset trace | P0 |
| `P18-WIF-14-02` | SQLite COMMIT returns IOERR | Inject VFS IOERR at commit | State UNKNOWN until reconcile, no false success | DB recovery | P0 |
| `P18-WIF-14-03` | WAL checkpoint stalls | Hold reader open | Bounded growth policy/monitoring | WAL size and latency | P0 |
| `P18-WIF-14-04` | DB locked during shutdown | Long-running transaction lock | Bounded shutdown without corruption | Stop deadline | P0 |
| `P18-WIF-14-05` | DB file truncated | Corrupt SQLite page | REPAIR_REQUIRED; no READY trust from corrupt DB | integrity_check | P0 |
| `P18-WIF-14-06` | DB transaction commit succeeds then ACK lost | Mock commit no callback | Re-read DB on restart rather than double transition | State revision | P0 |
| `P18-WIF-14-07` | `synchronous=NORMAL` accidentally configured | Flip pragma at startup | Config guard reject unsafe durability profile | PRAGMA logs | P0 |
| `P18-WIF-14-08` | DB offset larger than staging bytes | Edit persisted row via fixture | Repair/truncate state, never append past hole | Size vs offset | P0 |
| `P18-WIF-14-09` | DB offset smaller than staged data | Crash after fsync before DB commit | Truncate safe uncommitted suffix | File length | P0 |
| `P18-WIF-14-10` | Old DB snapshot restored | Replace db with older valid copy | Detect metadata/generation downgrade per policy | Version floor check | P0 |

### 15. Publish, atomic rename & verified store

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-15-01` | Crash after final fsync before rename | SIGKILL at boundary | Recovery revalidates file then promotes or quarantines | Stage/fs trace | P0 |
| `P18-WIF-15-02` | Crash after rename before dir fsync | Kill at promotion boundary | Do not assume name durable until reconciled | Directory listing after reboot | P0 |
| `P18-WIF-15-03` | Crash after dir fsync before DB READY | Kill before DB commit | Reconcile valid verified file and trusted metadata | Hash + DB reconciliation | P0 |
| `P18-WIF-15-04` | DB READY but verified file absent | Delete verified file before reboot | Mark REPAIR_REQUIRED, no handoff | Path check | P0 |
| `P18-WIF-15-05` | Verified destination already exists same hash | Precreate same content-addressed artifact | Reuse only after validating existing immutable artifact | Independent digest | P0 |
| `P18-WIF-15-06` | Verified destination exists wrong hash | Inject collision/malicious file | Refuse overwrite, report integrity incident | Old/new hash | P0 |
| `P18-WIF-15-07` | Rename crosses filesystems | Force EXDEV path | Use same FS staging/publish or explicit safe copy protocol | Rename error | P0 |
| `P18-WIF-15-08` | Verified file mutable by consumer | Change mode/inode through malicious client | Detect/deny mutation; do not trust path alone | Rehash/inode snapshot | P0 |
| `P18-WIF-15-09` | Second writer tries to promote same job | Schedule duplicate promotion callback | Exactly one READY transition | State revision | P0 |
| `P18-WIF-15-10` | Metadata revocation during fsync/rename | Revoke just before publish | Re-check policy at commit barrier, no READY | Trust epoch + handoff trace | P0 |

### 16. Process crash, boot recovery & reconciliation

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-16-01` | SIGKILL before initial DB job commit | Kill at enqueue boundary | No orphan reported as accepted | DB and API audit | P0 |
| `P18-WIF-16-02` | SIGKILL during network receive | Kill after partial write | Recover only stable checkpoint, validate prefix | Reboot stage scan | P0 |
| `P18-WIF-16-03` | SIGKILL after complete file before hashing | Kill before VERIFYING | Rehash from scratch after restart | SHA trace | P0 |
| `P18-WIF-16-04` | SIGKILL during hashing | Kill verifier mid-stream | No stale verification success after restart | State generation | P0 |
| `P18-WIF-16-05` | SIGKILL at PUBLISHING | Crash after phase persist | Reconcile all file/DB combinations | Recovery decision | P0 |
| `P18-WIF-16-06` | Crash loop five times/second | Restart continuously | Backoff restart, no metadata corruption | systemd/runner logs | P0 |
| `P18-WIF-16-07` | Recovered job has missing signed metadata | Delete persisted manifest | Fail closed, quarantine stage | Metadata store audit | P0 |
| `P18-WIF-16-08` | Recovered job has changed stage inode | Replace stage with different file | Reject unsafe candidate; never resume blindly | inode/ctime/hash | P0 |
| `P18-WIF-16-09` | Power cut rather than SIGKILL | Powercut simulator drops unsynced writes | Accepted durability assumptions hold within fault model | fsync and DB oracle | P0 |
| `P18-WIF-16-10` | Boot recovery meets incoming duplicate offer | Send offer during RECOVERING | Enforce single ownership, idempotent lookup | Process lock & DB row | P0 |

### 17. Multi-job admission, priorities & supersession

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-17-01` | Two targets download concurrently | Enqueue two signed jobs | Respect global and per-target concurrency | Active transfer count | P0 |
| `P18-WIF-17-02` | Two different updates same target | Offer old/new in parallel | Stable policy, no staging file collision | Generation/owner logs | P0 |
| `P18-WIF-17-03` | Critical update arrives during large low-priority job | Submit high priority job | Preemption only at safe boundary; no prefix corruption | Pause trace | P0 |
| `P18-WIF-17-04` | Priority starvation from endless urgent updates | Flood high-priority queue | Apply fairness/aging policy | Waiting-time histogram | P0 |
| `P18-WIF-17-05` | Duplicate signed artifact for different target | Enqueue same bytes for two targets | Target validation isolated; dedup immutable bytes only by policy | Target hashes | P0 |
| `P18-WIF-17-06` | Candidate superseded but response still streams | Keep old curl callback running | Epoch-fence and no writes to new stage | Old callback count | P1 |
| `P18-WIF-17-07` | Queue full while receiving critical update | Fill 128 job slots | Explicit admission policy, no memory spike | Rejected requests | P1 |
| `P18-WIF-17-08` | Many jobs target same verified content hash | Trigger simultaneous promotions | Safe lock on content address; correct references | Artifact DB | P1 |
| `P18-WIF-17-09` | Cancel one job sharing verified blob | Cancel consumer job | Shared immutable artifact not deleted while referenced | Reference counts | P1 |
| `P18-WIF-17-10` | Priority update arrives while no trusted time | Submit high priority during time WAIT | Security gate still applies, not bypassed by priority | Trust check | P0 |

### 18. Thread races, callbacks & ownership

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-18-01` | Cancel and complete occur simultaneously | Barrier two worker threads | Exactly one terminal outcome | State revision | P0 |
| `P18-WIF-18-02` | Retry timer and network recovery fire same tick | Fake clock simultaneous callbacks | Schedule at most one GET | Request count | P0 |
| `P18-WIF-18-03` | I/O callback runs after object destruction | Delay callback past shutdown | Lifetime guard blocks use-after-free | ASan run | P0 |
| `P18-WIF-18-04` | Config reload while callback reads policy | Swap config snapshots | Callback uses consistent version | Config generation | P0 |
| `P18-WIF-18-05` | Hash worker and cleanup remove same file | Trigger concurrent failure+cleanup | Single file owner, no FD reuse bug | TSan/FD trace | P0 |
| `P18-WIF-18-06` | Two threads update durable_offset | Parallel checkpoint requests | Monotonic, serialized transaction | Offset history | P0 |
| `P18-WIF-18-07` | Observer callback recursively calls cancel() | Reentrant action from status listener | No lock recursion deadlock | Deadline test | P0 |
| `P18-WIF-18-08` | Slow observer blocks coordinator | Sleep observer 5 seconds | Isolate bounded notification queue | Dispatcher latency | P0 |
| `P18-WIF-18-09` | Shutdown and incoming offer race | Barrier enqueue with Stop | Admission boundary deterministic | Accepted/denied trace | P0 |
| `P18-WIF-18-10` | Worker throws C++ exception mid-download | Throw from mock crypto/storage | Convert to bounded failure; no std::terminate | Process survival | P0 |

### 19. Pause, resume, cancel & shutdown lifecycle

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-19-01` | Pause request at first byte | Signal pause during write | Stop IO safely, durable PAUSED or pending state | Offset/status | P0 |
| `P18-WIF-19-02` | Pause called twice | Duplicate operation ID | Idempotent result, no duplicate fsync | Transition count | P0 |
| `P18-WIF-19-03` | Resume while already DOWNLOADING | Invoke resume many times | No new parallel download | Curl active count | P0 |
| `P18-WIF-19-04` | Cancel during VERIFYING | Cancel verification immediately before result | Respect cancellation barrier; not READY afterwards | Terminal history | P0 |
| `P18-WIF-19-05` | Cancel after READY handed to P19 | Try cancel completed job | Reject/return no-op, do not revoke installer unexpectedly | Handoff contract | P0 |
| `P18-WIF-19-06` | Stop(DRAIN) and slow GET | Delay body indefinitely | Bounded drain deadline, safe checkpoint | Stop duration | P1 |
| `P18-WIF-19-07` | Stop(DISCARD) while fsync | Force shutdown at flush boundary | Preserve durable prefix according to policy | Restart recovery | P1 |
| `P18-WIF-19-08` | Resume while low-battery policy DENY | Request resume manually | WAITING_POLICY, no GET | Power policy trace | P1 |
| `P18-WIF-19-09` | Pause while retry timer pending | Trigger timer and pause simultaneously | No GET after durable PAUSED | GET count | P1 |
| `P18-WIF-19-10` | Repeated START/STOP cycles | Loop 1000 service lifecycle | No leaked threads/FD and consistent status | RSS/FD/TSan | P0 |

### 20. Power constraints, thermal and vehicle policy

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-20-01` | Battery below configured threshold | Fake P22 battery critical | Pause/deny new download | Policy reason | P0 |
| `P18-WIF-20-02` | Battery crosses threshold repeatedly | Flap near 20% | Hysteresis prevents oscillating curl requests | State transitions | P0 |
| `P18-WIF-20-03` | Vehicle ignition OFF | Toggle ignition while GET | Continue/pause only as explicit OEM simulator policy | Config audit | P0 |
| `P18-WIF-20-04` | Low battery during fsync | Inject power deny at checkpoint | Complete safe boundary then quiesce | Durable offset | P0 |
| `P18-WIF-20-05` | Overheating TCU | Fake thermal alert | Throttle/pause transfer per policy | Throttle metrics | P0 |
| `P18-WIF-20-06` | Metered LTE budget reached | Simulate data quota exhaustion | PAUSED/WAITING_POLICY, no extra bytes charged | Network byte ledger | P1 |
| `P18-WIF-20-07` | Power policy returns UNKNOWN/stale | Remove vehicle signal freshness | Conservative decision per risk policy | Quality status | P1 |
| `P18-WIF-20-08` | OTA install busy from P19 | Trigger install-active event | P18 respects shared resource/lock policy | Admission trace | P1 |
| `P18-WIF-20-09` | Emergency diagnostic command arrives | Inject high-priority work from P20 | No interference; fair bounded CPU and IO | Latency stats | P1 |
| `P18-WIF-20-10` | Ignition signal flaps during final publish | Flip policy at rename boundary | Publish decision consistent with explicit policy gate | Health/power snapshot | P0 |

### 21. Storage quota, retention & cleanup

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-21-01` | Disk nearly full before download | Pre-fill filesystem to reserve threshold | WAITING_STORAGE without allocating full file | Disk usage | P0 |
| `P18-WIF-21-02` | ENOSPC on data write | Fill disk mid-transfer | No false READY; safe retry if disk freed | fs error & DB | P0 |
| `P18-WIF-21-03` | EDQUOT per-user reached | Apply project quota / fake quota | Same handling as storage pressure | Quota status | P0 |
| `P18-WIF-21-04` | Quota reservation shared by jobs | Start two jobs with limited space | Atomic reservation; no over-commit | Reserved totals | P0 |
| `P18-WIF-21-05` | Cleanup selects running stage incorrectly | Run cleanup under active download | Active stage protected | File existence | P0 |
| `P18-WIF-21-06` | Cleanup deletes verified artifact referenced by P19 | Mark artifact pinned and sweep | Keep until explicit reference release | Pin count | P1 |
| `P18-WIF-21-07` | Stale quarantine grows indefinitely | Generate 1000 invalid packages | Bounded quarantine TTL/quota, auditable drops | Directory size | P1 |
| `P18-WIF-21-08` | Filesystem inode exhaustion | Fill inode table using tiny files | Reject gracefully despite free bytes | ENOSPC inode evidence | P1 |
| `P18-WIF-21-09` | Slow disk causes queue backlog | Inject 5s fsync delay | Backpressure and CPU bounded | Queue and latency | P1 |
| `P18-WIF-21-10` | Partial stale job after crash | Leave orphan .part with no DB row | Reconcile/quarantine with bounded cleanup | Orphan report | P0 |

### 22. Memory, CPU, file descriptors & pressure

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-22-01` | Hundreds of simultaneous offers | Load test enqueue many jobs | Bounded request queue and RSS | Resource ceiling | P0 |
| `P18-WIF-22-02` | HTTP response chunk 100MiB | Fixture sends giant chunk | Stream bounded write buffer, no giant allocation | Heap peak | P0 |
| `P18-WIF-22-03` | 100k status subscribers | Flood local IPC observers | Bounded notification fan-out | Subscriber stats | P0 |
| `P18-WIF-22-04` | FD limit = 64 | Set ulimit then start | Report resource pressure without spin | Open FD count | P0 |
| `P18-WIF-22-05` | SIGPIPE on peer close | Close socket mid-send | Process survives; error mapped | Exit status | P0 |
| `P18-WIF-22-06` | Hashing large artifact competes with telemetry | Saturate CPU hash workers | Yield/limit workers, protect TCU QoS | CPU and telemetry latency | P0 |
| `P18-WIF-22-07` | Thread pool count misconfigured 0 | Inject config 0 threads | Reject config or safe fallback, no deadlock | Config validation | P0 |
| `P18-WIF-22-08` | Disk+network throughput exceed bounded buffers | Send data faster than storage | Apply libcurl pause/backpressure | RSS and write queue | P0 |
| `P18-WIF-22-09` | Memory allocation fails | Fault allocator at random points | Fail current job safely; no exception escape | Allocation fault logs | P0 |
| `P18-WIF-22-10` | Repeated timers accumulate across reconnects | 10000 reconnect cycles | No timer/heap leak; cancel old generations | Timer count | P0 |

### 23. Security credentials, authorization & exposure

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-23-01` | Bearer token printed by HTTP debug | Enable verbose logging with token | Redaction prevents secret disclosure | Redaction scanner | P0 |
| `P18-WIF-23-02` | Authorization header forwarded to mirror | Redirect to new origin | Do not forward absent explicit allowlist | Mirror log | P0 |
| `P18-WIF-23-03` | Token expires during body transfer | Server closes on expiration | Refresh only approved channel; no password logs | Auth audit | P0 |
| `P18-WIF-23-04` | mTLS key rotates during transfer | Rotate device cert | New attempt uses valid credential; old session fenced | TLS epoch | P0 |
| `P18-WIF-23-05` | Malicious URL uses file:// | Inject scheme via metadata | Reject non-HTTPS before libcurl | No file access | P0 |
| `P18-WIF-23-06` | URL includes userinfo credentials | https://user:pass@host | Reject or strip per explicit policy; no logs | URL parser audit | P1 |
| `P18-WIF-23-07` | HTTP response contains malicious Set-Cookie | Return cookie attempting persistence | No trusted security side effect from unneeded cookies | Cookie store empty | P1 |
| `P18-WIF-23-08` | Remote data reused as shell command | Put `$(touch ...)` in filename | Never execute; staging paths generated locally | Sandbox file audit | P1 |
| `P18-WIF-23-09` | Untrusted environment proxy variables | Set HTTPS_PROXY to attacker fixture | Explicit proxy policy rejects unexpected proxy | Effective connection path | P1 |
| `P18-WIF-23-10` | TLS verification disabled by bad config | Set insecure flag at runtime | Configuration rejected in secured profile | libcurl options assertion | P0 |

### 24. Payload integrity, digest & bit corruption

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-24-01` | Single byte corrupted in received stream | Flip byte after HTTP body | SHA mismatch, no READY | Independent full digest | P0 |
| `P18-WIF-24-02` | Storage bit rot after fsync | Mutate stage before VERIFYING | Full rehash fails | Hash oracle | P0 |
| `P18-WIF-24-03` | Content matches size but wrong release | Supply same-size different blob | Signed digest mismatch | Digest | P0 |
| `P18-WIF-24-04` | Hash comparison is case/format malformed | Uppercase/truncated hash metadata | Normalize strictly/reject invalid hex | Hash parser tests | P0 |
| `P18-WIF-24-05` | Fake SHA collisions supplied in test | Override mock hasher to collide | Security test flags mock-only; real hash required in integration | Crypto oracle mode | P0 |
| `P18-WIF-24-06` | File changes after verification before rename | Race write to staging file | Seal/check FD identity and rehash at boundary | inode and digest | P0 |
| `P18-WIF-24-07` | Chunk-level checksum pass, final digest fails | Swap two chunks same size | Final digest governs READY | Full hash | P0 |
| `P18-WIF-24-08` | Trusted hash changed by config reload | Hot reload manifest hash field | Immutable signed snapshot; no partial mutation | Manifest hash | P0 |
| `P18-WIF-24-09` | TOCTOU: verified path replaced after Ready | Swap symlink/mount in sandbox | P19 handoff checks trusted immutable identity | inode/hash validation | P0 |
| `P18-WIF-24-10` | Payload truncated after READY | Delete last block in verified file | P19 re-verifies, P18 recovery marks repair | Post-ready digest | P0 |

### 25. CDN, mirrors, object revisions & caching

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-25-01` | Mirror A and B serve different bytes | Switch mirror mid-transfer | Never merge without strong identity+final hash | Both mirror digests | P0 |
| `P18-WIF-25-02` | CDN stale cache returns prior image | Return old revision with 200 | Final signed digest fails | Manifest correlation | P0 |
| `P18-WIF-25-03` | CDN returns weak validator | Header ETag: W/"x" | Restart from zero rather than unsafe resume | Range request capture | P0 |
| `P18-WIF-25-04` | Intermediate proxy transforms bytes | Proxy gzip/rewrite | Reject mismatching representation | HTTP trace | P0 |
| `P18-WIF-25-05` | Origin object replaced while transfer underway | Mutate server file at half-download | Final digest fails; no READY | Object versions | P0 |
| `P18-WIF-25-06` | Mirror lacks Range support | Second endpoint always sends 200 | Restart clean on new mirror | Stage truncation | P0 |
| `P18-WIF-25-07` | Mirror list contains untrusted host | Add malicious mirror to response | Only signed/configured allowlist allowed | Egress capture | P0 |
| `P18-WIF-25-08` | Mirror returns matching strong ETag but wrong bytes | Forge validator in fixture | Full SHA still rejects | Hash mismatch | P0 |
| `P18-WIF-25-09` | CDN serves 206 from wrong cached revision | Serve mismatched range body | Final hash catches, quarantine | Mixed bytes fixture | P0 |
| `P18-WIF-25-10` | Mirror fails over during trusted-time outage | Trigger failover when clock UNKNOWN | Don't bypass metadata validity | Time gate and request count | P0 |

### 26. Logs, metrics, status reporting & audit

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-26-01` | Progress event lost on Event Bus | Drop one progress notification | Durable job state still correct | DB vs events | P0 |
| `P18-WIF-26-02` | Duplicate status after retry | Replay notifications | Consumers dedup by revision/generation | Event IDs | P0 |
| `P18-WIF-26-03` | Progress reaches 100% before hash verified | Complete HTTP download then corrupt hash | UI shows DOWNLOADED/VERIFYING not READY | Status trace | P0 |
| `P18-WIF-26-04` | Status callback returns late from old generation | Delay callback | Fence stale status, not overwrite new state | Revision monotonic | P0 |
| `P18-WIF-26-05` | Logs store signed URLs with query token | Use tokenized mock URL | Redact query secrets by default | Secret scanner | P0 |
| `P18-WIF-26-06` | Metrics counters overflow | Seed counters near 64-bit limit | Saturate/error safely; no wrap to negative | Metrics assertions | P0 |
| `P18-WIF-26-07` | Exporter crashes | Kill telemetry exporter during transfer | Download continues; health alert | Process status | P0 |
| `P18-WIF-26-08` | Wall clock jumps during duration computation | Change system time | Latency uses monotonic clock | Timing trace | P0 |
| `P18-WIF-26-09` | Too many error logs under retry storm | Inject 10000 network errors | Rate-limit/queue logs bounded | Drop metrics + RSS | P0 |
| `P18-WIF-26-10` | Trace ID reused across jobs | Deliberately reuse caller trace ID | Job ID still separates audit trails | Correlation verification | P0 |

### 27. P18→P19 artifact handoff & ownership

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-27-01` | P19 requests artifact before READY | IPC request while VERIFYING | Deny; never expose staged path | Handoff log | P0 |
| `P18-WIF-27-02` | Handoff ACK lost after P19 accepted | Drop IPC response | Idempotent reference retrieval, no duplicate flash instruction | P19 mock journal | P0 |
| `P18-WIF-27-03` | P19 opens path while cleanup runs | Barrier concurrent operations | Pin/FD ownership blocks deletion | Ref-count trace | P0 |
| `P18-WIF-27-04` | P19 changes artifact after handoff | Attempt write to verified file | Deny or detect tamper before installer uses | File permissions/checksum | P0 |
| `P18-WIF-27-05` | P19 asks for wrong target ECU | Supply mismatched target | Binding validation rejects | Target oracle | P0 |
| `P18-WIF-27-06` | P19 crashes after receiving artifact | Kill installer process | Artifact remains available, no false install success | P19 audit | P0 |
| `P18-WIF-27-07` | P19 rejects outdated metadata | Revoke release before handoff | P18 handles rejected handoff, no READY→INSTALLED claim | Handoff status | P0 |
| `P18-WIF-27-08` | P19 asks same artifact repeatedly | Repeated query 100 times | Stable identity, no duplicate promotion | Artifact ID | P0 |
| `P18-WIF-27-09` | P19 consumes same artifact while new download starts | Download unrelated job | Immutable verified file unchanged | Before/after SHA | P0 |
| `P18-WIF-27-10` | P18 restart while P19 file open | Restart download service | Verified identity stable, P19 owns safe handle | Open FD and hash | P0 |

### 28. Test harness, fault verification & false PASS prevention

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-28-01` | Fault injector targets wrong interface | Apply netem to inactive veth | Mark INCONCLUSIVE, not PASS | Injection receipt | P0 |
| `P18-WIF-28-02` | Test thinks fsync failed but mock not enabled | Run case with disabled fault point | INCONCLUSIVE with reason | Fault-hit counter | P0 |
| `P18-WIF-28-03` | Expected hash oracle uses same buggy function | Mutate both app and test hash stub | Independent sha256sum fixture required | External digest | P0 |
| `P18-WIF-28-04` | Test ends before late callback | Delay callback until after assertion | Wait configured observation window | Event timeline | P0 |
| `P18-WIF-28-05` | Seed not recorded | Run random netem without seed | Block reproducibility claim | Report metadata | P0 |
| `P18-WIF-28-06` | Clock advanced but timer worker not woken | Fake clock skips wake signal | Test INVALID/INCONCLUSIVE, not pass | Clock/timer acknowledgement | P0 |
| `P18-WIF-28-07` | Baseline already failing | Start with broken HTTPS server | Do not attribute baseline failure to injected fault | Baseline verdict | P0 |
| `P18-WIF-28-08` | Harness kills wrong PID | Inject SIGKILL into helper process | Mark inconclusive; verify process identity | PID namespace receipt | P0 |
| `P18-WIF-28-09` | Fault cleanup fails leaving qdisc | Force teardown error | FAIL harness/isolate next run; no stale environment | tc qdisc diff | P0 |
| `P18-WIF-28-10` | No evidence for rollback/READY state | Delete job DB snapshot before verdict | INCONCLUSIVE, no inferred PASS | Evidence manifest | P0 |

### 29. Hostile HTTP, metadata fuzzing & abuse cases

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-29-01` | Content-Range integer overflow | Send bytes 18446744073709551616-... | Reject parsing before arithmetic | Sanitizer output | P0 |
| `P18-WIF-29-02` | ETag contains embedded CRLF | Craft raw server header | Protocol layer rejects/sanitizes | Network and parser traces | P0 |
| `P18-WIF-29-03` | Large URL with Unicode ambiguities | Craft 100KB URL/homograph host | Bounded validation, canonical host policy | Parser test | P0 |
| `P18-WIF-29-04` | Millions of empty chunk boundaries | Chunked transfer pathological | Bound CPU/deadline | CPU and timeout | P0 |
| `P18-WIF-29-05` | Invalid UTF-8 in metadata | Signed invalid bytes under fixture key | Parser deterministic reject | Fuzz corpus | P0 |
| `P18-WIF-29-06` | CBOR/JSON length mismatch | Inject malformed byte length | Reject without out-of-bounds reads | ASan fuzz | P0 |
| `P18-WIF-29-07` | Multipart/byteranges for single range | Return multipart 206 payload | Reject if unsupported; never write multipart markers | Header test | P0 |
| `P18-WIF-29-08` | HTTP/2 stream reset mid-frame | RST_STREAM at payload byte | Retry safe prefix, no READY | HTTP2 trace | P0 |
| `P18-WIF-29-09` | HTTP 200 with contradictory final trailers | Send trailer conflicting with header | Fail according to strict supported profile | Response validator | P0 |
| `P18-WIF-29-10` | Fuzz response order: headers after body | Protocol fixture sends illegal ordering | Reject, no body write before valid header | Write callback counter | P0 |

### 30. Soak, fleet load, lifecycle & compound chaos

| ID | What if? | Cách gây lỗi (inject) | Behavior mong đợi | Evidence / oracle | Priority |
|---|---|---|---|---|---|
| `P18-WIF-30-01` | 24h steady 100 jobs/day | Simulate long campaign with random offsets | No unbounded RSS/FD/WAL growth | Resource graphs | P0 |
| `P18-WIF-30-02` | 1000 virtual vehicles receive update offer | Run isolated client instances | Fair quotas and unique job identities | Fleet metrics | P0 |
| `P18-WIF-30-03` | Repeated power cuts over same artifact | Crash every 1MiB progress | Eventually READY only with verified full image | Recovery trace | P0 |
| `P18-WIF-30-04` | Flapping network for 12h | netem periodic drop/recover | Retry bounded, no queue explosion | Attempt histogram | P0 |
| `P18-WIF-30-05` | Disk fill/empty loops | Inject ENOSPC and reclaim repeatedly | No orphan explosion, coherent progress | Disk directory audit | P0 |
| `P18-WIF-30-06` | Daily certificate rotations | Rotate CA/leaf in lab | Always verify peer; no insecure fallback | TLS audit | P1 |
| `P18-WIF-30-07` | Content-addressed store accumulates references | Many jobs same image, pin/unpin | Refcounts and garbage collection consistent | DB and file count | P1 |
| `P18-WIF-30-08` | Random concurrency scheduler seed | 1000 seeds for pause/cancel/retry | No double terminal, no TSan errors | Seeded traces | P1 |
| `P18-WIF-30-09` | Reboot during migration and download | Kill while schema migrates | Recovery guarded, not corrupt active artifacts | Schema/version check | P1 |
| `P18-WIF-30-10` | Chaos campaign injects 5 simultaneous faults | Combine net loss+disk full+SIGKILL+metadata revoke+ACK loss | Fail closed, no false READY, evidence complete | Independent oracles | P0 |

## 12. Multi-fault campaigns — 50 chiến dịch

Không giả định mọi fault độc lập: các test dưới đây ép nhiều fault xảy ra gần nhau, có thứ tự timeline để tái hiện. `T0/T1/T2...` dùng đồng hồ mô phỏng; tại mỗi mốc fault runner phải ghi `inject_receipt` và log state revisions. Mỗi campaign phải đối chiếu với một **baseline không inject** trước khi chạy. Đặc biệt, `ACK` mất có thể khiến *kết quả quan sát* không chắc chắn; không được suy diễn thành success/failure giả.

| ID | Campaign | Timeline / injection | Expected behavior | Invariants |
|---|---|---|---|---|
| `P18-MF-01` | Network loss + old callback + resume | T0 GET half; T1 cut network; T2 reconnect; T3 delayed old body callback | Old generation ignored; staged bytes match signed image; one active transfer | `INV-02,INV-04,INV-07` |
| `P18-MF-02` | HTTP 200 on resume + SIGKILL while reset | T0 durable prefix; T1 server ignores range; T2 crash during truncate/reset | Reconcile to safe zero or known prefix, never append full body to old prefix | `INV-03,INV-04` |
| `P18-MF-03` | HTTP 206 wrong range + malicious ETag | T0 partial; T1 return 206 start+1 and forged strong ETag | Reject before write and no new durable_offset | `INV-05,INV-03` |
| `P18-MF-04` | HTTP 416 + partial corruption + disk nearly full | T0 prefix claims final length; T1 alter byte; T2 reply 416 | Rehash fails, no READY, disk bounded | `INV-02,INV-10` |
| `P18-MF-05` | CDN mirror switch + object revision + ACK loss | T0 from mirror A; T1 switch B/new revision; T2 response lost | No cross-revision concatenation, final hash remains authoritative | `INV-02,INV-04` |
| `P18-MF-06` | TLS expiry + DNS rebinding + retry | T0 valid connection; T1 rotate expired certificate; T2 DNS points local service | No TLS bypass or local credential leakage | `INV-09` |
| `P18-MF-07` | Redirect to HTTP + Authorization header + cloud offline | T0 signed offer; T1 untrusted redirect to HTTP; T2 connectivity lost | Reject redirect, zero leaked credentials | `INV-09,INV-15` |
| `P18-MF-08` | Secure-time lost + manifest expires + transfer completes | T0 start valid; T1 time attestation lost; T2 bytes complete | Do not promote READY without applicable trustworthy time/policy | `INV-01,INV-15` |
| `P18-MF-09` | Trust key revocation + final hash pass + callback old | T0 valid metadata; T1 revoke key; T2 completion from pre-revoke attempt | Trust re-evaluation blocks disallowed READY | `INV-01,INV-07` |
| `P18-MF-10` | Target mismatch offer + high priority + capacity full | T0 full queue; T1 forged priority critical target mismatch | Security reject precedes queue priority preemption | `INV-01,INV-06` |
| `P18-MF-11` | Disk ENOSPC + WAL busy + network reconnect | T0 storage almost full; T1 DB busy; T2 Internet returns | No false durability, bounded retries/backpressure | `INV-03,INV-10` |
| `P18-MF-12` | File fsync fails + DB commit succeeds accidentally | T0 write; T1 fsync EIO; T2 force mock DB commit | Reconciliation detects invalid offset; never READY | `INV-03,INV-08` |
| `P18-MF-13` | Crash after fsync + DB offset stale + GET resumes | T0 write larger prefix; T1 crash pre-checkpoint; T2 resume | Truncate to trusted offset and safely redownload | `INV-03,INV-04` |
| `P18-MF-14` | Crash after verified rename + DB READY absent | T0 complete verify; T1 rename+dir fsync; T2 crash before DB commit | Recover by independent hash/metadata check; no duplicate promotion | `INV-02,INV-08` |
| `P18-MF-15` | DB says READY + file missing + P19 handoff request | T0 persist READY; T1 delete file; T2 P19 requests | Reject handoff and mark repair-required | `INV-08,INV-15` |
| `P18-MF-16` | Two jobs same target + one cancel + late body | T0 concurrent offers; T1 cancel owner; T2 old worker callback | Exactly one writer; cancelled worker cannot mutate new job | `INV-06,INV-07` |
| `P18-MF-17` | Priority supersession + unsigned offer + network down | T0 valid download; T1 unsigned high-priority offer; T2 network loss | Original job preserved; invalid supersession rejected | `INV-01,INV-06` |
| `P18-MF-18` | Pause + timeout + completed GET race | T0 worker near last byte; T1 pause and timeout same tick | Single terminal state, no post-pause write | `INV-11,INV-14` |
| `P18-MF-19` | Cancel + SHA verifier completes + handoff IPC | T0 verify running; T1 cancel; T2 verifier returns; T3 P19 polls | Publish only if finalization barrier won before cancellation | `INV-08,INV-11` |
| `P18-MF-20` | Shutdown + network recovered + retry timer fires | T0 waiting retry; T1 Stop; T2 link-up and timer | No new GET after Stop barrier, no dangling callbacks | `INV-07,INV-14` |
| `P18-MF-21` | Low battery + data budget exhausted + offer urgent | T0 battery low; T1 metered budget 0; T2 critical offer | Policy denies/waits; priority cannot bypass gates | `INV-13` |
| `P18-MF-22` | Ignition stale + thermal overheat + live download | T0 loss of vehicle signals; T1 heat critical; T2 body arrives | Conservative pause and safe checkpoint | `INV-13,INV-03` |
| `P18-MF-23` | CPU overload + slow filesystem + network storm | T0 throttle CPU; T1 fsync delay; T2 send 100kB/s events | Bounded queues/RSS, priority telemetry protected | `INV-10` |
| `P18-MF-24` | Many jobs + quota race + cleanup | T0 two jobs reserve last free bytes; T1 cleanup orphan; T2 writers continue | Reservations serialized; no deletion of active stage | `INV-06,INV-12` |
| `P18-MF-25` | Quarantine growth + invalid hash corpus + log pressure | T0 100 invalid artifacts; T1 log flood | Bounded disk/logs and no READY | `INV-02,INV-10` |
| `P18-MF-26` | Malicious symlink + early callback + restart | T0 swap stage symlink; T1 crash; T2 old callback arrives | Never write outside staging and stale callback fenced | `INV-07,INV-12` |
| `P18-MF-27` | Wrong manifest rev + trust-store stale + old DB snapshot | T0 update revocations; T1 roll back DB/store; T2 old offer | Reject anti-rollback violation, repair required | `INV-01` |
| `P18-MF-28` | Signed manifest valid + URL token leaked in redirect | T0 secure manifest; T1 redirect to other hostname; T2 enable verbose log | Header/token redacted; redirect policy enforced | `INV-09` |
| `P18-MF-29` | Proxy changes Content-Encoding + range mismatch + hash fail | T0 partial; T1 proxy gzip with 206; T2 EOF | Reject unsupported transformed representation before append | `INV-04,INV-05` |
| `P18-MF-30` | Malformed HTTP headers + giant response + OOM pressure | T0 huge headers; T1 payload > size; T2 allocator fails | Resource bounds and no READY | `INV-02,INV-10` |
| `P18-MF-31` | Retry after 429 + RTC jump + reboot | T0 Retry-After delay; T1 wall clock changes; T2 reboot | Deadline/retry state reconciled, no spin storm | `INV-10` |
| `P18-MF-32` | Handshake timeout + old TLS callback + new cert | T0 handshake stall; T1 retry with new cert; T2 old completion | Fence old epoch, verify new peer | `INV-07,INV-09` |
| `P18-MF-33` | Hash success + manifest expires before promotion + fsync delay | T0 digest good; T1 slow fsync; T2 expire metadata | Re-evaluate policy before READY | `INV-01,INV-08` |
| `P18-MF-34` | Handoff ACK lost + P19 restart + content GC | T0 P19 accepted artifact; T1 lost ACK; T2 installer restarts; T3 sweep | Artifact remains immutable/pinned; handoff idempotent | `INV-08,INV-12` |
| `P18-MF-35` | Telemetry exporter down + download ready + status retransmit | T0 kill exporter; T1 complete verify; T2 reconnect | Durable job state authoritative; no false update installed | `INV-08,INV-15` |
| `P18-MF-36` | Config hot reload + active transfer + ETag change | T0 active GET; T1 reload limits; T2 ETag changes | Consistent config epoch; restart safe | `INV-04,INV-07` |
| `P18-MF-37` | Worker exception + observer reentrant cancel + Stop | T0 mock throws; T1 status observer cancels; T2 Stop | No terminate/deadlock/double terminal | `INV-11,INV-14` |
| `P18-MF-38` | Storage power cut + SQLite WAL normal mode + hot reboot | T0 degrade sync mode; T1 power cut; T2 recovery | Startup refuses unsafe profile or marks unknown; no false durable claims | `INV-03,INV-15` |
| `P18-MF-39` | Read-only filesystem + corrupt DB + duplicate offer | T0 force EROFS; T1 corrupt DB; T2 send duplicate | REPAIR_REQUIRED; no staging writes/new READY | `INV-03,INV-15` |
| `P18-MF-40` | Same SHA artifact + different target + concurrent promotion | T0 offer same digest for two target IDs; T1 complete simultaneously | Target policy isolation; immutable content store safe | `INV-01,INV-06` |
| `P18-MF-41` | Cancellation while full verified file exists + P19 polling | T0 verify complete; T1 cancel at promotion barrier; T2 P19 polls | Strict barrier: either READY valid or CANCELLED inaccessible | `INV-08,INV-11` |
| `P18-MF-42` | NTP false future + short-lived manifest + modem reset | T0 valid; T1 untrusted RTC jump; T2 modem reset | Do not promote based on untrusted clock | `INV-01` |
| `P18-MF-43` | Slowloris HTTPS + timer overflow + reconnect spam | T0 drip bytes; T1 huge retry exponent; T2 link flapping | Timeouts and bounded retry, CPU stable | `INV-10` |
| `P18-MF-44` | Untrusted mirror + SSRF redirect + private network exposed | T0 wrong mirror; T1 redirect to link-local; T2 credentials present | Never contact disallowed endpoint or leak headers | `INV-09` |
| `P18-MF-45` | Fuzz corpus + sanitizer + process restart | T0 1000 malformed metadata/responses; T1 crash process mid-set | No memory safety errors, recovery correctness | `INV-10,INV-14` |
| `P18-MF-46` | Multiple download jobs + 24h soak + random kill | T0 start fleet; T1 periodic netem; T2 random SIGKILL/restart | No false READY, durable offsets safe, resources bounded | `INV-02,INV-03,INV-10` |
| `P18-MF-47` | Final digest mismatch + orphan cleanup + mirror retry | T0 corrupted blob; T1 quarantine; T2 cleanup and new attempt | No reuse of untrusted prefix; verified store unchanged | `INV-02,INV-12` |
| `P18-MF-48` | Low storage + update revocation + P19 installed earlier image | T0 artifact pinned; T1 revoke new release; T2 GC quota | Never erase existing installed/known-good image; no handoff revoked | `INV-01,INV-12` |
| `P18-MF-49` | Partial resume + new signed metadata same URL + old callback | T0 download version A; T1 signed offer B same URL; T2 A callback arrives | Separate generations and content identity; never mix A/B | `INV-04,INV-07` |
| `P18-MF-50` | Five independent faults at once | T0 net loss; T1 disk full; T2 cert expired; T3 SIGKILL; T4 P19 polls | Correct conservative state, no false READY, complete verifiable evidence | `INV-01,INV-03,INV-08,INV-09` |

### 12.1 Dữ liệu chứng minh cho mỗi campaign

1. **Injection evidence:** PID / namespace / target file or socket / faultpoint / timestamp (monotonic) / exact return code / observed fault-hit count.
2. **Protocol evidence:** sanitized HTTP request + response status, ETag, Content-Range, transfer epoch, attempted bytes, abort reason.
3. **Storage evidence:** SQLite state+revision+durable_offset, staged file length/hash, final artifact digest, rename/fsync receipts, repair decision.
4. **Concurrency evidence:** thread/callback IDs, generation, timer state, sanitized logs, ASan/TSan reports when applicable.
5. **Verdict:** `PASS`, `FAIL`, `INCONCLUSIVE`, `SKIPPED` + reason; a case cannot PASS merely because expected result appeared without the fault actually being injected.

### 12.2 Một fault có nhiều possible outcomes

- **Network interruption:** dữ liệu đã gửi đến client nhưng chưa checkpoint có thể phải tải lại; điều đó hợp lệ, nhưng `durable_offset` không được báo cao hơn an toàn.
- **Power cut between DB and file operation:** có thể tồn tại staged/orphan/verified file chưa được DB index. Recovery phải đối soát bằng manifest+hash+generation thay vì lấy một bên làm sự thật tuyệt đối.
- **Cancel vs. READY race:** nếu cancellation arrives *after* durable promotion barrier thì trả `ALREADY_READY`; nếu trước barrier thì không handoff. Thứ tự phải đo từ serialized coordinator, không từ wall clock.
- **Consumer ACK loss:** P18 chỉ xác định `artifact verified / handed off`, không thể suy ra P19 đã cài đặt firmware.

## 13. Test strategy & coverage-to-requirement matrix

### 13.1 Test pyramid

| Test tier | Công cụ | Bằng chứng bắt buộc |
|---|---|---|
| Unit / deterministic | Google Test, Mock, fake clock, fake FS | Transition table, no wrong READY; reproducible seeds |
| Property-based / fuzz | libFuzzer, ASan, UBSan, malformed HTTP/JSON | No OOB, integer overflow, unsafe allocation or parser inconsistency |
| Integration | HTTPS test server, libcurl, SQLite, P13/P19 mocks | Real 200/206/416 protocol trace + independent hash |
| Linux network | P11 netns + veth + `tc netem` | Fault injection receipt, route/link direction, no host leakage |
| Crash / storage | SIGKILL, SQLite VFS/fault storage mock; power-cut model | Atomicity and recovery outcomes across crashpoints |
| Security | TLS CA fixtures, redirect/SSRF stubs, manifest corpus | Fail-closed, credential-redaction, target binding |
| Concurrency | TSan-capable build separately from ASan, barrier scheduler | One terminal state, no deadlock, UAF, data race |
| Soak | 24h controlled job load, metrics snapshots | bounded RSS/FD/disk/timers and recovery correctness |

### 13.2 Test traceability (mỗi nhóm 10 case)

| Requirement / invariant | Scenarios | Required verification |
|---|---|---|
| FR-01/FR-12 idempotency | `P18-WIF-01` | Same key same job; changed offer conflict |
| FR-02 metadata security | `P18-WIF-02–05` | Independent verifier, trusted time, target binding |
| FR-04 HTTPS transport | `P18-WIF-06–07,09–10` | TLS peer, redirects, header and body limits |
| FR-05 range resume | `P18-WIF-08,25` | Byte-accurate 200/206/416, strong ETag, SHA-256 |
| FR-07 retry & deadline | `P18-WIF-11–12` | Monotonic fake clock and bounded requests |
| FR-06/FR-11 durability | `P18-WIF-13–16` | Checkpoint/rename crashpoint matrix |
| FR-09 resource bounds | `P18-WIF-17,21–22` | Quota and resource high-watermarks |
| FR-12 concurrency lifecycle | `P18-WIF-18–19` | Schedule barriers, sanitizers |
| FR-08 power and vehicle policy | `P18-WIF-20` | P22/P10 policy fake, no GET while denied |
| FR-10 integrity | `P18-WIF-24` | Independent complete SHA-256 |
| FR-13 status/secret redaction | `P18-WIF-23,26` | Scanner, dedup status revision |
| FR-18 handoff | `P18-WIF-27` | P19 mock rejects unverified/unsafe artifact |
| Fault harness fidelity | `P18-WIF-28–29` | Fault receipts, negative corpus |
| Reliability & soak | `P18-WIF-30` | 24-hour resource trend + deterministic chaos |

### 13.3 Golden vectors cho HTTP resume

| Request | Server response | Điều kiện bắt buộc | Expected |
|---|---|---|---|
| `GET` no Range | `200`, identity body size S | Manifest preverified; size<=S | Persist staged bytes; final hash verify |
| `Range: bytes=4-`, `If-Range: "rev1"` | `206`, `Content-Range: bytes 4-9/10` | Existing trustworthy prefix length 4; strong validator | Append exactly 6 bytes, then verify full 10 bytes |
| Range as above | `200`, full body | Server ignored range / validator mismatch | Restart clean from zero, not append |
| Range as above | `206`, `Content-Range: bytes 5-9/10` | Requested start was 4 | Reject, write 0 bytes |
| Range as above | `416`, `Content-Range: bytes */10` | Staged size already 10 | Rehash complete file; 416 never proves success |
| Range as above | `206`, encoded gzip | Representation transformed | Reject incompatible representation |

**Strict coding note:** signed manifest `expected_size` and `expected_hash` are cryptographic sources of truth, not `Content-Length`, ETag or mutable HTTP headers. Verify no body is written before validating status and range headers. For `200` range-ignored recovery, transactionally invalidate old checkpoint/generation before truncating or recreating staging; startup reconciliation must handle interruption between these steps.

### 13.4 Minimal BDD examples

```gherkin
Scenario: Resume cannot mix artifact revisions
  Given signed image A has expected hash H and 4 MiB safely checkpointed
  And the origin now serves image B under a new strong ETag
  When the TCU resumes using Range and If-Range
  Then it must not append B onto A
  And any new full download must still match signed hash H before READY

Scenario: Power loss cannot turn unverified artifact into READY
  Given the image is fully received but promotion has not been committed
  When the OTA process is killed at a chosen crashpoint
  And the OTA service restarts
  Then the service reconciles the file, manifest, journal, and hash
  And the artifact is not handed to P19 unless all checks pass

Scenario: TLS downgrade is forbidden
  Given a valid signed manifest whose source is HTTPS
  When the server redirects to an HTTP endpoint
  Then the request is refused without leaking Authorization
  And the job never transitions to READY
```

### 13.5 Independent oracle pseudocode

```text
function evaluate_test(run):
    if !run.baseline_healthy: return INCONCLUSIVE("broken baseline")
    if !run.inject_receipt.hit_expected_target: return INCONCLUSIVE("fault not injected")
    if !run.evidence_complete: return INCONCLUSIVE("insufficient oracle")
    if run.sanitizer_error || run.deadlock || run.resource_budget_exceeded: return FAIL
    state = read_sqlite_and_filesystem_independently()
    if state.READY:
        assert trusted_manifest_signature_is_valid()
        assert trusted_time_and_target_policy_permits_handoff()
        assert verified_artifact_exists_and_is_immutable()
        assert independent_sha256(artifact) == trusted_manifest.sha256
        assert file_length(artifact) == trusted_manifest.size
    assert no_stage_is_modified_by_stale_job_generation()
    assert durable_offset_not_above_safe_recovery_prefix()
    assert no_untrusted_url_or_secret_is_used()
    return PASS if scenario_expected_behavior_is_proven else FAIL
```

## 14. Jira backlog — 36 tickets / 4 sprint gợi ý

Các estimates cần điều chỉnh theo team và không phải cam kết schedule. Sprint 1: thiết kế/hạ tầng; Sprint 2: download và resume; Sprint 3: durable publish + integration; Sprint 4: hardening và chaos.

| Sprint | Ticket | Task | Implementation scope | Acceptance criteria |
|---|---|---|---|---|
| S1 | `P18-001` | CMake library layout and strict toolchain | Create libtcu_ota_download; C++17, -Wall -Wextra -Werror; dependency injection baseline | Clean build, unit smoke in CI |
| S1 | `P18-002` | Update offer parsing and size bounds | Reject malformed/oversized/ambiguous manifest before IO | All parser negative fixture tests pass |
| S1 | `P18-003` | Signed manifest verification and target binding | Verify canonical signed bytes, trust store, ECU identity and hardware | Invalid key/signature/target blocks GET |
| S1 | `P18-004` | Trusted time and anti-rollback policy | Inject ITrustedTime and persistent metadata revision floor | RTC jump/replay tests fail closed |
| S1 | `P18-005` | Job state machine and transition journal | Define state/event guards, revisions, idempotent operation IDs | Every state transition documented and covered |
| S1 | `P18-006` | SQLite initial schema and repository | WAL/FULL, migrations, durable job snapshots and indexes | DDL runs; integrity/commit tests pass |
| S1 | `P18-007` | Fake clock, fake HTTP and filesystem adapters | Test seam supports deterministic callback order and injected OS errors | Can force timeout/race/crashpoint |
| S1 | `P18-008` | Structured logging and metrics baseline | Correlation IDs, redaction, bounded logs, resource counters | Zero token leakage in log corpus |
| S1 | `P18-009` | Mock HTTPS server and artifact generator | Script responses 200/206/416 and generate signed binary fixtures | Known-range and digest tests pass |
| S2 | `P18-010` | libcurl transport RAII wrapper | HTTPS-only, peer/host validation, timeout, cancellation, bounded headers | TLS and timeout negative tests pass |
| S2 | `P18-011` | HTTP resume and Content-Range validator | Range/If-Range strict parser; ETag strong handling and 200 reset | All 206/200/416 matrix tests pass |
| S2 | `P18-012` | Staging store with safe paths | Create via secure flags, sanitized local names and single-writer locking | Symlink/traversal test cannot escape staging |
| S2 | `P18-013` | Durable checkpoint protocol | fsync file then persist durable_offset and generation; recovery truncation | Kill-at-boundary tests no false offset |
| S2 | `P18-014` | Retry and failure classifier | Map network/HTTP/TLS/storage cases to stable failure classes | Backoff budget tests deterministic |
| S2 | `P18-015` | Download coordinator and bounded queue | Per-target owner, max concurrency and bounded work queue | No parallel writer or OOM stress |
| S2 | `P18-016` | Pause/resume/cancel API lifecycle | Stop IO at safe boundary; requests idempotent and inspectable | Pause+cancel race has single outcome |
| S2 | `P18-017` | Connectivity and power policy adapters | P10/P22 fake gates; network and battery transitions | No GET under deny/stale policy |
| S2 | `P18-018` | Byte-exact streaming with quotas | No decompression, strict size cap, short-write handling | Oversized/truncated binary cases pass |
| S3 | `P18-019` | Streaming SHA-256 and final independent check | Verify complete artifact length/hash and signed policy at finish | One-byte corruption never READY |
| S3 | `P18-020` | Verified artifact immutable store | Content-addressed path, no remote filename trust, pinned refs | Verified bytes unchanged under concurrent jobs |
| S3 | `P18-021` | Promotion crash consistency and reconcile | File fsync, DB phase, rename, directory fsync, DB READY | Crash at every phase recoverable/fail closed |
| S3 | `P18-022` | Restart job scanning and repair states | Detect orphan stage, missing metadata, missing verified file, invalid offsets | Recovery matrix covered with external oracle |
| S3 | `P18-023` | Supersession and job priority arbiter | Authenticated offer comparison and safe preemption per target | Unsigned high-priority offer cannot stop valid job |
| S3 | `P18-024` | P19 handoff interface and reference pinning | Export verified identity only, no install controls, idempotent receiver | P19 mock never sees unverified file |
| S3 | `P18-025` | Security tests including SSRF and credentials | Mirror allowlist, auth header policy, HTTPS-only, log redaction | All credential and redirect tests pass |
| S3 | `P18-026` | Job status outbox & monitoring | Durable monotonic status revision, UNKNOWN/REPAIR needed | No READY/INSTALLED confusion |
| S3 | `P18-027` | SQLite migration and schema recovery | Versioned migration with process-exclusive lock | Interrupted migration boot behavior documented |
| S4 | `P18-028` | 300 What-if automation mapping | Map every WIF case to test tier and executable fixture/stub | IDs unique and traceable; P0 automated |
| S4 | `P18-029` | 50 multi-fault campaigns scheduler | Deterministic seed and event timeline with receipts | Each campaign proves injections occurred |
| S4 | `P18-030` | HTTP adversarial fuzz harness | Fuzz malformed headers/ranges/manifests with ASan/UBSan | No sanitizer regressions in configured corpus |
| S4 | `P18-031` | FS/DB crashpoint matrix | Fault IO VFS and SIGKILL across checkpoint/publish transitions | Reboot reconcile passes defined recovery matrix |
| S4 | `P18-032` | Network namespace netem integration | Use P11 isolated namespace, loss/delay/route changes | No fault leakage to host; cleanup verified |
| S4 | `P18-033` | Long-running soak & resource limits | Run continuous job/failure load, FD/RSS/CPU/disk metrics | No sustained unbounded growth over 24h |
| S4 | `P18-034` | Artifacts and independent oracle | External sha256sum, sqlite integrity, injection evidence, machine report | Inconclusive when oracle/injection absent |
| S4 | `P18-035` | Developer runbook and reproducible CI | One-command fixture, lint, unit, integration, fault suite | Fresh Ubuntu lab reproduces smoke tests |
| S4 | `P18-036` | End-to-end P18/P19 integration review | Hand verified artifact to P19 mock, reject install unless independently checked | End-to-end Demo and release gate signed off |

### 14.1 Definition of Done (mỗi ticket)

- C++17 source, public API, CMake target, unit tests, sanitizer-capable build và build warnings clean.
- Feature có **behavior contract + negative test**; không chỉ có happy-path demo.
- Error code / log / metrics không chứa token, signed URL bí mật hoặc key material.
- Inter-thread ownership rõ ràng; retry/cancel callback từ generation cũ phải bị loại.
- Có test chứng minh invariant liên quan; `PASS` chỉ khi fault injected và oracle độc lập xác nhận.
- Chạy CI unit/integration và lưu evidence của failure mode.

## 15. Proposed repository layout & build profiles

```text
virtual-tcu-lab/
  project18-ota-download/
    CMakeLists.txt
    README.md
    include/tcu/ota/{types.hpp,service.hpp,http.hpp,manifest.hpp,staging.hpp,clock.hpp}
    src/{ota_service.cpp,coordinator.cpp,state_machine.cpp,manifest_verifier.cpp,
         curl_transport.cpp,range_validator.cpp,sqlite_job_repo.cpp,
         staging_store.cpp,artifact_publisher.cpp,recovery.cpp,policy.cpp}
    apps/tcu-otad/main.cpp
    configs/{lab.json,secure_profile.json}
    fixtures/{root-ca/,signed_manifests/,artifacts/,http_scenarios/}
    servers/https_fixture_server.py
    tests/unit/{state,range,manifest,backoff,policy,recovery}_test.cpp
    tests/integration/{http_resume,sqlite,crashpoints,p19_handoff}_test.cpp
    tests/fuzz/{manifest_fuzzer.cpp,http_headers_fuzzer.cpp}
    tests/chaos/scenarios/*.yaml
    tools/{make_fixture.py,inject_fault.py,verify_evidence.py}
    evidence/<run_id>/
    docs/18_OTA_Download_Manager_System_Design.md
```

**CMake build profiles:** `Debug` with ASan+UBSan; separate TSan build (không kết hợp TSan và ASan); `RelWithDebInfo` perf/soak. Unit harness uses fake clock and HTTP, integration uses real libcurl and a locally trusted HTTPS server. Never disable TLS verification to make demo easier. Prefer a test CA and explicit CA bundle.

**Suggested dependencies:** C++17, libcurl, OpenSSL/libcrypto, SQLite3, Google Test/Mock, nlohmann/json only for bounded schema parsing; Linux `iproute2`, `tc netem`, `strace`, `perf`, and P11 network lab for realistic faults.

## 16. Ubuntu lab runbook (safe & isolated)

### 16.1 Install prerequisites

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev sqlite3 \
  libgtest-dev libgmock-dev python3 python3-venv openssl \
  iproute2 curl jq gdb valgrind
```

Use separate test namespaces from P11 rather than changing global host firewall/routing. P18 operates against a **local HTTPS fixture**; do not point it at production cloud or real vehicle update endpoints.

### 16.2 Locally trusted TLS identity for the fixture

```bash
mkdir -p fixtures/root-ca
openssl req -x509 -newkey rsa:3072 -nodes -days 3 \
  -keyout fixtures/root-ca/ca.key \
  -out fixtures/root-ca/ca.crt \
  -subj '/CN=VirtualTCU-Lab-CA'
# Sign a server certificate containing SAN DNS:ota.lab.local or IP:127.0.0.1.
# Configure libcurl with CURLOPT_CAINFO pointing at ca.crt and verify both
# peer chain and hostname. Do not ship the test CA/private key in production.
```

**Note:** The command above makes a local CA but *does not create or start an HTTPS server*. Implement `servers/https_fixture_server.py` to script `200`, `206`, `416`, wrong ETag/Content-Range, redirects, delays, chunk corruption and termination. Add a fixture server certificate signed by the local CA with SAN matching the URL used by the tests. A plain `python3 -m http.server` is **not** a substitute for an HTTPS/Range fault fixture.

### 16.3 Smoke steps after implementation

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
# Start your local scripted HTTPS fixture in another terminal.
# Example CLI contract to implement (not available until source is written):
./build/tcu-otad --config configs/lab.json
./build/ota-cli offer --manifest fixtures/signed_manifests/demo.json
./build/ota-cli status --job demo-job-01
sha256sum ./lab-data/verified/*.img
sqlite3 ./lab-data/db/jobs.sqlite3 'PRAGMA integrity_check;'
```

### 16.4 Fault injection recipes

| Fault | Injection method | Safety / evidence |
|---|---|---|
| Packet loss/delay | P11 `tc netem` applied to isolated `veth` egress | Compare qdisc and packet counters before/after |
| Link cut / route swap | `ip netns exec ... ip link set dev ... down` | Verify target interface and direction |
| HTTP 200/206/416 | Stateful fixture scripted per request number | Save sanitized headers and byte ranges |
| Invalid TLS chain | Server cert signed by untrusted CA | Observe libcurl verify error and no fallback |
| Process crash | `SIGKILL` at named coordinator checkpoint | Verify killed PID, restart and reconcile |
| SQLite `IOERR/BUSY` | Fault VFS/mock repository, or transaction held open | Capture SQLite error code and DB snapshot |
| Disk full | Separate tmpfs with quota, or injectable FS adapter | Never fill host root filesystem |
| `fsync`/`rename` errors | Inject file-ops adapter / fault filesystem | Record exact syscall faultpoint |
| Clock jump | Fake clock and trusted-time adapter | Never modify host time |
| Metadata tamper | Signed fixture with bit flip or wrong key | Independent signature oracle |

### 16.5 YAML example of one deterministic scenario

```yaml
id: P18-WIF-08-02
seed: 180802
baseline:
  job: signed_artifact_v1
  initial_durable_offset: 4194304
steps:
  - at_ms: 0
    action: start_resume
    expect_request:
      range: "bytes=4194304-"
      if_range: '"strong-v1"'
  - at_ms: 10
    action: http_respond
    status: 200
    body_fixture: signed_artifact_v1_full
  - at_ms: 20
    action: await_job_state
    value: VERIFYING
assertions:
  - old_prefix_never_appended_to_full_200
  - final_sha256_equals_trusted_manifest
  - durable_offset_never_exceeds_safe_prefix
  - no_unverified_handoff
```

**Thực thi YAML:** chỉ là test specification mẫu. `ota-fault-runner` cần được implement để thực sự đọc script, quan sát request/response và đối chiếu independent oracle. Không được coi YAML tự chạy là bằng chứng đã test.

## 17. Acceptance gates, triage & release checklist

### 17.1 Quality gates (gợi ý cho lab)

| Gate | Yêu cầu để merge / release lab |
|---|---|
| Integrity/security | 100% các case P0 đã chọn tự động hóa; **zero false READY**; no invalid TLS fallback; no secrets in logs |
| Reproducibility | Scenario ID + seed + fixture SHA + build commit + fault receipts are saved |
| Crash consistency | All named fsync/DB/rename crashpoints have documented reconciliation outcome |
| HTTP range | 200/206/416 table, strong ETag, changed ETag, malformed Content-Range covered |
| Cancellation | No double terminal state across bounded concurrency stress; ASan/TSan no known relevant findings |
| Resources | Memory, disk, FD, active downloads, timers, retries have defined and measured ceilings |
| Handoff | P19 mock never obtains unverified/staged artifact; no install/flash from P18 |
| Reporting | Explicit `PASS`, `FAIL`, `INCONCLUSIVE`, `SKIPPED`; unresolved P0 blocks release |

### 17.2 Release invariants review

- [ ] Signed metadata verified and bound to device/target/software version and trusted time policy.
- [ ] Transfer bytes match authenticated size/hash, including after resume/mirror/restart.
- [ ] Recovery can only trust durable offsets proven by successful file sync + journal protocol.
- [ ] No vulnerable fallback across redirects, proxies, TLS, authorization or signed metadata.
- [ ] Reader receives only immutable verified artifacts, never staging objects.
- [ ] Job and callback generation fencing verified under cancel/stop/reconnect.
- [ ] P18 does not declare OTA installation success, only artifact readiness/handoff.
- [ ] Test runner validates fault injection before judging SUT correctness.

### 17.3 Known limitations / what simulation cannot prove

- Actual NAND/eMMC power-failure durability, FTL caching, kernel driver behavior, hardware wear-out and physical bootloader sequence require dedicated target/HIL testing.
- Root-of-trust provisioning, secure time attestation, HSM signing, real vehicle authorization and safety requirements must come from the OEM; a development manifest format does **not** establish Uptane compliance.
- Linux `SIGKILL` simulation models process termination, **not** sudden physical loss of power; use explicit fault-storage models for the latter.
- HTTP ETag/Range validation improves representation consistency but is never a substitute for independently verifying the signed manifest's cryptographic digest.
- Even 300 separate cases and 50 campaigns cannot represent every possible interleaving or security vulnerability; maintain issue-derived regression additions.

## 18. Source references & terminology

### 18.1 References (authoritative)

1. **RFC 9110 — HTTP Semantics, sections 13, 14, 15**: Range, If-Range, Content-Range, `206`, `416`. https://www.rfc-editor.org/rfc/rfc9110.html
2. **libcurl official docs**: `CURLOPT_RESUME_FROM_LARGE`, `CURLOPT_RANGE`, `CURLOPT_SSL_VERIFYPEER`, `CURLOPT_SSL_VERIFYHOST`, `CURLOPT_FOLLOWLOCATION`, timeouts and write callbacks. https://curl.se/libcurl/c/
3. **SQLite WAL and PRAGMA synchronous**: durability, checkpoint behavior and limits. https://sqlite.org/wal.html and https://sqlite.org/pragma.html#pragma_synchronous
4. **RAUC using / Security**: signed bundles and update-system integration reference, **not** a guarantee this simulator is RAUC compatible. https://rauc.readthedocs.io/en/latest/using.html
5. **Uptane Standard**: Director/Image repository trust, signed metadata, target checks, secure time; lab uses a smaller illustrative signed-manifest design. https://uptane.github.io/standard/uptane-standard
6. **Linux man-pages, fsync(2), rename(2)**: file and directory durability semantics. https://man7.org/linux/man-pages/man2/fsync.2.html and https://man7.org/linux/man-pages/man2/rename.2.html
7. **P11 Network Fault Injection Lab** and **P19 OTA Verification & A/B Slot Simulator**: sister designs within Virtual TCU Lab.

### 18.2 Terminology

- **OTA Download Manager (ODM)**: component that acquires and verifies an update artifact; it does **not** flash/install the ECU.
- **Manifest**: signed statement binding artifact bytes (size and hash) to ECU and release requirements.
- **ETag**: HTTP representation validator; a strong ETag can be used with `If-Range`, but is not a cryptographic signature.
- **Staging**: untrusted, mutable local file receiving bytes from remote server.
- **Verified artifact**: immutable bytes whose size/hash/trust/policy have been independently checked.
- **Durable offset**: checkpoint proven to be recoverable under the selected durability model.
- **Crashpoint**: chosen point between side effects where the runner forcibly terminates or simulates power loss.
- **P19**: a **separate** component responsible for installation, A/B slot selection, boot health and rollback.

---

**Version 1.1 — Project 18 design & resilience catalog.** Documentation only: no C++ implementation, successful stress-test certification, or vehicle deployment is claimed.

