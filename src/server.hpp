#pragma once
// Event-loop plumbing shared by every I/O multiplexer (epoll, select).
//
// Only two things differ between multiplexers: how a socket is registered
// and unregistered, and the wait loop itself. The first is the Poller
// interface below; the second lives in each main_*.cpp.

#include "context.hpp"

#include <string>

class Poller
{
public:
  virtual ~Poller() = default;
  // Start watching a newly accepted client. Returning false rejects the
  // connection (e.g. select()'s FD_SETSIZE limit).
  virtual bool add(int fd) = 0;
  // Stop watching a client, called before its fd is closed.
  virtual void remove(int fd) = 0;
};

// Creates a non-blocking listening socket on `port`. Exits on bind failure.
int make_listen_socket(int port);

// Accepts every pending connection on the listening socket.
void accept_connections(ServerContext &ctx, int server_fd, Poller &poller);

// Reads whatever the client sent and runs every complete command in it.
void handle_client_event(ServerContext &ctx, int fd, Poller &poller);

void disconnect_client(ServerContext &ctx, int fd, Poller &poller);

// Parses and dispatches every complete command in the client's read buffer,
// stopping early if a command (BLPOP) blocks it.
void process_buffer(ServerContext &ctx, int fd);

// Wakes clients blocked on `key` (FIFO order) while the list has data.
void serve_blocked_waiters(ServerContext &ctx, const std::string &key);

// How long the multiplexer may sleep before a BLPOP deadline needs handling,
// in milliseconds. -1 means block indefinitely.
int compute_timeout(ServerContext &ctx);

// Replies to BLPOP waiters whose deadline passed, and drops expired keys.
void sweep_timeouts(ServerContext &ctx);
