#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <bits/stdc++.h>

void DoWork(int client_fd);

std::vector<std::string> f(const std::string &s);

std::unordered_map<std::string, std::string> m;

int main()
{
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
  {
    std::cerr << "socket failed\n";
    return 1;
  }

  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);

  if (bind(server_fd, (sockaddr *)&server_addr, sizeof(server_addr)) != 0)
  {
    std::cerr << "bind failed\n";
    return 1;
  }

  listen(server_fd, 5);

  sockaddr_in client_addr{};
  socklen_t client_len = sizeof(client_addr);
  std::cout << "Waiting for clients...\n";

  while (true)
  {
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
    // std::cout << "This is your server revciving data from client: " << client_fd << std::endl;
    // for (auto i : tokens)
    //   std::cout << i << std::endl;

    if (!tokens.empty())
    {
      if (tokens.size() == 1 && tokens[0] == "PING")
      {
        const char *pong = "+PONG\r\n";
        send(client_fd, pong, strlen(pong), 0);
      }
      else if (tokens.size() == 2 && tokens[0] == "ECHO")
      {
        std::string msg = tokens[1];
        std::string resp = "+" + msg + "\r\n";
        send(client_fd, resp.c_str(), resp.size(), 0);
      }
      else if (tokens.size() == 3 && tokens[0] == "SET")
      {
        m[tokens[1]] = tokens[2];
        const char *okay = "+OK\r\n";
        send(client_fd, okay, strlen(okay), 0);
      }
      else if (tokens.size() == 2 && tokens[0] == "GET")
      {
        auto it = m.find(tokens[1]);
        if (it != m.end())
        {
          std::string msg = m[tokens[1]];
          std::string resp = "+" + msg + "\r\n";
          send(client_fd, resp.c_str(), resp.size(), 0);
        }
        else
        {
          const char *not_found = "-1\r\n";
          send(client_fd, not_found, strlen(not_found), 0);
        }
      }

      else
      {
        const char *err = "-ERR unknown cmd\r\n";
        send(client_fd, err, strlen(err), 0);
      }
    }
  }
  close(client_fd);
}

std::vector<std::string> f(const std::string &s)
{
  std::vector<std::string> tokens;
  int i = 1; // skip '*'
  int arr_count = 0;

  while (isdigit(s[i]))
  {
    arr_count = arr_count * 10 + (s[i] - '0');
    i++;
  }
  i += 2; // skip \r\n

  for (int j = 0; j < arr_count; ++j)
  {
    if (s[i] != '$')
      break;
    i++; // skip $
    int len = 0;
    while (isdigit(s[i]))
    {
      len = len * 10 + (s[i] - '0');
      i++;
    }
    i += 2; // skip \r\n
    std::string curr = s.substr(i, len);
    tokens.push_back(curr);
    i += len + 2; // skip data + \r\n
  }
  return tokens;
}