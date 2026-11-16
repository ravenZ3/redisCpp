#pragma once
// Command interface and lookup. Each command family registers itself into
// the table from its own source file.

#include "../context.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Command
{
public:
  virtual ~Command() = default;
  virtual void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) = 0;
};

using CommandTable = std::unordered_map<std::string, std::unique_ptr<Command>>;

// Returns the command registered under `name` (uppercase), or nullptr.
Command *find_command(const std::string &name);

void register_string_commands(CommandTable &table); // PING ECHO SET GET TYPE
void register_list_commands(CommandTable &table);   // LPUSH RPUSH LRANGE LLEN LPOP BLPOP
void register_stream_commands(CommandTable &table); // XADD XRANGE
