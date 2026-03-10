#include "inode.h"

#define FUSE_USE_VERSION 317

#include <dirent.h>
#include <fuse_lowlevel.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "http.h"
#include "util.h"

#define MAX_FILE_SIZE 512
#define MAX_DIR_SIZE 4096
#define DEF_DIR_RIGHTS 0755  // 0, S_IRWXU, S_IRGRP | S_IXGRP, S_IROTH | S_IXOTH
#define DEF_FILE_RIGHTS 0644  // 0, S_IRUSR | S_IWUSR, S_IRGRP, S_IROTH

static const std::unordered_map<int, int> server_to_posix = {
    {0, EXIT_SUCCESS}, {1, ENOENT},      {2, EISDIR}, {3, ENOTDIR},
    {4, ENOENT},       {5, EEXIST},      {6, EFBIG},  {7, ENOMEM},
    {8, ENOTEMPTY},    {9, ENAMETOOLONG}};

static const std::unordered_map<int, std::string> error_messages = {
    {1, " object with given inode number was not found\n"},
    {2, " given object is not a file\n"},
    {3, " given object is not a directory\n"},
    {4, " no such name in specified directory\n"},
    {5, " file with such name already exists\n"},
    {6, " file size limit is reached(512 byte)\n"},
    {7, " entry quantity limit is reached(16)\n"},
    {8, " directory is not empty\n"},
    {9, " file name length limit reached(255)\n"}};

void process_error_code(const std::string& foo_name, int code,
                        fuse_req_t& req) {
  if (code) {
    std::cerr << foo_name << ":" << error_messages.find(code)->second;
  }
  fuse_reply_err(req, server_to_posix.find(code)->second);
}

void networkfs_init(void* userdata, struct fuse_conn_info* conn) {
  (void)userdata;
  (void)conn;
  // TODO: Implement initialization if needed
}

void networkfs_destroy(void* private_data) {
  // Token string, which was allocated in main.
  free(private_data);
}

struct entry_info {
  unsigned char entry_type;  // DT_DIR (4) or DT_REG (8)
  ino_t ino;
};

struct content {
  uint64_t content_length;
  char content[MAX_FILE_SIZE];
};

struct Returns {};

std::string ino_to_string(Returns ret, fuse_ino_t ino) {
  char ino_s[21];
  ino_to_string(ino_s, ino);
  return ino_s;
}

template <typename U, std::size_t sz>
std::pair<U, int> server_call(
    const char* name,
    const std::array<std::pair<std::string, std::string>, sz>& args) {
  const char* tok = getenv("NETWORKFS_TOKEN");
  U ret;
  int code = networkfs_http_call(tok, name, reinterpret_cast<char*>(&ret),
                                 sizeof(ret), args);
  return {ret, code};
}

fuse_entry_param defaulted_entry_param(unsigned char entry_type,
                                       fuse_ino_t ino) {
  fuse_entry_param e = {0};
  e.ino = ino;
  e.entry_timeout = 1.0;
  e.attr_timeout = 1.0;
  if (entry_type == DT_DIR) {
    e.attr.st_mode = S_IFDIR | DEF_DIR_RIGHTS;
    e.attr.st_nlink = 2;
    e.attr.st_size = MAX_DIR_SIZE;
  } else {
    content c =
        server_call<content>("read",
                             std::array<std::pair<std::string, std::string>, 1>{
                                 {{"inode", ino_to_string(Returns{}, ino)}}})
            .first;
    e.attr.st_mode = S_IFREG | DEF_FILE_RIGHTS;
    e.attr.st_nlink = 1;
    e.attr.st_size = c.content_length;
  }
  e.attr.st_ino = ino;
  e.attr.st_uid = getuid();
  e.attr.st_gid = getgid();
  return e;
}

void networkfs_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  auto res = server_call<entry_info>(
      "lookup",
      std::array<std::pair<std::string, std::string>, 2>{
          {{"parent", ino_to_string(Returns{}, parent)}, {"name", name}}});

  if (res.second == 0) {
    fuse_entry_param e =
        defaulted_entry_param(res.first.entry_type, res.first.ino);
    fuse_reply_entry(req, &e);
  } else {
    process_error_code("lookup", res.second, req);
  }
}

struct stat defaulted_stat(
    fuse_ino_t ino, struct fuse_file_info* fi) {  // called with fi != nullptr
  struct stat st = {0};
  st.st_ino = ino;
  if (ino == 1) {
    st.st_mode = S_IFDIR | DEF_DIR_RIGHTS;
    st.st_nlink = 2;
    st.st_size = MAX_DIR_SIZE;
  } else {
    st.st_mode = S_IFREG | DEF_FILE_RIGHTS;
    st.st_nlink = 1;
    st.st_size = reinterpret_cast<std::vector<char>*>(fi->fh)->size();
  }
  st.st_uid = getuid();
  st.st_gid = getgid();
  return st;
}

