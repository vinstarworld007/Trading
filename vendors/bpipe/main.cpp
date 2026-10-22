// bpipe-server: the lab's Bloomberg B-PIPE look-alike. Consolidates a venue's
// ITCH feed and serves //blp/mktdata subscriptions over the lab's BLPAPI-style
// event protocol (docs/specs/vendor-bpipe.md).
//
//   bpipe-server --md 239.1.1.1:30001 --retrans 127.0.0.1:30002 --listen 127.0.0.1:17003

#include <string>

#include "lab/vendor/cli.hpp"
#include "lab/vendor/sessions.hpp"

int main(int argc, char** argv) {
  lab::vendor::ServerConfig defaults;
  defaults.name = "bpipe-server";
  defaults.listen = "127.0.0.1:17003";
  lab::vendor::BpipeConfig bp;
  return lab::vendor::run_vendor(
      argc, argv, defaults, "", [](std::string_view, const std::string&) { return false; },
      [&] { return [bp] { return lab::vendor::make_bpipe_session(bp); }; });
}
