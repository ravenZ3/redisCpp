#include "context.hpp"

bool check_expired(ServerContext &ctx, const std::string &key)
{
  auto it = ctx.expires.find(key);
  if (it == ctx.expires.end())
    return false;
  if (std::chrono::steady_clock::now() >= it->second)
  {
    ctx.kv.erase(key);
    ctx.expires.erase(it);
    return true;
  }
  return false;
}
