#include "resp.hpp"

#include <sys/socket.h>

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

void send_raw(int fd, const std::string &data)
{
  send(fd, data.c_str(), data.size(), 0);
}

void send_simple(int fd, const std::string &msg)
{
  send_raw(fd, "+" + msg + "\r\n");
}

void send_bulk(int fd, const std::string &msg)
{
  send_raw(fd, "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n");
}

void send_integer(int fd, int num)
{
  send_raw(fd, ":" + std::to_string(num) + "\r\n");
}

void send_array_header(int fd, size_t count)
{
  send_raw(fd, "*" + std::to_string(count) + "\r\n");
}

void send_nil(int fd)
{
  send_raw(fd, "$-1\r\n");
}

void send_nil_array(int fd)
{
  send_raw(fd, "*-1\r\n");
}

void send_error(int fd, const std::string &msg)
{
  send_raw(fd, "-ERR " + msg + "\r\n");
}
