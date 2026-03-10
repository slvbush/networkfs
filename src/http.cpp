#include "http.h"

#include <span>
#include <string>

#include "subprojects/cpp-httplib/httplib.h"
#include "util.h"

const char* SERVER_HOST = "nerc.itmo.ru";
const int SERVER_PORT = 80;

int64_t networkfs_http_call(
    const char* token, const char* method, char* response_buffer,
    size_t buffer_size,
    std::span<const std::pair<std::string, std::string>> args) {
  httplib::Client cli(SERVER_HOST, SERVER_PORT);
  cli.set_keep_alive(false);
  cli.set_connection_timeout(10);  // 10 seconds
  cli.set_read_timeout(10);        // 10 seconds
  cli.set_write_timeout(10);       // 10 seconds

  std::string path = "/teaching/os/networkfs/v1/";
  path += token;
  path += "/fs/";
  path += method;

  httplib::Params params;
  for (const auto& [key, value] : args) {
    params.emplace(key, value);
  }

  auto res = cli.Get(path.c_str(), params, httplib::Headers());

  if (!res) {
    switch (res.error()) {
      case httplib::Error::Connection:
        return -ESOCKNOCONNECT;
      case httplib::Error::Read:
        return -ESOCKNOMSGRECV;
      case httplib::Error::Write:
        return -ESOCKNOMSGSEND;
      default:
        return -EHTTPMALFORMED;
    }
  }

  if (res->status != 200) {
    return -EHTTPBADCODE;
  }

  if (res->body.size() < sizeof(int64_t)) {
    return -EPROTMALFORMED;
  }

  int64_t return_value;
  memcpy(&return_value, res->body.data(), sizeof(int64_t));

  size_t response_data_len = res->body.size() - sizeof(int64_t);
  if (response_data_len > buffer_size) {
    return -ENOSPC;
  }

  if (response_data_len > 0) {
    memcpy(response_buffer, res->body.data() + sizeof(int64_t),
           response_data_len);
  }

  return return_value;
}
