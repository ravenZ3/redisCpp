#pragma once
// RESP2 protocol: request parsing and reply writers.

#include <string>
#include <vector>

// Parses a single RESP array-of-bulk-strings command from the front of `buf`.
// Returns false if the buffer doesn't yet contain a complete command (caller
// should wait for more bytes). On success, `consumed` is set to the number of
// bytes to erase from the front of the buffer.
bool tryParseRESP(const std::string &buf, size_t &consumed, std::vector<std::string> &tokens);

void send_raw(int fd, const std::string &data);
void send_simple(int fd, const std::string &msg);
void send_bulk(int fd, const std::string &msg);
void send_integer(int fd, int num);
void send_array_header(int fd, size_t count);
void send_nil(int fd);
void send_nil_array(int fd);
void send_error(int fd, const std::string &msg);
