// Connection and string commands: PING, ECHO, SET, GET, TYPE.

#include "command.hpp"
#include "../resp.hpp"

#include <chrono>

namespace
{
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
    const std::string &key = tokens[1];
    ctx.kv[key] = tokens[2];
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
    const std::string &key = tokens[1];
    check_expired(ctx, key);
    auto it = ctx.kv.find(key);
    if (it != ctx.kv.end())
      send_bulk(client_fd, it->second);
    else
      send_nil(client_fd);
  }
};

class TypeCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 2)
      return send_error(client_fd, "wrong number of arguments for TYPE");
    const std::string &key = tokens[1];
    check_expired(ctx, key);

    if (ctx.kv.count(key))
      send_simple(client_fd, "string");
    else if (ctx.lists.count(key))
      send_simple(client_fd, "list");
    else if (ctx.streams.count(key))
      send_simple(client_fd, "stream");
    else
      send_simple(client_fd, "none");
  }
};
} // namespace

void register_string_commands(CommandTable &table)
{
  table["PING"] = std::make_unique<PingCommand>();
  table["ECHO"] = std::make_unique<EchoCommand>();
  table["SET"] = std::make_unique<SetCommand>();
  table["GET"] = std::make_unique<GetCommand>();
  table["TYPE"] = std::make_unique<TypeCommand>();
}
