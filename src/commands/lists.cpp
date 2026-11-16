// List commands: LPUSH, RPUSH, LRANGE, LLEN, LPOP, BLPOP.

#include "command.hpp"
#include "../resp.hpp"
#include "../server.hpp"

#include <algorithm>
#include <chrono>

namespace
{
class LPushCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    const std::string &key = tokens[1];
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
    const std::string &key = tokens[1];
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
    const std::string &key = tokens[1];
    int start = std::stoi(tokens[2]);
    int stop = std::stoi(tokens[3]);

    auto it = ctx.lists.find(key);
    if (it == ctx.lists.end())
      return send_array_header(client_fd, 0);

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
      return send_array_header(client_fd, 0);

    send_array_header(client_fd, stop - start + 1);
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
    auto it = ctx.lists.find(tokens[1]);
    send_integer(client_fd, it == ctx.lists.end() ? 0 : it->second.size());
  }
};

class LPopCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments");

    auto it = ctx.lists.find(tokens[1]);
    if (it == ctx.lists.end() || it->second.empty())
      return send_nil(client_fd);

    if (tokens.size() == 2)
    {
      // LPOP key -> single element, return bulk string
      std::string val = it->second.front();
      it->second.pop_front();
      send_bulk(client_fd, val);
      return;
    }

    // LPOP key N -> multiple elements, return array
    int N = std::max(0, std::stoi(tokens[2]));
    int count = std::min(N, static_cast<int>(it->second.size()));
    send_array_header(client_fd, count);
    for (int i = 0; i < count; i++)
    {
      std::string val = it->second.front();
      it->second.pop_front();
      send_bulk(client_fd, val);
    }
  }
};

// BLPOP doesn't block a thread. If data is available it responds
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
    const std::string &key = tokens[1];
    double timeout = std::stod(tokens[2]);

    auto lit = ctx.lists.find(key);
    if (lit != ctx.lists.end() && !lit->second.empty())
    {
      std::string val = lit->second.front();
      lit->second.pop_front();
      send_array_header(client_fd, 2);
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
    client.has_deadline = timeout > 0;
    if (client.has_deadline)
    {
      client.deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeout));
    }
    ctx.blocked_on_key[key].push_back(client_fd);
  }
};
} // namespace

void register_list_commands(CommandTable &table)
{
  table["LPUSH"] = std::make_unique<LPushCommand>();
  table["RPUSH"] = std::make_unique<RPushCommand>();
  table["LRANGE"] = std::make_unique<LRangeCommand>();
  table["LLEN"] = std::make_unique<LLenCommand>();
  table["LPOP"] = std::make_unique<LPopCommand>();
  table["BLPOP"] = std::make_unique<BLPopCommand>();
}
