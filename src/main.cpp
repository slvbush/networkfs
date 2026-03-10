#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)
#include <fuse_lowlevel.h>

#include "inode.h"

int main(int argc, char* argv[]) {
  fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_cmdline_opts opts;

  if (fuse_parse_cmdline(&args, &opts) != 0) {
    return 1;
  }

  if (opts.show_help) {
    std::cout << "usage: " << argv[0] << " [options] <mountpoint>\n\n";
    fuse_cmdline_help();
    fuse_lowlevel_help();
    return 0;
  }

  if (opts.show_version) {
    std::cout << "FUSE library version " << fuse_pkgversion() << "\n";
    fuse_lowlevel_version();
    return 0;
  }

  if (!opts.mountpoint) {
    std::cerr << "usage: " << argv[0] << " [options] <mountpoint>\n";
    std::cerr << "       " << argv[0] << " --help\n";
    return 1;
  }
  auto mountpoint =
      std::unique_ptr<char, decltype(&free)>(opts.mountpoint, &free);

  const char* token = getenv("NETWORKFS_TOKEN");
  if (!token) {
    std::cerr << "NETWORKFS_TOKEN environment variable not set\n";
    return 1;
  }

  auto se = std::unique_ptr<fuse_session, decltype(&fuse_session_destroy)>(
      fuse_session_new(&args, &networkfs_oper, sizeof(networkfs_oper),
                       strdup(token)),
      &fuse_session_destroy);

  if (!se) {
    return 1;
  }

  if (fuse_set_signal_handlers(se.get()) != 0) {
    return 1;
  }

  if (fuse_session_mount(se.get(), mountpoint.get()) != 0) {
    return 1;
  }

  fuse_daemonize(opts.foreground);

  int ret = fuse_session_loop(se.get());

  fuse_session_unmount(se.get());
  fuse_remove_signal_handlers(se.get());

  return ret ? 1 : 0;
}