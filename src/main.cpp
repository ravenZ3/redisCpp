#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <memory>
#include <algorithm>
#include <chrono>

std::vector<std::string> parseRESP(const std::string &s)
{
  std::vector<std::string> tokens;
  int i = 1;
  int arr_count = 0;
  while (isdigit(s[i]))
    arr_count = arr_count * 10 + (s[i++] - '0');
  i += 2;

  for (int j = 0; j < arr_count; ++j)
  {
    if (s[i] != '$')
      break;
    i++;
    int len = 0;
    while (isdigit(s[i]))
      len = len * 10 + (s[i++] - '0');
    i += 2;
    std::string curr = s.substr(i, len);
    tokens.push_back(curr);
    i += len + 2;
  }

  return tokens;
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
  std::unordered_map<std::string, std::string> fields;
};
struct ServerContext
{
  std::unordered_map<std::string, std::string> kv;
  std::unordered_map<std::string, std::deque<std::string>> lists;
  std::unordered_map<std::string, std::condition_variable> cvs;
  std::unordered_map<std::string, std::deque<Entry>> streams;
  std::mutex mtx;
};

// ============================
// Command Base Class
// ============================
class Command
{
public:
  virtual ~Command() = default;
  virtual void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) = 0;
};

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
    {
      std::lock_guard<std::mutex> lock(ctx.mtx);
      ctx.kv[key] = value;
    }
    send_simple(client_fd, "OK");

    // PX expiry option
    if (tokens.size() > 4 && tokens[3] == "PX")
    {
      int ttl = std::stoi(tokens[4]);
      std::thread([&ctx, key, ttl]()
                  {
                std::this_thread::sleep_for(std::chrono::milliseconds(ttl));
                std::lock_guard<std::mutex> lock(ctx.mtx);
                ctx.kv.erase(key); })
          .detach();
    }
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
    std::lock_guard<std::mutex> lock(ctx.mtx);
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
    std::lock_guard<std::mutex> lock(ctx.mtx);
    auto &dq = ctx.lists[key];
    for (int i = 2; i < tokens.size(); ++i)
      dq.push_front(tokens[i]);
    send_integer(client_fd, dq.size());
    if (ctx.cvs.count(key))
      ctx.cvs[key].notify_one();
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
    std::lock_guard<std::mutex> lock(ctx.mtx);
    auto &dq = ctx.lists[key];
    for (int i = 2; i < tokens.size(); ++i)
      dq.push_back(tokens[i]);
    send_integer(client_fd, dq.size());
    if (ctx.cvs.count(key))
      ctx.cvs[key].notify_one();
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

    std::lock_guard<std::mutex> lock(ctx.mtx);
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
    std::lock_guard<std::mutex> lock(ctx.mtx);
    int len = ctx.lists.count(key) ? ctx.lists[key].size() : 0;
    send_integer(client_fd, len);
  }
};

class BLPopCommand : public Command
{
public:
  void execute(ServerContext &ctx, int client_fd, const std::vector<std::string> &tokens) override
  {
    if (tokens.size() < 3)
      return send_error(client_fd, "wrong number of arguments");
    std::string key = tokens[1];
    double timeout = std::stod(tokens[2]);
    std::unique_lock<std::mutex> lock(ctx.mtx);

    auto pop_value = [&](std::deque<std::string> &dq) -> std::string
    {
      std::string val = dq.front();
      dq.pop_front();
      return val;
    };

    if (ctx.lists.count(key) && !ctx.lists[key].empty())
    {
      std::string val = pop_value(ctx.lists[key]);
      lock.unlock();

      std::string resp = "*2\r\n";
      send(client_fd, resp.c_str(), resp.size(), 0);
      send_bulk(client_fd, key);
      send_bulk(client_fd, val);
    }
    else
    {
      if (!ctx.cvs.count(key))
        ctx.cvs[key];
      bool notified = false;

      if (timeout == 0)
      {
        ctx.cvs[key].wait(lock, [&]
                          { return ctx.lists.count(key) && !ctx.lists[key].empty(); });
        notified = true;
      }
      else
      {
        notified = ctx.cvs[key].wait_for(lock, std::chrono::duration<double>(timeout),
                                         [&]
                                         { return ctx.lists.count(key) && !ctx.lists[key].empty(); });
      }

      if (notified)
      {
        std::string val = pop_value(ctx.lists[key]);
        lock.unlock();

        std::string resp = "*2\r\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
        send_bulk(client_fd, key);
        send_bulk(client_fd, val);
      }
      else
      {
        const char *nil = "*-1\r\n";
        send(client_fd, nil, strlen(nil), 0);
      }
    }
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
    std::lock_guard<std::mutex> lock(ctx.mtx);

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
    std::lock_guard<std::mutex> lock(ctx.mtx);

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
    

    std::lock_guard<std::mutex> lock(ctx.mtx);
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
        e.fields[tokens[i]] = tokens[i + 1];
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
        e.fields[tokens[i]] = tokens[i + 1];
      }
      ctx.streams[stream_key].push_back(e);
    }

    send_simple(client_fd, generated_id);
  }
};

// ============================
// Command Registry
// ============================
std::unordered_map<std::string, std::unique_ptr<Command>> command_map;

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
}

// ============================
// Worker Thread
// ============================
void handle_client(ServerContext &ctx, int client_fd)
{
  char buffer[1024];
  while (true)
  {
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
      break;

    buffer[bytes_read] = '\0';
    auto tokens = parseRESP(buffer);
    if (tokens.empty())
      continue;

    std::string cmd = tokens[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    if (command_map.count(cmd))
      command_map[cmd]->execute(ctx, client_fd, tokens);
    else
      send_error(client_fd, "unknown command");
  }
  close(client_fd);
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

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) != 0)
  {
    perror("bind failed");
    return 1;
  }

  listen(server_fd, 5);
  std::cout << "Server listening on port 6379...\n";

  while (true)
  {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &len);
    std::thread(handle_client, std::ref(ctx), client_fd).detach();
  }

  close(server_fd);
  return 0;
}
