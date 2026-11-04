// T014: RAII ownership of one FD. spec section 7 "Ownership", section 8.3.

#include "unix_socket.hpp"

#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>

#include "support/temp_socket_dir.hpp"

namespace {

using tcu::ipc::detail::UnixSocket;
using tcu::ipc::test::TempSocketDir;

std::size_t openFdCount() {
  std::size_t n = 0;
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) {
    return 0;
  }
  while (::readdir(d) != nullptr) {
    ++n;
  }
  ::closedir(d);
  return n;
}

TEST(UnixSocket, DefaultConstructedOwnsNoFd) {
  UnixSocket s;
  EXPECT_FALSE(s.valid());
  EXPECT_EQ(s.fd(), -1);
}

TEST(UnixSocket, BindListenAcceptLifecycleClosesEveryFd) {
  const std::size_t before = openFdCount();
  {
    TempSocketDir dir;
    UnixSocket server;
    ASSERT_TRUE(server.open());
    ASSERT_EQ(server.bindPath(dir.socketPath()), 0) << "errno=" << server.lastErrno();
    ASSERT_EQ(server.listen(8), 0);

    UnixSocket client;
    ASSERT_TRUE(client.open());
    ASSERT_EQ(client.connectPath(dir.socketPath()), 0);

    UnixSocket accepted = server.accept();
    EXPECT_TRUE(accepted.valid());
  }
  EXPECT_EQ(openFdCount(), before);
}

TEST(UnixSocket, BindOnDrvFsPathFailsAndReportsErrno) {
  // research R-00 note 2: AF_UNIX bind on /mnt fails EOPNOTSUPP under WSL2.
  UnixSocket s;
  ASSERT_TRUE(s.open());
  const int rc = s.bindPath("/mnt/d/TCU/build-wsl/should-not-bind.sock");
  EXPECT_NE(rc, 0);
  EXPECT_NE(s.lastErrno(), 0);
}

TEST(UnixSocket, MoveTransfersOwnershipSoTheFdClosesOnce) {
  const std::size_t before = openFdCount();
  {
    UnixSocket a;
    ASSERT_TRUE(a.open());
    const int raw = a.fd();
    UnixSocket b(std::move(a));
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.fd(), raw);
  }
  EXPECT_EQ(openFdCount(), before);
}

TEST(UnixSocket, CloseIsIdempotent) {
  const std::size_t before = openFdCount();
  UnixSocket s;
  ASSERT_TRUE(s.open());
  s.close();
  s.close();
  EXPECT_FALSE(s.valid());
  EXPECT_EQ(openFdCount(), before);
}

TEST(UnixSocket, NonBlockingReadReturnsEagainRatherThanBlocking) {
  TempSocketDir dir;
  UnixSocket server;
  ASSERT_TRUE(server.open());
  ASSERT_EQ(server.bindPath(dir.socketPath()), 0);
  ASSERT_EQ(server.listen(8), 0);

  UnixSocket client;
  ASSERT_TRUE(client.open());
  ASSERT_EQ(client.connectPath(dir.socketPath()), 0);
  UnixSocket accepted = server.accept();
  ASSERT_TRUE(accepted.valid());
  ASSERT_TRUE(accepted.setNonBlocking());

  char buf[16];
  const ssize_t n = accepted.recvSome(buf, sizeof(buf));
  EXPECT_LT(n, 0);
  EXPECT_TRUE(accepted.lastErrno() == EAGAIN || accepted.lastErrno() == EWOULDBLOCK);
}

TEST(UnixSocket, PeerCredentialsReportThisProcess) {
  TempSocketDir dir;
  UnixSocket server;
  ASSERT_TRUE(server.open());
  ASSERT_EQ(server.bindPath(dir.socketPath()), 0);
  ASSERT_EQ(server.listen(8), 0);

  UnixSocket client;
  ASSERT_TRUE(client.open());
  ASSERT_EQ(client.connectPath(dir.socketPath()), 0);
  UnixSocket accepted = server.accept();
  ASSERT_TRUE(accepted.valid());

  tcu::ipc::detail::PeerCredentials cred;
  ASSERT_TRUE(accepted.peerCredentials(cred));
  EXPECT_EQ(cred.uid, ::geteuid());
  EXPECT_EQ(cred.pid, ::getpid());
}

TEST(UnixSocket, SendToClosedPeerFailsWithoutKillingTheProcess) {
  // MSG_NOSIGNAL: without it this test would die of SIGPIPE (spec section 8.3).
  TempSocketDir dir;
  UnixSocket server;
  ASSERT_TRUE(server.open());
  ASSERT_EQ(server.bindPath(dir.socketPath()), 0);
  ASSERT_EQ(server.listen(8), 0);

  UnixSocket accepted;
  {
    UnixSocket client;
    ASSERT_TRUE(client.open());
    ASSERT_EQ(client.connectPath(dir.socketPath()), 0);
    accepted = server.accept();
    ASSERT_TRUE(accepted.valid());
  }  // the client closes here

  const char payload[] = "x";
  ssize_t n = 0;
  for (int i = 0; i < 1000; ++i) {
    n = accepted.sendSome(payload, sizeof(payload));
    if (n < 0) {
      break;
    }
  }
  EXPECT_LT(n, 0);
  EXPECT_TRUE(accepted.lastErrno() == EPIPE || accepted.lastErrno() == ECONNRESET);
}

}  // namespace
