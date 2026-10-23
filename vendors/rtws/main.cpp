// rt-server: the lab's Reuters-style vendor. Consolidates a venue's ITCH feed
// and serves it through the Refinitiv Real-Time WebSocket API: OMM JSON on
// subprotocol tr_json2 at ws://HOST:PORT/WebSocket (docs/specs/vendor-omm-json.md).
//
//   rt-server --md 239.1.1.1:30001 --retrans 127.0.0.1:30002 --listen 127.0.0.1:17002

#include <string>

#include "lab/vendor/cli.hpp"
#include "lab/vendor/sessions.hpp"

int main(int argc, char** argv) {
  lab::vendor::ServerConfig defaults;
  defaults.name = "rt-server";
  defaults.listen = "127.0.0.1:17002";
  lab::vendor::OmmConfig omm;
  return lab::vendor::run_vendor(
      argc, argv, defaults, " [--service NAME]",
      [&](std::string_view a, const std::string& v) {
        if (a != "--service") return false;
        omm.service = v;
        return true;
      },
      [&] { return [omm] { return lab::vendor::make_omm_session(omm); }; });
}
