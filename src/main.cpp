#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <deque>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>
#include <algorithm>
#include <chrono>

// ============================
// RESP Request Parsing
// ============================
// Parses a single RESP array-of-bulk-strings command from the front of `buf`.
// Returns false if the buffer doesn't yet contain a complete command (caller
// should wait for more bytes). On success, `consumed` is set to the number of
// bytes to erase from the front of the buffer.
bool tryParseRESP(const std::string &buf, size_t &consumed, std::vector<std::string> &tokens)
{
  tokens.clear();
  size_t n = buf.size();
  if (n == 0)
    return false;
  if (buf[0] != '*')
    return false;

  size_t i = 1;
  size_t start = i;
  while (i < n && buf[i] != '\r')
    i++;
  if (i + 1 >= n)
    return false;
  int arr_count = std::stoi(buf.substr(start, i - start));
  i += 2;

  std::vector<std::string> result;
  for (int j = 0; j < arr_count; ++j)
  {
    if (i >= n)
      return false;
    if (buf[i] != '$')
      return false;
    i++;
    start = i;
    while (i < n && buf[i] != '\r')
      i++;
    if (i + 1 >= n)
      return false;
    int len = std::stoi(buf.substr(start, i - start));
    i += 2;
    if (i + (size_t)len + 2 > n)
      return false;
    result.push_back(buf.substr(i, len));
    i += len + 2;
  }

  tokens = std::move(result);
  consumed = i;
  return true;
}

// ============================
// RESP Response Helpers
// ============================
void send_simple(int fd, const std::string &msg)
{
  std::string resp = "+" + msg + "\r\n";
  send(fd, resp.c_str(), resp.size(), 0);
}
void send_bulk(int fd, const std::string &msg)
{
  std::string resp = "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
  send(fd, resp.c_str(), resp.size(), 0);
}
void send_integer(int fd, int num)
{
  std::string resp = ":" + std::to_string(num) + "\r\n";
  send(fd, resp.c_str(), resp.size(), 0);
}
void send_nil(int fd)
{
  const char *resp = "$-1\r\n";
  send(fd, resp, strlen(resp), 0);
}
void send_nil_array(int fd)
{
  const char *resp = "*-1\r\n";
  send(fd, resp, strlen(resp), 0);
}
void send_error(int fd, const std::string &msg)
{
  std::string resp = "-ERR " + msg + "\r\n";
  send(fd, resp.c_str(), resp.size(), 0);
}

// ============================
// Server Context (shared state)
// ============================
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
bool check_expired(ServerContext &ctx, const std::string &key)
{
  auto it = ctx.expires.find(key);
  if (it == ctx.expires.end())
    return false;
  if (std::chrono::steady_clock::now() >= it->second)
  {
    ctx.kv.erase(key);
    ctx.expires.erase(it);
    return true;
  }
  return false;
}

// ============================
// Command Base Class
// ============================
class Command
{
public:
  virtual ~Command() = default;
  virtual void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) = 0;
};

std::unordered_map<std::string, std::unique_ptr<Command>> command_map;

// Forward declarations: LPUSH/RPUSH need to wake BLPOP waiters, and waking a
// waiter may let it process commands it already sent us while it was blocked.
void process_buffer(ServerContext &ctx, int fd);
void serve_blocked_waiters(ServerContext &ctx, const std::string &key);

// ============================
// Concrete Commands
// ============================
class PingCommand : public Command
{
public:
  void execute(ServerContext &, int client_fd, const std::vector<std::string> &) override
  {
    send_simple(client_fd, "PONG");
  }
};

class EchoCommand : public Command
{
public:
  void execute(ServerContext &, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments");
    send_bulk(client_fd, tokens[1]);
  }
};

class SetCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    std::string value = tokens[2];
    ctx.kv[key] = value;
    ctx.expires.erase(key);

    if (tokens.size() > 4 && tokens[3] == "PX")
    {
      int ttl = std::stoi(tokens[4]);
      ctx.expires[key] = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl);
    }

    send_simple(client_fd, "OK");
  }
};

class GetCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    check_expired(ctx, key);
    auto it = ctx.kv.find(key);
    if (it != ctx.kv.end())
      send_bulk(client_fd, it->second);
    else
      send_nil(client_fd);
  }
};

class LPushCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    auto &dq = ctx.lists[key];
    for (size_t i = 2; i < tokens.size(); ++i)
      dq.push_front(tokens[i]);
    send_integer(client_fd, dq.size());
    serve_blocked_waiters(ctx, key);
  }
};

class RPushCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    auto &dq = ctx.lists[key];
    for (size_t i = 2; i < tokens.size(); ++i)
      dq.push_back(tokens[i]);
    send_integer(client_fd, dq.size());
    serve_blocked_waiters(ctx, key);
  }
};

class LRangeCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 4)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    int start = std::stoi(tokens[2]);
    int stop = std::stoi(tokens[3]);

    auto it = ctx.lists.find(key);
    if (it == ctx.lists.end())
    {
      std::string empty = "*0\r\n";
      send(client_fd, empty.c_str(), empty.size(), 0);
      return;
    }

    auto &v = it->second;
    int n = v.size();
    if (start < 0)
      start = n + start;
    if (stop < 0)
      stop = n + stop;
    if (start < 0)
      start = 0;
    if (stop >= n)
      stop = n - 1;
    if (start > stop)
    {
      std::string empty = "*0\r\n";
      send(client_fd, empty.c_str(), empty.size(), 0);
      return;
    }

    std::string resp = "*" + std::to_string(stop - start + 1) + "\r\n";
    send(client_fd, resp.c_str(), resp.size(), 0);
    for (int i = start; i <= stop; ++i)
      send_bulk(client_fd, v[i]);
  }
};

class LLenCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    int len = ctx.lists.count(key) ? ctx.lists[key].size() : 0;
    send_integer(client_fd, len);
  }
};

// BLPOP no longer blocks a thread. If data is available it responds
// immediately; otherwise it registers the client as a waiter and returns.
// The event loop resumes it later via serve_blocked_waiters() or the
// timeout sweep.
class BLPopCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    double timeout = std::stod(tokens[2]);

    auto lit = ctx.lists.find(key);
    if (lit != ctx.lists.end() && !lit->second.empty())
    {
      std::string val = lit->second.front();
      lit->second.pop_front();

      std::string resp = "*2\r\n";
      send(client_fd, resp.c_str(), resp.size(), 0);
      send_bulk(client_fd, key);
      send_bulk(client_fd, val);
      return;
    }

    auto cit = ctx.clients.find(client_fd);
    if (cit == ctx.clients.end())
      return;
    Client &client = *cit->second;
    client.blocked = true;
    client.blocked_key = key;
    if (timeout > 0)
    {
      client.has_deadline = true;
      client.deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeout));
    }
    else
    {
      client.has_deadline = false;
    }
    ctx.blocked_on_key[key].push_back(client_fd);
  }
};

class TypeCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
    {
      return send_error(client_fd, "wrong number of arguments for TYPE");
    }
    std::string key = tokens[1];
    check_expired(ctx, key);

    if (ctx.kv.count(key))
    {
      send_simple(client_fd, "string");
    }
    else if (ctx.lists.count(key))
    {
      send_simple(client_fd, "list");
    }
    else if (ctx.streams.count(key))
    {
      send_simple(client_fd, "stream");
    }
    else
    {
      send_simple(client_fd, "none");
    }
  }
};

class LPopCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments");

    std::string key = tokens[1];
    auto it = ctx.lists.find(key);
    if (it == ctx.lists.end() || it->second.empty())
    {
      send_nil(client_fd);
      return;
    }

    if (tokens.size() == 2)
    {
      // LPOP key -> single element, return bulk string
      std::string val = it->second.front();
      it->second.pop_front();
      send_bulk(client_fd, val);
    }
    else
    {
      // LPOP key N -> multiple elements, return array
      int N = std::stoi(tokens[2]);
      if (N < 0)
        N = 0;

      int count = std::min(N, static_cast<int>(it->second.size()));
      std::string header = "*" + std::to_string(count) + "\r\n";
      send(client_fd, header.c_str(), header.size(), 0);

      for (int i = 0; i < count; i++)
      {
        std::string val = it->second.front();
        it->second.pop_front();
        send_bulk(client_fd, val);
      }
    }
  }
};

class XAddCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    // Implementation for XADD command goes here
    std::string stream_key = tokens[1];
    std::string entry_id = tokens[2];
    std::string generated_id;
    if (tokens.size() < 5 || (tokens.size() - 3) % 2 != 0)
    {
      return send_error(client_fd, "wrong number of arguments for XADD");
    }

    // validate entry ID format (basic validation)
    size_t dash_pos = entry_id.find('-');

    if (entry_id != "*" && dash_pos == std::string::npos)
    {
      return send_error(client_fd, "invalid entry ID format");
    }

    std::string ts, seq;
    if (entry_id != "*")
    {
      ts = entry_id.substr(0, dash_pos);
      seq = entry_id.substr(dash_pos + 1);
    }

    if (ctx.streams.count(stream_key) == 0)
    {
      // New stream
      if (entry_id == "*")
      {
        ts = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
        seq = "0";
      }
      else if (seq == "*")
      {
        seq = "1";
      }

      entry_id = ts + "-" + seq;

      Entry e;
      e.id = entry_id;
      generated_id = entry_id;
      // Store field-value pairs in the Entry's fields map
      for (size_t i = 3; i < tokens.size(); i += 2)
      {
        e.fields.emplace_back(tokens[i], tokens[i + 1]);
      }
      ctx.streams[stream_key].push_back(e);
    }
    else
    {
      // Existing stream
      auto &last = ctx.streams[stream_key].back();
      std::string last_id = last.id;
      size_t last_dash_pos = last_id.find('-');
      long long int last_ts = std::stoll(last_id.substr(0, last_dash_pos));
      long long int last_seq = std::stoll(last_id.substr(last_dash_pos + 1));

      if (entry_id == "*")
      {
        // Full auto-generation
        if (last_seq == INT64_MAX)
        {
          ts = std::to_string(last_ts + 1);
          seq = "0";
        }
        else
        {
          ts = std::to_string(last_ts);
          seq = std::to_string(last_seq + 1);
        }
        entry_id = ts + "-" + seq;
      }
      else if (seq == "*")
      {
        // Partial auto-generation (timestamp provided, sequence auto-generated)
        long long int current_ts = std::stoll(ts);

        if (current_ts > last_ts)
        {
          seq = "0";
        }
        else if (current_ts == last_ts)
        {
          seq = std::to_string(last_seq + 1);
        }
        else
        {
          return send_error(client_fd, "The ID specified in XADD is equal or smaller than the target stream top item");
        }
        entry_id = ts + "-" + seq;
      }
      else
      {
        // Full ID provided - validate it
        if (std::stoll(ts) == 0 && std::stoll(seq) == 0)
        {
          return send_error(client_fd, "The ID specified in XADD must be greater than 0-0");
        }

        if ((std::stoll(ts) < last_ts) ||
            (std::stoll(ts) == last_ts && std::stoll(seq) <= last_seq))
        {
          return send_error(client_fd, "The ID specified in XADD is equal or smaller than the target stream top item");
        }
      }

      Entry e;
      e.id = entry_id;
      generated_id = entry_id;
      // Store field-value pairs in the Entry's fields map
      for (size_t i = 3; i < tokens.size(); i += 2)
      {
        e.fields.emplace_back(tokens[i], tokens[i + 1]);
      }
      ctx.streams[stream_key].push_back(e);
    }

    send_bulk(client_fd, generated_id);
  }
};

class XRangeCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    std::string key = tokens[1];
    std::string start = tokens[2], end = tokens[3];

    if (ctx.streams.find(key) == ctx.streams.end() || ctx.streams[key].empty())
    {
      send(client_fd, "*0\r\n", 4, 0); // empty array
      return;
    }

    std::deque<Entry> &stream = ctx.streams[key];

    if (start == "-")
      start = stream.front().id;
    if (end == "+")
      end = stream.back().id;

    // collect matching entries
    std::vector<Entry *> matches;
    for (auto &entry : stream)
    {
      if (in_range(entry.id, start, end))
      {
        matches.push_back(&entry);
      }
    }

    // send top-level array header
    std::string header = "*" + std::to_string(matches.size()) + "\r\n";
    send(client_fd, header.c_str(), header.size(), 0);

    // send each entry as nested RESP array
    for (auto *entry : matches)
    {
      std::string entry_header = "*2\r\n";
      send(client_fd, entry_header.c_str(), entry_header.size(), 0);
      send_bulk(client_fd, entry->id);

      std::string fields_header = "*" + std::to_string(entry->fields.size() * 2) + "\r\n";
      send(client_fd, fields_header.c_str(), fields_header.size(), 0);
      for (const auto &kv : entry->fields)
      {
        send_bulk(client_fd, kv.first);
        send_bulk(client_fd, kv.second);
      }
    }
  };

private:
  bool in_range(const std::string &id, const std::string &start, const std::string &end)
  {
    auto parse = [](const std::string &s)
    {
      size_t dash = s.find('-');
      if (dash == std::string::npos)
        return std::pair<long long, long long>(std::stoll(s), 0);
      else
      {
        return std::pair<long long, long long>(
            std::stoll(s.substr(0, dash)), std::stoll(s.substr(dash + 1)));
      }
    };
    auto [t1, s1] = parse(start);
    auto [t2, s2] = parse(end);
    auto [t, s] = parse(id);

    bool ge_start = (t > t1) || (t == t1 && s >= s1);
    bool le_end = (t < t2) || (t == t2 && s <= s2);
    return ge_start && le_end;
  }
};

