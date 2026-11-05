// The rotating file sink: BC-20, BC-21, BC-27 and invariants F1 to F6.
// Every failure here is injected through FileOps, so no test fills a disk or
// changes a real permission (Constitution Principle IV).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "support/faulty_file_ops.hpp"
#include "tcu/logging/file_sink.hpp"

namespace tcu::logging {
namespace {

using namespace std::chrono_literals;

class FileSinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("tcu_sink_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  FileSinkOptions options(std::size_t max_bytes = 10 * 1024 * 1024,
                          std::size_t file_count = 5) {
    FileSinkOptions opts;
    opts.path = dir_ / "tcu.log";
    opts.fileMaxBytes = max_bytes;
    opts.fileCount = file_count;
    opts.fallbackToStderr = false;  // keep the test output readable
    return opts;
  }

  std::size_t countLogFiles() const {
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
      if (entry.path().filename().string().rfind("tcu.log", 0) == 0) {
        ++n;
      }
    }
    return n;
  }

  std::filesystem::path dir_;
};

// BC-20, F2: the decision to rotate happens before the write, so the active
// file never exceeds the configured maximum.
TEST_F(FileSinkTest, WritesAndRotates) {
  const std::string line(60, 'a');
  FileSink sink(options(200, 5));
  ASSERT_TRUE(sink.open());

  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(sink.write(line)) << "write " << i;
    EXPECT_LE(sink.currentSize(), 200u);
  }
  sink.flush();

  EXPECT_GE(sink.rotations(), 2u);
  EXPECT_TRUE(std::filesystem::exists(dir_ / "tcu.log"));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "tcu.log.1"));
  EXPECT_LE(std::filesystem::file_size(dir_ / "tcu.log"), 200u);
}

// BC-20, F3: at most fileCount rotated files plus the active one.
TEST_F(FileSinkTest, RetainsAtMostFileCount) {
  const std::string line(60, 'b');
  FileSink sink(options(100, 2));
  ASSERT_TRUE(sink.open());

  for (int i = 0; i < 20; ++i) {
    sink.write(line);
  }
  sink.flush();

  EXPECT_LE(countLogFiles(), 3u) << "active plus two rotated is the whole budget";
  EXPECT_FALSE(std::filesystem::exists(dir_ / "tcu.log.3"));
}

// F4. On Windows std::filesystem::permissions only toggles the read-only
// attribute, so the bit check is Linux-only; the call still has to succeed
// everywhere, which the open() assertions cover.
TEST_F(FileSinkTest, PermissionsAreOwnerOnly) {
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX permission bits are not modelled on Windows";
#else
  FileSink sink(options(100, 2));
  ASSERT_TRUE(sink.open());
  sink.write(std::string(60, 'c'));
  sink.write(std::string(60, 'c'));
  sink.flush();

  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    const auto perms = std::filesystem::status(entry.path()).permissions();
    EXPECT_EQ(perms & std::filesystem::perms::group_all,
              std::filesystem::perms::none)
        << entry.path();
    EXPECT_EQ(perms & std::filesystem::perms::others_all,
              std::filesystem::perms::none)
        << entry.path();
  }
#endif
}

// BC-21, F1: a full disk makes the sink unhealthy and costs the record, and
// that is all it costs. The caller is never told through an exception.
TEST_F(FileSinkTest, DiskFullMarksUnhealthyAppSurvives) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSink sink(options(), std::move(ops));
  ASSERT_TRUE(sink.open());

  EXPECT_TRUE(sink.write("first line is fine"));
  raw->failWriteFrom(raw->writeCalls() + 1, ENOSPC);

  EXPECT_FALSE(sink.write("this one meets a full disk"));
  EXPECT_EQ(sink.health(), SinkHealth::Unhealthy);
  EXPECT_EQ(sink.lastError(), ENOSPC);

  // And it does not keep trying on every subsequent record.
  const std::size_t writes_before = raw->writeCalls();
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(sink.write("still full"));
  }
  EXPECT_EQ(raw->writeCalls(), writes_before) << "an unhealthy sink must not spin";
}

