// epoll variant of the benchmark server. Command logic is 100% shared with
// main_select.cpp via redis_common.hpp; only the multiplexer differs.
#include "redis_common.hpp"
#include <sys/epoll.h>
#include <cstdlib>

int main(int argc, char **argv)
{
  int port = argc > 1 ? std::atoi(argv[1]) : 6379;

  ServerContext ctx;
  register_commands();

  int server_fd = make_listen_socket(port);
  std::cout << "[epoll] listening on port " << port << "...\n";

  int epfd = epoll_create1(0);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  auto register_fd = [&](int fd) -> bool
  {
    epoll_event cev{};
    cev.events = EPOLLIN;
    cev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &cev);
    return true;
  };
  auto unregister_fd = [&](int fd)
  {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
  };

  const int MAX_EVENTS = 1024;
  std::vector<epoll_event> events(MAX_EVENTS);

  while (true)
  {
    int timeout_ms = compute_timeout(ctx);
    int n = epoll_wait(epfd, events.data(), MAX_EVENTS, timeout_ms);

    for (int i = 0; i < n; ++i)
    {
      int fd = events[i].data.fd;
      if (fd == server_fd)
      {
        accept_connections(ctx, server_fd, register_fd);
        continue;
      }
      if (events[i].events & (EPOLLHUP | EPOLLERR))
      {
        disconnect_client(ctx, fd, unregister_fd);
        continue;
      }
      if (events[i].events & EPOLLIN)
      {
        handle_client_event(ctx, fd, unregister_fd);
      }
    }

    sweep_timeouts(ctx);
  }
}
