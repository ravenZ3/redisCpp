#include "server.hpp"

#include "commands/command.hpp"
#include "resp.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace
{
void set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Without this, Nagle's algorithm on the server side batches/delays our
// small RESP response packets against the client's delayed-ACK timer,
// which cripples pipelined throughput.
void set_nodelay(int fd)
{
  int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

void remove_waiter(ServerContext &ctx, const std::string &key, int fd)
{
  auto wit = ctx.blocked_on_key.find(key);
  if (wit == ctx.blocked_on_key.end())
    return;
  auto &waiters = wit->second;
  waiters.erase(std::remove(waiters.begin(), waiters.end(), fd), waiters.end());
  if (waiters.empty())
    ctx.blocked_on_key.erase(wit);
}
} // namespace

int make_listen_socket(int port)
{
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  set_nonblocking(server_fd);

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) != 0)
  {
    perror("bind failed");
    exit(1);
  }
  listen(server_fd, 512);
  return server_fd;
}

void accept_connections(ServerContext &ctx, int server_fd, Poller &poller)
{
  while (true)
  {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &len);
    if (client_fd < 0)
      return; // EAGAIN/EWOULDBLOCK: no more pending connections

    if (!poller.add(client_fd))
    {
      close(client_fd);
      continue;
    }
    set_nonblocking(client_fd);
    set_nodelay(client_fd);

    auto client = std::make_unique<Client>();
    client->fd = client_fd;
    ctx.clients[client_fd] = std::move(client);
  }
}

void disconnect_client(ServerContext &ctx, int fd, Poller &poller)
{
  auto it = ctx.clients.find(fd);
  if (it != ctx.clients.end())
  {
    if (it->second->blocked)
      remove_waiter(ctx, it->second->blocked_key, fd);
    ctx.clients.erase(it);
  }
  poller.remove(fd);
  close(fd);
}

void handle_client_event(ServerContext &ctx, int fd, Poller &poller)
{
  char buf[4096];
  while (true)
  {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0)
    {
      auto it = ctx.clients.find(fd);
      if (it == ctx.clients.end())
        return;
      it->second->inbuf.append(buf, n);
      if (n < (ssize_t)sizeof(buf))
        break; // likely drained the socket for now
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
      disconnect_client(ctx, fd, poller);
      return;
    }
    break; // EAGAIN: nothing more to read
  }

  process_buffer(ctx, fd);
}

void process_buffer(ServerContext &ctx, int fd)
{
  while (true)
  {
    auto cit = ctx.clients.find(fd);
    if (cit == ctx.clients.end())
      return; // closed as a side effect of a previous command
    Client &client = *cit->second;
    if (client.blocked)
      return;

    size_t consumed;
    std::vector<std::string> tokens;
    if (!tryParseRESP(client.inbuf, consumed, tokens))
      return;
    client.inbuf.erase(0, consumed);
    if (tokens.empty())
      continue;

    std::string name = tokens[0];
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    if (Command *cmd = find_command(name))
      cmd->execute(ctx, fd, tokens);
    else
      send_error(fd, "unknown command");
  }
}

// A woken client's remaining buffered input is drained immediately, since
// it may have pipelined further commands while it was waiting.
void serve_blocked_waiters(ServerContext &ctx, const std::string &key)
{
  auto wit = ctx.blocked_on_key.find(key);
  if (wit == ctx.blocked_on_key.end())
    return;
  auto &waiters = wit->second;

  while (!waiters.empty())
  {
    auto lit = ctx.lists.find(key);
    if (lit == ctx.lists.end() || lit->second.empty())
      break;

    int fd = waiters.front();
    waiters.pop_front();

    auto cit = ctx.clients.find(fd);
    if (cit == ctx.clients.end())
      continue; // client disconnected while waiting

    Client &client = *cit->second;
    std::string val = lit->second.front();
    lit->second.pop_front();

    send_array_header(fd, 2);
    send_bulk(fd, key);
    send_bulk(fd, val);

    client.blocked = false;
    client.has_deadline = false;
    process_buffer(ctx, fd);
  }

  if (waiters.empty())
    ctx.blocked_on_key.erase(wit);
}

int compute_timeout(ServerContext &ctx)
{
  bool have_deadline = false;
  std::chrono::steady_clock::time_point earliest;

  for (auto &[fd, client] : ctx.clients)
  {
    if (client->blocked && client->has_deadline &&
        (!have_deadline || client->deadline < earliest))
    {
      earliest = client->deadline;
      have_deadline = true;
    }
  }

  if (!have_deadline)
    return -1;

  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now).count();
  return ms > 0 ? (int)ms : 0;
}

void sweep_timeouts(ServerContext &ctx)
{
  auto now = std::chrono::steady_clock::now();

  std::vector<int> expired_fds;
  for (auto &[fd, client] : ctx.clients)
    if (client->blocked && client->has_deadline && now >= client->deadline)
      expired_fds.push_back(fd);

  for (int fd : expired_fds)
  {
    auto it = ctx.clients.find(fd);
    if (it == ctx.clients.end())
      continue;
    Client &client = *it->second;
    remove_waiter(ctx, client.blocked_key, fd);
    client.blocked = false;
    client.has_deadline = false;
    send_nil_array(fd);
    process_buffer(ctx, fd);
  }

  std::vector<std::string> expired_keys;
  for (auto &[key, deadline] : ctx.expires)
    if (now >= deadline)
      expired_keys.push_back(key);
  for (auto &key : expired_keys)
  {
    ctx.kv.erase(key);
    ctx.expires.erase(key);
  }
}
