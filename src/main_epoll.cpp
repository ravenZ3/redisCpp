// The server: a single-threaded event loop on epoll.
//
//   redis [port]        (default 6379)

#include "server.hpp"

#include <cstdlib>
#include <iostream>
#include <sys/epoll.h>
#include <vector>

namespace
{
class EpollPoller : public Poller
{
public:
  explicit EpollPoller(int epfd) : epfd_(epfd) {}

  bool add(int fd) override
  {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    return true;
  }

  void remove(int fd) override
  {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  }

private:
  int epfd_;
};
} // namespace

int main(int argc, char **argv)
{
  int port = argc > 1 ? std::atoi(argv[1]) : 6379;

  ServerContext ctx;
  int server_fd = make_listen_socket(port);
  std::cout << "[epoll] listening on port " << port << "...\n";

  int epfd = epoll_create1(0);
  EpollPoller poller(epfd);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  const int MAX_EVENTS = 1024;
  std::vector<epoll_event> events(MAX_EVENTS);

  while (true)
  {
    int n = epoll_wait(epfd, events.data(), MAX_EVENTS, compute_timeout(ctx));

    for (int i = 0; i < n; ++i)
    {
      int fd = events[i].data.fd;
      if (fd == server_fd)
        accept_connections(ctx, server_fd, poller);
      else if (events[i].events & (EPOLLHUP | EPOLLERR))
        disconnect_client(ctx, fd, poller);
      else if (events[i].events & EPOLLIN)
        handle_client_event(ctx, fd, poller);
    }

    sweep_timeouts(ctx);
  }
}
