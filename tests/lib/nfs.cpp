#include "nfs.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <thread>

#include "util.hpp"

namespace fs = std::filesystem;

NfsBucket::NfsBucket() : client("nerc.itmo.ru", 80) {}

void NfsBucket::initialize() {
  auto response = issue();
  this->token_ =
      std::string(response.token, response.token + sizeof(response.token));

  if (!fs::exists(TEST_ROOT)) {
    fs::create_directory(TEST_ROOT);
  }

  pid_t pid = fork();
  if (pid == 0) {
    setenv("NETWORKFS_TOKEN", this->token_.c_str(), 1);
    execl("./networkfs", "./networkfs", "-f", TEST_ROOT.c_str(), NULL);
    perror("execl failed");
    exit(1);
  } else if (pid > 0) {
    this->fuse_pid = pid;
    this->mounted = true;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } else {
    throw std::runtime_error("fork failed");
  }
}

const std::string NfsBucket::token() const { return token_; }

void NfsBucket::unmount(bool do_throw) {
  this->mounted = false;

  bool unmounted = false;
  for (int i = 0; i < 3; i++) {
    std::string cmd = "fusermount3 -u " + TEST_ROOT.string();
    if (system(cmd.c_str()) == 0) {
      unmounted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (this->fuse_pid > 0) {
    kill(this->fuse_pid, SIGKILL);
    waitpid(this->fuse_pid, nullptr, 0);
    this->fuse_pid = 0;
  }

  if (!unmounted) {
    if (do_throw) {
      throw std::runtime_error(
          "Filesystem can not be unmounted. Try `sudo umount " +
          TEST_ROOT.string() + '`');
    } else {
      std::cerr << "error: Filesystem can not be unmounted" << std::endl;
    }
  }
}

NfsBucket::~NfsBucket() {
  if (this->mounted) {
    std::cerr << "warning: you shouldn't rely on filesystem unmounting in "
                 "destructor, use NfsBucket::unmount"
              << std::endl;
    unmount(false);
  }
}

void NfsBucket::clear(ino_t ino) {
  auto response = list(ino);
  if (response.status == 1) return;
  if (response.status != 0)
    throw std::runtime_error("Unexpected status " +
                             std::to_string(response.status));

  for (int i = 0; i < response.entries_count; i++) {
    if (response.entries[i].entry_type == EntryType::FILE) {
      if (uint64_t status = unlink(ino, response.entries[i].name).status) {
        throw std::runtime_error("Unexpected status " + std::to_string(status));
      }
    } else {
      clear(response.entries[i].ino);
      if (uint64_t status = rmdir(ino, response.entries[i].name).status) {
        throw std::runtime_error("Unexpected status " + std::to_string(status));
      }
    }
  }
}

std::string NfsBucket::call_api(const std::string& uri,
                                const httplib::Params& params,
                                size_t attempts) {
  std::string full_uri = std::string(API_BASE);

  if (!uri.starts_with("token")) {
    full_uri += token();
    full_uri += "/";
  }

  full_uri += uri;

  auto req = client.Get(full_uri, params, {});

  if (!req) {
    if (attempts == MAX_ATTEMPTS) {
      throw std::runtime_error("Request failed: " + to_string(req.error()));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(REQUEST_DELAY));
    return call_api(uri, params, attempts + 1);
  }

  if (req->status != 200) {
    throw std::runtime_error("Request failed with status code " +
                             std::to_string(req->status));
  }

  return req->body;
}

template <typename T>
T convert(const std::string& from) {
  T value;
  memcpy(&value, from.data(), from.size());
  return value;
}

struct token_response NfsBucket::issue() {
  return convert<token_response>(call_api("token/issue"));
}

struct list_response NfsBucket::list(ino_t inode) {
  return convert<list_response>(
      call_api("fs/list", {{"inode", std::to_string(inode)}}));
}

struct create_response NfsBucket::create(ino_t parent, const std::string& name,
                                         EntryType type) {
  return convert<create_response>(call_api(
      "fs/create", {{"parent", std::to_string(parent)},
                    {"name", name},
                    {"type", type == EntryType::FILE ? "file" : "directory"}}));
}

struct read_response NfsBucket::read(ino_t inode) {
  return convert<read_response>(
      call_api("fs/read", {{"inode", std::to_string(inode)}}));
}

struct empty_response NfsBucket::write(ino_t inode,
                                       const std::string& content) {
  return convert<empty_response>(call_api(
      "fs/write", {{"inode", std::to_string(inode)}, {"content", content}}));
}

struct empty_response NfsBucket::link(ino_t source, ino_t parent,
                                      const std::string& name) {
  return convert<empty_response>(
      call_api("fs/link", {{"source", std::to_string(source)},
                           {"parent", std::to_string(parent)},
                           {"name", name}}));
}
struct empty_response NfsBucket::unlink(ino_t parent, const std::string& name) {
  return convert<empty_response>(call_api(
      "fs/unlink", {{"parent", std::to_string(parent)}, {"name", name}}));
}

struct empty_response NfsBucket::rmdir(ino_t parent, const std::string& name) {
  return convert<empty_response>(call_api(
      "fs/rmdir", {{"parent", std::to_string(parent)}, {"name", name}}));
}

struct lookup_response NfsBucket::lookup(ino_t parent,
                                         const std::string& name) {
  return convert<lookup_response>(call_api(
      "fs/lookup", {{"parent", std::to_string(parent)}, {"name", name}}));
}
