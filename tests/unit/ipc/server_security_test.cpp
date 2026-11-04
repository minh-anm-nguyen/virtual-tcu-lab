// T079-T081: the local trust boundary of spec section 8.8, made executable by
// research R-10. B-14, FR-09, invariant P3.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "support/raw_listener.hpp"
#include "support/raw_peer.hpp"
#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::ipc::test::RawPeer;
using tcu::ipc::test::TempSocketDir;

// T079 / B-14 / FR-09
TEST(ServerSecurity, RejectsUnauthorizedUid) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  // A UID this process certainly does not have, so the allowlist must bite
  // whether the suite runs as root or not.
  options.allowed_uids = {::geteuid() + 1000};
  IpcServer server(options);

  bool handler_ran = false;
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [&handler_ran](std::string) {
    handler_ran = true;
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  // connect() succeeds - the rejection happens after accept(), before any
  // frame is read.
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  peer.readAvailable(1000ms);
  EXPECT_TRUE(peer.isClosedByPeer()) << "an unauthorized peer must be closed";
  EXPECT_FALSE(handler_ran) << "no handler may run for a rejected peer";
  EXPECT_EQ(server.sessionCount(), 0u);

  server.stop();
}

TEST(ServerSecurity, OwnUidIsAllowedByDefault) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  // An empty allowlist means owner only, not "anyone" (invariant P3).
  ASSERT_TRUE(options.allowed_uids.empty());
  IpcServer server(options);
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  // The session is accepted, so a handshake is possible.
  const std::string hello =
      tcu::ipc::test::rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}");
  ASSERT_TRUE(peer.writeBytes(hello));
  EXPECT_FALSE(peer.readAtLeast(4, 2000ms).empty());

  server.stop();
}

// T080 / research R-10
TEST(ServerSecurity, RejectsDirectoryWithLoosePermissions) {
  TempSocketDir dir;
  const std::string loose_dir = dir.path() + "/loose";
  ASSERT_EQ(::mkdir(loose_dir.c_str(), 0755), 0);
  ASSERT_EQ(::chmod(loose_dir.c_str(), 0755), 0);

  ServerOptions options;
  options.socket_path = loose_dir + "/vehicle.sock";
  IpcServer server(options);

  // A world- or group-accessible runtime directory makes the UID allowlist
  // meaningless, so start() refuses rather than "fixing" the directory.
  EXPECT_FALSE(server.start());

  struct stat st {};
  ASSERT_EQ(::stat(loose_dir.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777u, 0755u) << "start() must not have changed the directory";
  ::rmdir(loose_dir.c_str());
}

TEST(ServerSecurity, AcceptsOwnedPrivateDirectory) {
  TempSocketDir dir;  // created 0700 and owned by this user
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  EXPECT_TRUE(server.start());
  server.stop();
}

// T081 / research R-10
TEST(ServerSecurity, RefusesAPathALiveServerOwns) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();

  IpcServer first(options);
  ASSERT_TRUE(first.start());

  // A probe connect() succeeds, which proves a live server owns the path, so
  // the second server must refuse instead of unlinking a working socket.
  IpcServer second(options);
  EXPECT_FALSE(second.start());

  // The first server is unharmed.
  RawPeer peer;
  EXPECT_TRUE(peer.connect(dir.socketPath()));
  first.stop();
}

TEST(ServerSecurity, UnlinksAStaleSocketAndBinds) {
  TempSocketDir dir;
  const std::string path = dir.socketPath();

  // Leave a socket file behind with nobody listening: bind then close without
  // unlinking. A connect() to it now fails ECONNREFUSED.
  {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ::close(fd);
  }
  struct stat st {};
  ASSERT_EQ(::stat(path.c_str(), &st), 0) << "the stale socket file should exist";

  ServerOptions options;
  options.socket_path = path;
  IpcServer server(options);
  EXPECT_TRUE(server.start()) << "a stale socket must be unlinked and rebound";

  RawPeer peer;
  EXPECT_TRUE(peer.connect(path));
  server.stop();
}

}  // namespace