// ============================
// Command Registry
// ============================
void register_commands()
{
  command_map["PING"] = std::make_unique<PingCommand>();
  command_map["ECHO"] = std::make_unique<EchoCommand>();
  command_map["SET"] = std::make_unique<SetCommand>();
  command_map["GET"] = std::make_unique<GetCommand>();
  command_map["LPUSH"] = std::make_unique<LPushCommand>();
  command_map["LPOP"] = std::make_unique<LPopCommand>();
  command_map["RPUSH"] = std::make_unique<RPushCommand>();
  command_map["LRANGE"] = std::make_unique<LRangeCommand>();
  command_map["LLEN"] = std::make_unique<LLenCommand>();
  command_map["BLPOP"] = std::make_unique<BLPopCommand>();
  command_map["XADD"] = std::make_unique<XAddCommand>();
  command_map["TYPE"] = std::make_unique<TypeCommand>();
  command_map["XRANGE"] = std::make_unique<XRangeCommand>();
}

// ============================
// Event Loop Plumbing
// ============================
void set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Wakes clients blocked on `key` (in FIFO order) as long as there's data for
// them. A woken client's remaining buffered input is drained immediately,
// since it may have pipelined further commands while it was waiting.
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

    std::string resp = "*2\r\n";
    send(fd, resp.c_str(), resp.size(), 0);
    send_bulk(fd, key);
    send_bulk(fd, val);

    client.blocked = false;
    client.has_deadline = false;
    process_buffer(ctx, fd);
  }

  if (waiters.empty())
    ctx.blocked_on_key.erase(wit);
}

// Parses and dispatches every complete command currently sitting in the
// client's read buffer, stopping early if a command (BLPOP) blocks it.
void process_buffer(ServerContext &ctx, int fd)
{
  auto it = ctx.clients.find(fd);
  if (it == ctx.clients.end())
    return;

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

    std::string cmd = tokens[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    auto mit = command_map.find(cmd);
    if (mit != command_map.end())
      mit->second->execute(ctx, fd, tokens);
    else
      send_error(fd, "unknown command");
  }
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

void disconnect_client(ServerContext &ctx, int epfd, int fd)
{
  auto it = ctx.clients.find(fd);
  if (it != ctx.clients.end())
  {
    if (it->second->blocked)
      remove_waiter(ctx, it->second->blocked_key, fd);
    ctx.clients.erase(it);
  }
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
  close(fd);
}

void accept_connections(ServerContext &ctx, int epfd, int server_fd)
{
  while (true)
  {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &len);
    if (client_fd < 0)
      return; // EAGAIN/EWOULDBLOCK: no more pending connections

    set_nonblocking(client_fd);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

    auto client = std::make_unique<Client>();
    client->fd = client_fd;
    ctx.clients[client_fd] = std::move(client);
  }
}

void handle_client_event(ServerContext &ctx, int epfd, int fd)
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
    if (n == 0)
    {
      disconnect_client(ctx, epfd, fd);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    disconnect_client(ctx, epfd, fd);
    return;
  }

  process_buffer(ctx, fd);
}

// Computes how long epoll_wait may sleep before something needs attention
// again (a BLPOP timeout or a key expiry), in milliseconds. -1 means "block
// indefinitely" (nothing pending).
int compute_timeout(ServerContext &ctx)
{
  bool have_deadline = false;
  std::chrono::steady_clock::time_point earliest;

  for (auto &[fd, client] : ctx.clients)
  {
    if (client->blocked && client->has_deadline)
    {
      if (!have_deadline || client->deadline < earliest)
      {
        earliest = client->deadline;
        have_deadline = true;
      }
    }
  }

  if (!have_deadline)
    return -1;

  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now).count();
  return ms > 0 ? (int)ms : 0;
}

// Expires BLPOP waiters whose deadline has passed (replying with a null
// array) and lazily-tracked keys whose PX TTL has elapsed.
void sweep_timeouts(ServerContext &ctx)
{
  auto now = std::chrono::steady_clock::now();

  std::vector<int> expired_fds;
  for (auto &[fd, client] : ctx.clients)
  {
    if (client->blocked && client->has_deadline && now >= client->deadline)
      expired_fds.push_back(fd);
  }
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

// ============================
// Main Server Entry
// ============================
int main()
{
  ServerContext ctx;
  register_commands();

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  set_nonblocking(server_fd);

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) != 0)
  {
    perror("bind failed");
    return 1;
  }

  listen(server_fd, 128);
  std::cout << "Server listening on port 6379...\n";

  int epfd = epoll_create1(0);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  const int MAX_EVENTS = 128;
  epoll_event events[MAX_EVENTS];

  while (true)
  {
    int timeout_ms = compute_timeout(ctx);
    int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);

    for (int i = 0; i < n; ++i)
    {
      int fd = events[i].data.fd;
      if (fd == server_fd)
      {
        accept_connections(ctx, epfd, server_fd);
        continue;
      }
      if (events[i].events & (EPOLLHUP | EPOLLERR))
      {
        disconnect_client(ctx, epfd, fd);
        continue;
      }
      if (events[i].events & EPOLLIN)
      {
        handle_client_event(ctx, epfd, fd);
      }
    }

    sweep_timeouts(ctx);
  }

  close(server_fd);
  return 0;
}
