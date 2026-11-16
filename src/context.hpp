#pragma once
// Server state: the keyspace (strings, lists, streams) and per-client state.

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Entry
{
  std::string id;
  std::vector<std::pair<std::string, std::string>> fields;
};

// Per-connection state. Everything the event loop needs to know about a
// client lives here instead of on a thread's stack.
struct Client
{
  int fd;
  std::string inbuf;
  bool blocked = false;
  std::string blocked_key;
  bool has_deadline = false;
  std::chrono::steady_clock::time_point deadline;
};

struct ServerContext
{
  std::unordered_map<std::string, std::string> kv;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> expires;
  std::unordered_map<std::string, std::deque<std::string>> lists;
  std::unordered_map<std::string, std::deque<Entry>> streams;

  std::unordered_map<int, std::unique_ptr<Client>> clients;
  // key -> fds of clients blocked in BLPOP on that key, FIFO order
  std::unordered_map<std::string, std::deque<int>> blocked_on_key;
};

// Lazily expires a string key if its PX deadline has passed. Returns true if
// the key was (or already had been) expired.
bool check_expired(ServerContext &ctx, const std::string &key);