void networkfs_getattr(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info* fi) {
  struct stat st = defaulted_stat(ino, fi);
  fuse_reply_attr(req, &st, 0);
}

struct entries {
  size_t entries_count;
  struct entry {
    unsigned char entry_type;  // DT_DIR (4) or DT_REG (8)
    ino_t ino;
    char name[256];
  } entries[16];
};

void networkfs_iterate(fuse_req_t req, fuse_ino_t i_ino, size_t size, off_t off,
                       struct fuse_file_info* fi) {
  auto res = server_call<entries>(
      "list", std::array<std::pair<std::string, std::string>, 1>{
                  {{"inode", ino_to_string(Returns{}, i_ino)}}});
  size_t in_buf_off = 0;
  if (res.second == 0) {
    char buf[MAX_DIR_SIZE];
    for (int i = off; i != res.first.entries_count; ++i) {
      struct stat s = {0};
      if (res.first.entries[i].entry_type == DT_DIR) {
        s.st_mode = S_IFDIR | DEF_DIR_RIGHTS;
      } else {
        s.st_mode = S_IFREG | DEF_FILE_RIGHTS;
      }
      s.st_ino = res.first.entries[i].ino;
      size_t e_len =
          fuse_add_direntry(req, buf + in_buf_off, MAX_DIR_SIZE - in_buf_off,
                            res.first.entries[i].name, &s, i + 1);
      if (MAX_DIR_SIZE - in_buf_off < e_len) {
        break;
      }
      in_buf_off += e_len;
    }
    fuse_reply_buf(req, buf, in_buf_off);
  } else {
    process_error_code("iterate", res.second, req);
  }
}

std::pair<fuse_ino_t, int> create_impl(fuse_ino_t parent, const char* name,
                                       const char* type) {
  return server_call<ino_t>("create",
                            std::array<std::pair<std::string, std::string>, 3>{
                                {{"parent", ino_to_string(Returns{}, parent)},
                                 {"name", name},
                                 {"type", type}}});
}

void networkfs_create(fuse_req_t req, fuse_ino_t parent, const char* name,
                      mode_t mode, struct fuse_file_info* fi) {
  if (fi) {
    auto res = create_impl(parent, name, "file");
    if (res.second == 0) {
      fuse_entry_param ep = defaulted_entry_param(DT_REG, res.first);

      std::vector<char>* buf = new std::vector<char>;
      fi->fh = reinterpret_cast<uint64_t>(buf);
      fuse_reply_create(req, &ep, fi);
    } else {
      process_error_code("create", res.second, req);
    }
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void delete_impl(fuse_req_t& req, fuse_ino_t parent, const char* name,
                 const char* to_call) {
  const char* tok = getenv("NETWORKFS_TOKEN");

  std::array<std::pair<std::string, std::string>, 2> storage{
      {{"parent", ino_to_string(Returns{}, parent)}, {"name", name}}};
  uint64_t res = networkfs_http_call(tok, to_call, nullptr, 0, storage);
  process_error_code("unlink", res, req);
}

void networkfs_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  delete_impl(req, parent, name, "unlink");
}

void networkfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                     mode_t mode) {
  auto res = create_impl(parent, name, "directory");
  if (res.second == 0) {
    fuse_entry_param ep = defaulted_entry_param(DT_DIR, res.first);
    fuse_reply_entry(req, &ep);
  } else {
    process_error_code("mkdir", res.second, req);
  }
}

void networkfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  delete_impl(req, parent, name, "rmdir");
}

