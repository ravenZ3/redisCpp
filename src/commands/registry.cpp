#include "command.hpp"

namespace
{
CommandTable build_table()
{
  CommandTable table;
  register_string_commands(table);
  register_list_commands(table);
  register_stream_commands(table);
  return table;
}
} // namespace

Command *find_command(const std::string &name)
{
  static CommandTable table = build_table();
  auto it = table.find(name);
  return it == table.end() ? nullptr : it->second.get();
}
