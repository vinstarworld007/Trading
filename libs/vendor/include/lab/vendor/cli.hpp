#pragma once

// Shared command line for the three vendor servers:
//
//   VENDOR [--listen IP:PORT] [--md GROUP:PORT] [--iface IP] [--retrans IP:PORT]
//          [--duration S] [protocol options]
//
// Joins the venue's MoldUDP64 feed, serves clients until the feed's End of
// Session (or --duration, or Ctrl-C), prints a report and exits 0 if the
// venue's books had no integrity errors.

#include <functional>
#include <string>
#include <string_view>

#include "lab/vendor/server.hpp"

namespace lab::vendor {

struct Cli {
  ServerConfig server;
  double duration_s = 0;
};

// Parses the shared options; `extra(name, value)` gets the rest and returns
// false for an unknown option. `defaults` sets the listen port and name.
int run_vendor(int argc, char** argv, ServerConfig defaults, std::string_view usage_extra,
               const std::function<bool(std::string_view, const std::string&)>& extra,
               const std::function<Server::Factory()>& factory);

}  // namespace lab::vendor
