#include "resp.hpp"

#include <gtest/gtest.h>

namespace
{
struct Parsed
{
  bool ok;
  size_t consumed;
  std::vector<std::string> tokens;
};

Parsed parse(const std::string &buf)
{
  Parsed p{false, 0, {}};
  p.ok = tryParseRESP(buf, p.consumed, p.tokens);
  return p;
}
} // namespace

TEST(Resp, ParsesSingleCommand)
{
  auto p = parse("*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n");
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.consumed, 23u);
  EXPECT_EQ(p.tokens, (std::vector<std::string>{"ECHO", "hey"}));
}

TEST(Resp, ConsumesOnlyFirstOfPipelinedCommands)
{
  std::string one = "*1\r\n$4\r\nPING\r\n";
  auto p = parse(one + one);
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.consumed, one.size());
  EXPECT_EQ(p.tokens, (std::vector<std::string>{"PING"}));
}

TEST(Resp, IncompleteInputAtEveryCutPoint)
{
  std::string full = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nvalue\r\n";
  for (size_t cut = 0; cut < full.size(); ++cut)
  {
    auto p = parse(full.substr(0, cut));
    EXPECT_FALSE(p.ok) << "should wait for more bytes at cut " << cut;
  }
  EXPECT_TRUE(parse(full).ok);
}

TEST(Resp, BulkStringMayContainCRLF)
{
  auto p = parse("*1\r\n$4\r\na\r\nb\r\n");
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.tokens[0], "a\r\nb");
}

TEST(Resp, EmptyBulkString)
{
  auto p = parse("*2\r\n$4\r\nECHO\r\n$0\r\n\r\n");
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.tokens[1], "");
}

TEST(Resp, RejectsNonArrayInput)
{
  EXPECT_FALSE(parse("PING\r\n").ok);
  EXPECT_FALSE(parse("").ok);
}

TEST(Resp, ClearsTokensOnFailure)
{
  std::vector<std::string> tokens{"stale"};
  size_t consumed = 0;
  EXPECT_FALSE(tryParseRESP("*1\r\n$4\r\nPI", consumed, tokens));
  EXPECT_TRUE(tokens.empty());
}