// BC-27, F5: an unusable path leaves the sink unhealthy, and the logger runs on.
TEST_F(FileSinkTest, UnwritablePathFallsBackToStderr) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  raw->failNextOpens(1, EACCES);

  FileSinkOptions opts = options();
  opts.fallbackToStderr = true;
  FileSink sink(std::move(opts), std::move(ops));

  EXPECT_FALSE(sink.open());
  EXPECT_EQ(sink.health(), SinkHealth::Unhealthy);
  EXPECT_EQ(sink.lastError(), EACCES);
  EXPECT_FALSE(sink.write("goes to stderr instead"));
}

// A failed write must not leave half a record behind, so a short write is a
// failure for that record rather than something to patch up.
TEST_F(FileSinkTest, ShortWriteIsAFailureNotAPartialLine) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSink sink(options(), std::move(ops));
  ASSERT_TRUE(sink.open());

  raw->failWriteFrom(1, EIO);
  EXPECT_FALSE(sink.write("partial"));
  EXPECT_EQ(sink.health(), SinkHealth::Unhealthy);
}

// EINTR is a signal, not a disk fault: retried, boundedly.
TEST_F(FileSinkTest, EintrIsRetriedThenSucceeds) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSink sink(options(), std::move(ops));
  ASSERT_TRUE(sink.open());

  raw->interruptNextWrites(2);
  EXPECT_TRUE(sink.write("survives two interruptions"));
  EXPECT_EQ(sink.health(), SinkHealth::Healthy);
}

TEST_F(FileSinkTest, EintrBeyondTheRetryBudgetFails) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSink sink(options(), std::move(ops));
  ASSERT_TRUE(sink.open());

  raw->interruptNextWrites(50);
  EXPECT_FALSE(sink.write("never gets through"));
  EXPECT_EQ(sink.health(), SinkHealth::Unhealthy);
}

// F6: at most one probe per interval, and no spin in between.
TEST_F(FileSinkTest, RecoveryProbeReopensWhenAllowed) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSinkOptions opts = options();
  opts.recoveryProbeInterval = 0s;  // probe allowed immediately
  FileSink sink(std::move(opts), std::move(ops));
  ASSERT_TRUE(sink.open());

  raw->failWriteFrom(raw->writeCalls() + 1, ENOSPC);
  EXPECT_FALSE(sink.write("breaks the sink"));
  ASSERT_EQ(sink.health(), SinkHealth::Unhealthy);

  // The fault was armed on writes, not on opens, so the probe reopens and the
  // sink recovers - which is the behaviour scenario F describes.
  const std::size_t opens_before = raw->openCalls();
  sink.periodic();
  EXPECT_EQ(raw->openCalls(), opens_before + 1);
  EXPECT_EQ(sink.health(), SinkHealth::Healthy);
}

TEST_F(FileSinkTest, RecoveryProbeIsRateLimited) {
  auto ops = std::make_unique<test::FaultyFileOps>();
  test::FaultyFileOps* raw = ops.get();
  FileSinkOptions opts = options();
  opts.recoveryProbeInterval = 3600s;  // far away on purpose
  FileSink sink(std::move(opts), std::move(ops));
  ASSERT_TRUE(sink.open());

  raw->failWriteFrom(raw->writeCalls() + 1, ENOSPC);
  EXPECT_FALSE(sink.write("breaks the sink"));

  const std::size_t opens_before = raw->openCalls();
  for (int i = 0; i < 100; ++i) {
    sink.periodic();
  }
  EXPECT_EQ(raw->openCalls(), opens_before)
      << "100 wakeups inside one interval must cost zero probes";
  EXPECT_GT(sink.nextPeriodicIn().count(), 0);
}

// R-13: the active path is not followed when it is a symlink.
TEST_F(FileSinkTest, RefusesSymlinkPath) {
  const std::filesystem::path target = dir_ / "real.log";
  { std::ofstream out(target); out << "x"; }
  const std::filesystem::path link = dir_ / "tcu.log";
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec) {
    GTEST_SKIP() << "symlink creation not permitted here: " << ec.message();
  }

  FileSink sink(options());
  EXPECT_FALSE(sink.open());
  EXPECT_EQ(sink.health(), SinkHealth::Unhealthy);
}

}  // namespace
}  // namespace tcu::logging
