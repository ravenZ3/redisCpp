// select() variant of the server, built for comparison against epoll.
// Command logic is shared; only the multiplexer differs.
//
//   redis-select [port]   (default 6379)
//
// select() has two well-known scaling problems this variant deliberately
// keeps rather than works around, since they're the point of the benchmark:
//   1. FD_SETSIZE (typically 1024): fds at or above it can't be watched at
//      all. Those connections are rejected instead of corrupting the
//      fd_set bitmap.
//   2. The read set has to be rebuilt and rescanned linearly every loop
//      iteration, so cost grows with the highest fd in use regardless of
//      how many connections are actually busy.

#include "server.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sys/select.h>
#include <vector>

namespace
{
class SelectPoller : public Poller
{
public:
  bool add(int fd) override { return fd < FD_SETSIZE; }
  void remove(int) override {} // the fd_set is rebuilt every iteration
};
} // namespace

int main(int argc, char **argv)
{
  int port = argc > 1 ? std::atoi(argv[1]) : 6379;

  ServerContext ctx;
  SelectPoller poller;
  int server_fd = make_listen_socket(port);
  std::cout << "[select] listening on port " << port << "...\n";

  while (true)
  {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(server_fd, &read_fds);
    int max_fd = server_fd;
    for (auto &[fd, client] : ctx.clients)
    {
      FD_SET(fd, &read_fds);
      max_fd = std::max(max_fd, fd);
    }

    int timeout_ms = compute_timeout(ctx);
    timeval tv{};
    timeval *tvp = nullptr;
    if (timeout_ms >= 0)
    {
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      tvp = &tv;
    }

    int n = select(max_fd + 1, &read_fds, nullptr, nullptr, tvp);

    if (n > 0)
    {
      if (FD_ISSET(server_fd, &read_fds))
        accept_connections(ctx, server_fd, poller);

      // Snapshot first: handling one fd (BLPOP wake-ups, disconnects) can
      // mutate ctx.clients while we're mid-iteration.
      std::vector<int> ready_fds;
      for (auto &[fd, client] : ctx.clients)
        if (FD_ISSET(fd, &read_fds))
          ready_fds.push_back(fd);

      for (int fd : ready_fds)
        if (ctx.clients.count(fd))
          handle_client_event(ctx, fd, poller);
    }

    sweep_timeouts(ctx);
  }
}
