#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <deque>

std::unordered_map<std::string, std::string> m;
std::mutex mtx;
std::unordered_map<std::string, std::deque<std::string>> list;

void DoWork(int client_fd);
std::vector<std::string> f(const std::string &s);

int main()
{
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return 1;

  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) != 0)
    return 1;
  listen(server_fd, 5);

  std::cout << "Waiting for clients...\n";

  while (true)
  {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &client_len);
    std::thread(DoWork, client_fd).detach();
  }

  close(server_fd);
  return 0;
}

void DoWork(int client_fd)
{
  char buffer[1024];

  while (true)
  {
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
      break;

    buffer[bytes_read] = '\0';
    auto tokens = f(buffer);

    if (tokens.empty())
      continue;

    if (tokens.size() == 1 && tokens[0] == "PING")
    {
      const char *pong = "+PONG\r\n";
      send(client_fd, pong, strlen(pong), 0);
    }
    else if (tokens.size() == 2 && tokens[0] == "ECHO")
    {
      std::string msg = "+" + tokens[1] + "\r\n";
      send(client_fd, msg.c_str(), msg.size(), 0);
    }
    else if (tokens[0] == "SET")
    {
      std::string key = tokens[1];
      std::string value = tokens[2];

      {
        std::lock_guard<std::mutex> lock(mtx);
        m[key] = value;
      }

      const char *okay = "+OK\r\n";
      send(client_fd, okay, strlen(okay), 0);

      if (tokens.size() > 3 && tokens[3] == "PX")
      {
        int time = std::stoi(tokens[4]);
        std::thread([key, time]()
                    {
                    std::this_thread::sleep_for(std::chrono::milliseconds(time));
                    std::lock_guard<std::mutex> lock(mtx);
                    m.erase(key); })
            .detach();
      }
    }
    else if (tokens.size() == 2 && tokens[0] == "GET")
    {
      std::lock_guard<std::mutex> lock(mtx);
      auto it = m.find(tokens[1]);
      if (it != m.end())
      {
        std::string msg = "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
        send(client_fd, msg.c_str(), msg.size(), 0);
      }
      else
      {
        const char *nil = "$-1\r\n";
        send(client_fd, nil, strlen(nil), 0);
      }
    }
    else if (tokens.size() == 2 && tokens[0] == "LLEN")
    {
      std::lock_guard<std::mutex> lock(mtx);
      std::string key = tokens[1];
      auto it = list.find(key);
      if (it == list.end())
      {
        send(client_fd, ":0\r\n", 4, 0);
      }
      else if (it != list.end())
      {
        int N = list[key].size();
        std::string s = ":" + std::to_string(N) + +"\r\n";
        send(client_fd, s.c_str(), s.size(), 0);
      }
    }
    else if (tokens.size() == 2 && tokens[0] == "LPOP")
    {
      std::string key = tokens[1];
      {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = list.find(key);
        if (it == list.end())
        {
          send(client_fd, "-1\r\n", 4, 0);
        }
        else
        {
          std::string front_element = list[key][0];
          list[key].pop_front();
          std::string s = "$" + std::to_string(front_element.size()) + "\r\n" + front_element + "\r\n";
          send(client_fd, s.c_str(), s.size(), 0);
        }
      }
    }
    else if (tokens.size() == 4 && tokens[0] == "LRANGE")
    {
      std::lock_guard<std::mutex> lock(mtx);
      std::string key = tokens[1];
      auto it = list.find(key);
      if (it == list.end())
      {
        std::string empty = "*0\r\n";
        send(client_fd, empty.c_str(), empty.size(), 0);
      }
      else if (it != list.end())
      {
        const auto &v = it->second;
        int start = std::stoi(tokens[2]);
        int stop = std::stoi(tokens[3]);

        // handling the cases on stop and start interators

        // handling the negative cases
        if (stop < 0 || start < 0)
        {
          if (start <= (int)-v.size())
            start = 0;
          if (stop <= (int)-v.size())
            stop = 0;

          if (start < 0)
            start += v.size();

          if (stop < 0)
            stop += v.size();
        }
        // handing the case where
        if (stop > v.size())
          stop = v.size() - 1;
        if (start > stop)
        {
          std::string empty = "*0\r\n";
          send(client_fd, empty.c_str(), empty.size(), 0);
        }

        else
        {
          int n = (stop - start + 1);
          std::string s = "*" + std::to_string(n) + "\r\n";
          send(client_fd, s.c_str(), s.size(), 0);
          for (int i = start; i <= stop; i++)
          {
            std::string curr = v[i];
            std::string s = "$" + std::to_string(curr.size()) + "\r\n" + curr + "\r\n";
            send(client_fd, s.c_str(), s.size(), 0);
          }
        }
      }
    }
    else if (tokens.size() >= 3 && tokens[0] == "RPUSH")
    {
      int n = tokens.size();
      std::lock_guard<std::mutex> lock(mtx);
      std::string key = tokens[1];
      auto it = list.find(key);
      if (it == list.end())
      {
        std::deque<std::string> v;
        for (int i = 2; i < n; i++)
          v.push_back(tokens[i]);
        list[key] = v;
      }
      else
      {
        for (int i = 2; i < n; i++)
          list[key].push_back(tokens[i]);
      }

      std::string resp = ":" + std::to_string(list[key].size()) + "\r\n";
      send(client_fd, resp.c_str(), resp.size(), 0);
    }
    else if (tokens.size() >= 3 && tokens[0] == "LPUSH")
    {
      std::string key = tokens[1];

      {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = list.find(key);
        int n = tokens.size();

        if (it != list.end())
        {
          for (int i = 2; i < n; i++)
          {
            list[key].push_front(tokens[i]);
          }
        }
        else if (it == list.end())
        {
          std::deque<std::string> s;
          for (int i = 2; i < n; i++)
          {
            s.push_front(tokens[i]);
          }
          list[key] = s;
        }
      }
      int t = list[key].size();
      std::string s = ":" + std::to_string(t) + "\r\n";
      send(client_fd, s.c_str(), s.size(), 0);
    }
    else
    {
      const char *err = "-ERR unknown cmd\r\n";
      send(client_fd, err, strlen(err), 0);
    }
  }
  close(client_fd);
}

std::vector<std::string> f(const std::string &s)
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