void networkfs_open(fuse_req_t req, fuse_ino_t i_ino, fuse_file_info* fi) {
  if (fi) {
    if (fi->flags & O_TRUNC) {
      std::vector<char>* buf = new std::vector<char>;
      delete (reinterpret_cast<std::vector<char>*>(fi->fh));
      fi->fh = reinterpret_cast<uint64_t>(buf);
      fuse_reply_open(req, fi);
    } else {
      auto res = server_call<content>(
          "read", std::array<std::pair<std::string, std::string>, 1>{
                      {{"inode", ino_to_string(Returns{}, i_ino)}}});
      if (res.second == 0) {
        delete (reinterpret_cast<std::vector<char>*>(fi->fh));
        std::vector<char>* buf = new std::vector(
            res.first.content, res.first.content + res.first.content_length);
        fi->fh = reinterpret_cast<uint64_t>(buf);
        fuse_reply_open(req, fi);
      } else {
        process_error_code("open", res.second, req);
      }
    }
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void networkfs_release(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info* fi) {
  if (fi) {
    (void)ino;
    delete (reinterpret_cast<std::vector<char>*>(fi->fh));
  }
  fuse_reply_err(req, 0);
}

void networkfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                    struct fuse_file_info* fi) {
  (void)ino;
  if (fi) {
    fuse_reply_buf(req,
                   (reinterpret_cast<std::vector<char>*>(fi->fh)->data()) + off,
                   size);
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void networkfs_write(fuse_req_t req, fuse_ino_t ino, const char* buffer,
                     size_t size, off_t off, struct fuse_file_info* fi) {
  if (fi) {
    if (off + size > MAX_FILE_SIZE) {
      fuse_reply_err(req, EFBIG);
    } else {
      std::vector<char>* fbuf = reinterpret_cast<std::vector<char>*>(fi->fh);
      if (fi->flags & O_APPEND) {
        std::size_t old_size = fbuf->size();
        fbuf->resize(fbuf->size() + size);
        memcpy(fbuf->data() + old_size, buffer, size);
      } else {
        if (off + size > fbuf->size()) {
          fbuf->resize(off + size);
          memcpy(fbuf->data() + off, buffer, size);
        } else {
          memcpy(fbuf->data() + off, buffer, size);
        }
      }
      fuse_reply_write(req, size);
    }
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void networkfs_sync_impl(fuse_req_t& req, fuse_ino_t ino,
                         struct fuse_file_info* fi, const std::string& name) {
  std::vector<char>* fbuf = reinterpret_cast<std::vector<char>*>(fi->fh);
  uint64_t res = 0;
  if (fbuf->data()) {
    const char* tok = getenv("NETWORKFS_TOKEN");
    std::vector<char> c_style_msg(fbuf->size() + 1);
    memcpy(c_style_msg.data(), fbuf->data(), fbuf->size());
    c_style_msg.back() = '\0';
    std::array<std::pair<std::string, std::string>, 2> storage{
        {{"inode", ino_to_string(Returns{}, ino)},
         {"content", c_style_msg.data()}}};
    res = networkfs_http_call(tok, "write", nullptr, 0, storage);
  }
  process_error_code(name, res, req);
}

void networkfs_flush(fuse_req_t req, fuse_ino_t ino,
                     struct fuse_file_info* fi) {
  if (fi) {
    networkfs_sync_impl(req, ino, fi, "flush");
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void networkfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                     struct fuse_file_info* fi) {
  (void)datasync;  // no meta data
  if (fi) {
    networkfs_sync_impl(req, ino, fi, "fsync");
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void networkfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                       int to_set, struct fuse_file_info* fi) {
  if (fi) {
    if (to_set & FUSE_SET_ATTR_SIZE) {
      if (attr->st_size > MAX_FILE_SIZE) {
        fuse_reply_err(req, EFBIG);
        return;
      }
      struct stat st = defaulted_stat(ino, fi);
      st.st_size = attr->st_size;
      std::vector<char>* fbuf = reinterpret_cast<std::vector<char>*>(fi->fh);
      if (fbuf->size() != st.st_size) {
        std::size_t old_size = fbuf->size();
        fbuf->resize(st.st_size);
      }
      fuse_reply_attr(req, &st, 1.0);
    } else {
      // no time to do everything else... and no tests also :)
      fuse_reply_err(req, ENOSYS);
    }
  }
}

void networkfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                    const char* name) {
  const char* tok = getenv("NETWORKFS_TOKEN");
  std::array<std::pair<std::string, std::string>, 3> storage{
      {{"source", ino_to_string(Returns{}, ino)},
       {"parent", ino_to_string(Returns{}, newparent)},
       {"name", name}}};

  uint64_t res = networkfs_http_call(tok, "link", nullptr, 0, storage);
  if (res == 0) {
    fuse_entry_param ep = defaulted_entry_param(DT_REG, ino);
    fuse_reply_entry(req, &ep);
  } else {
    if (res == 2) {
      std::cerr << "link: source is not file\n";
    } else if (res == 3) {
      std::cerr << "link: parent is not a directory\n";
    } else {
      process_error_code("link", res, req);
    }
  }
}

void networkfs_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  (void)ino;
  (void)nlookup;
  fuse_reply_none(req);
}

const struct fuse_lowlevel_ops networkfs_oper = {
    .init = networkfs_init,
    .destroy = networkfs_destroy,
    .lookup = networkfs_lookup,
    .forget = networkfs_forget,
    .getattr = networkfs_getattr,
    .setattr = networkfs_setattr,
    .mkdir = networkfs_mkdir,
    .unlink = networkfs_unlink,
    .rmdir = networkfs_rmdir,
    .link = networkfs_link,
    .open = networkfs_open,
    .read = networkfs_read,
    .write = networkfs_write,
    .flush = networkfs_flush,
    .release = networkfs_release,
    .fsync = networkfs_fsync,
    .readdir = networkfs_iterate,
    .create = networkfs_create,
};