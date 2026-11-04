// T010: structured log fields. contracts/metrics.md, "Structured log fields".

#include "log.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "support/raw_listener.hpp"
#include "support/raw_peer.hpp"
#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

using tcu::ipc::detail::LogFields;
using tcu::ipc::detail::LogLevel;

std::vector<std::string> splitKeys(const std::string& line) {
  std::vector<std::string> keys;
  std::size_t pos = 0;
  while (pos < line.size()) {
    const std::size_t eq = line.find('=', pos);
    if (eq == std::string::npos) {
      break;
    }
    keys.push_back(line.substr(pos, eq - pos));
    const std::size_t sp = line.find(' ', eq);
    if (sp == std::string::npos) {
      break;
    }
    pos = sp + 1;
  }
  return keys;
}

TEST(Log, FieldsAndNoPayload) {
  LogFields f;
  f.level = LogLevel::Info;
  f.service = "vehicle_service";
  f.connection_id = 7;
  f.request_id = 42;
  f.method = "vehicle.getStatus";
  f.event = "request_completed";
  f.duration_ms = 3;

  const std::string line = tcu::ipc::detail::format(f);

  const std::vector<std::string> expected = {"timestamp",     "level",      "service",
                                             "connection_id", "request_id", "method",
                                             "event",         "reason",     "duration_ms"};
  EXPECT_EQ(splitKeys(line), expected);

  // One physical line (10 is the newline byte).
  EXPECT_EQ(line.find(static_cast<char>(10)), std::string::npos);
  // No payload key exists at all, so a payload cannot leak through it.
  EXPECT_EQ(line.find("payload"), std::string::npos);
}

TEST(Log, AbsentValuesRenderAsDash) {
  LogFields f;
  f.level = LogLevel::Warn;
  f.service = "tcu_client";
  f.event = "reconnecting";

  const std::string line = tcu::ipc::detail::format(f);

  EXPECT_NE(line.find("connection_id=-"), std::string::npos);
  EXPECT_NE(line.find("request_id=-"), std::string::npos);
  EXPECT_NE(line.find("method=-"), std::string::npos);
  EXPECT_NE(line.find("reason=-"), std::string::npos);
  EXPECT_NE(line.find("duration_ms=-"), std::string::npos);
}

TEST(Log, TimestampIsIso8601Utc) {
  LogFields f;
  f.level = LogLevel::Info;
  f.service = "s";
  f.event = "listening";

  const std::string line = tcu::ipc::detail::format(f);
  const std::string ts = line.substr(std::string("timestamp=").size(), 24);

  // 2026-11-04T09:12:33.481Z
  ASSERT_EQ(ts.size(), 24u);
  EXPECT_EQ(ts[4], '-');
  EXPECT_EQ(ts[7], '-');
  EXPECT_EQ(ts[10], 'T');
  EXPECT_EQ(ts[13], ':');
  EXPECT_EQ(ts[16], ':');
  EXPECT_EQ(ts[19], '.');
  EXPECT_EQ(ts[23], 'Z');
}

TEST(Log, LevelNames) {
  LogFields f;
  f.service = "s";
  f.event = "stopped";
  f.level = LogLevel::Debug;
  EXPECT_NE(tcu::ipc::detail::format(f).find("level=debug"), std::string::npos);
  f.level = LogLevel::Error;
  EXPECT_NE(tcu::ipc::detail::format(f).find("level=error"), std::string::npos);
}

/// Captures everything written to stderr while `body` runs.
template <typename Body>
std::string captureStderr(Body body) {
  char template_path[] = "/tmp/tcu-ipc-log-XXXXXX";
  const int tmp_fd = ::mkstemp(template_path);
  if (tmp_fd < 0) {
    return {};
  }
  std::fflush(stderr);
  const int saved = ::dup(STDERR_FILENO);
  ::dup2(tmp_fd, STDERR_FILENO);

  body();

  std::fflush(stderr);
  ::dup2(saved, STDERR_FILENO);
  ::close(saved);

  ::lseek(tmp_fd, 0, SEEK_SET);
  std::string out;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(tmp_fd, buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(tmp_fd);
  ::unlink(template_path);
  return out;
}

// T085: peer identity is an audit record, not general log furniture
// (contracts/metrics.md, "What must never appear in a log line").
TEST(Log, RejectedPeerIsTheOnlyPlaceWithPeerIds) {
  const std::string captured = captureStderr([] {
    tcu::ipc::test::TempSocketDir dir;
    tcu::ipc::ServerOptions options;
    options.socket_path = dir.socketPath();
    // One peer is rejected, and one is accepted and handshakes, so the capture
    // holds both kinds of line.
    options.allowed_uids = {::geteuid(), ::geteuid() + 1000};
    tcu::ipc::IpcServer server(options);
    ASSERT_TRUE(server.start());

    tcu::ipc::test::RawPeer accepted;
    ASSERT_TRUE(accepted.connect(dir.socketPath()));
    ASSERT_TRUE(accepted.writeBytes(tcu::ipc::test::rawFrame(
        1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}")));
    accepted.readAtLeast(4, std::chrono::milliseconds(1000));

    tcu::ipc::ServerOptions strict = options;
    strict.allowed_uids = {::geteuid() + 1000};
    strict.socket_path = dir.socketPath("strict.sock");
    tcu::ipc::IpcServer strict_server(strict);
    ASSERT_TRUE(strict_server.start());
    tcu::ipc::test::RawPeer rejected;
    ASSERT_TRUE(rejected.connect(strict.socket_path));
    rejected.readAvailable(std::chrono::milliseconds(500));

    server.stop();
    strict_server.stop();
  });

  ASSERT_FALSE(captured.empty()) << "nothing was logged, so nothing was checked";

  std::istringstream lines(captured);
  std::string line;
  int rejected_lines = 0;
  int total_lines = 0;
  while (std::getline(lines, line)) {
    if (line.find("timestamp=") != 0) {
      continue;  // not one of our lines
    }
    ++total_lines;
    EXPECT_EQ(line.find("payload"), std::string::npos) << line;
    const bool has_peer_ids = line.find("uid=") != std::string::npos ||
                              line.find("pid=") != std::string::npos;
    if (has_peer_ids) {
      ++rejected_lines;
      EXPECT_NE(line.find("event=rejected_peer"), std::string::npos)
          << "peer identity appeared outside the audit line: " << line;
    }
  }
  EXPECT_GT(total_lines, 1);
  EXPECT_EQ(rejected_lines, 1) << "expected exactly one rejected_peer audit line";
}

}  // namespace
