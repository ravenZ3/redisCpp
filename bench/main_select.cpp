// select() variant of the benchmark server, for comparison against
// main_epoll.cpp. Command logic is byte-identical (shared via
// redis_common.hpp) — only the multiplexer differs.
//
// select() has two well-known scaling problems this variant deliberately
// keeps rather than works around, since they're the point of the benchmark:
//   1. FD_SETSIZE (typically 1024): fds at or above it can't be watched at
//      all. We reject those connections outright instead of corrupting the
//      fd_set bitmap.
//   2. The read set has to be rebuilt from scratch and rescanned linearly
//      every single loop iteration, so cost grows with the highest fd
//      number in use regardless of how many connections are actually busy.
#include "redis_common.hpp"
#include <sys/select.h>
#include <cstdlib>

int main(int argc, char **argv)
{
  int port = argc > 1 ? std::atoi(argv[1]) : 6379;

  ServerContext ctx;
  register_commands();

  int server_fd = make_listen_socket(port);
  std::cout << "[select] listening on port " << port << "...\n";

  auto register_fd = [&](int fd) -> bool
  {
    return fd < FD_SETSIZE;
  };
  auto unregister_fd = [&](int) {};

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
        accept_connections(ctx, server_fd, register_fd);

      // Snapshot first: handling one fd (BLPOP wake-ups, disconnects) can
      // mutate ctx.clients while we're mid-iteration.
      std::vector<int> ready_fds;
      for (auto &[fd, client] : ctx.clients)
        if (FD_ISSET(fd, &read_fds))
          ready_fds.push_back(fd);

      for (int fd : ready_fds)
      {
        if (ctx.clients.count(fd))
          handle_client_event(ctx, fd, unregister_fd);
      }
    }

    sweep_timeouts(ctx);
  }
}
