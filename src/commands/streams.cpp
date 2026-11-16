// Stream commands: XADD, XRANGE.

#include "command.hpp"
#include "../resp.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace
{
const char *const kIdTooSmall = "The ID specified in XADD is equal or smaller than the target stream top item";

class XAddCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    std::string stream_key = tokens[1];
    std::string entry_id = tokens[2];
    if (tokens.size() < 5 || (tokens.size() - 3) % 2 != 0)
      return send_error(client_fd, "wrong number of arguments for XADD");

    size_t dash_pos = entry_id.find('-');
    if (entry_id != "*" && dash_pos == std::string::npos)
      return send_error(client_fd, "invalid entry ID format");

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
    }
    else
    {
      // Existing stream: the new ID must be strictly greater than the last one
      const std::string &last_id = ctx.streams[stream_key].back().id;
      size_t last_dash_pos = last_id.find('-');
      long long last_ts = std::stoll(last_id.substr(0, last_dash_pos));
      long long last_seq = std::stoll(last_id.substr(last_dash_pos + 1));

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
        // Partial auto-generation (timestamp given, sequence generated)
        long long current_ts = std::stoll(ts);
        if (current_ts > last_ts)
          seq = "0";
        else if (current_ts == last_ts)
          seq = std::to_string(last_seq + 1);
        else
          return send_error(client_fd, kIdTooSmall);
        entry_id = ts + "-" + seq;
      }
      else
      {
        // Full ID given: validate it
        if (std::stoll(ts) == 0 && std::stoll(seq) == 0)
          return send_error(client_fd, "The ID specified in XADD must be greater than 0-0");
        if ((std::stoll(ts) < last_ts) ||
            (std::stoll(ts) == last_ts && std::stoll(seq) <= last_seq))
          return send_error(client_fd, kIdTooSmall);
      }
    }

    Entry e;
    e.id = entry_id;
    for (size_t i = 3; i < tokens.size(); i += 2)
      e.fields.emplace_back(tokens[i], tokens[i + 1]);
    ctx.streams[stream_key].push_back(std::move(e));

    send_bulk(client_fd, entry_id);
  }
};

class XRangeCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    const std::string &key = tokens[1];
    std::string start = tokens[2], end = tokens[3];

    auto sit = ctx.streams.find(key);
    if (sit == ctx.streams.end() || sit->second.empty())
      return send_array_header(client_fd, 0);

    std::deque<Entry> &stream = sit->second;
    if (start == "-")
      start = stream.front().id;
    if (end == "+")
      end = stream.back().id;

    std::vector<const Entry *> matches;
    for (const auto &entry : stream)
      if (in_range(entry.id, start, end))
        matches.push_back(&entry);

    // Each entry is a nested array: [id, [field, value, field, value, ...]]
    send_array_header(client_fd, matches.size());
    for (const Entry *entry : matches)
    {
      send_array_header(client_fd, 2);
      send_bulk(client_fd, entry->id);
      send_array_header(client_fd, entry->fields.size() * 2);
      for (const auto &[field, value] : entry->fields)
      {
        send_bulk(client_fd, field);
        send_bulk(client_fd, value);
      }
    }
  }

private:
  // IDs compare as (timestamp, sequence) pairs; a bare "123" means "123-0".
  static std::pair<long long, long long> parse_id(const std::string &s)
  {
    size_t dash = s.find('-');
    if (dash == std::string::npos)
      return {std::stoll(s), 0};
    return {std::stoll(s.substr(0, dash)), std::stoll(s.substr(dash + 1))};
  }

  static bool in_range(const std::string &id, const std::string &start, const std::string &end)
  {
    auto lo = parse_id(start);
    auto hi = parse_id(end);
    auto x = parse_id(id);
    return lo <= x && x <= hi;
  }
};
} // namespace

void register_stream_commands(CommandTable &table)
{
  table["XADD"] = std::make_unique<XAddCommand>();
  table["XRANGE"] = std::make_unique<XRangeCommand>();
}
