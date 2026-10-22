// icf-server: the lab's ICE Consolidated Feed look-alike. Consolidates a
// venue's ITCH feed into Level 1 records and serves them over SoupBinTCP with
// text commands and token messages (docs/specs/vendor-icf.md).
//
//   icf-server --md 239.1.1.1:30001 --retrans 127.0.0.1:30002 --listen 127.0.0.1:17001

#include <string>

#include "lab/vendor/cli.hpp"
#include "lab/vendor/sessions.hpp"

int main(int argc, char** argv) {
  lab::vendor::ServerConfig defaults;
  defaults.name = "icf-server";
  defaults.listen = "127.0.0.1:17001";
  lab::vendor::IcfConfig icf;
  return lab::vendor::run_vendor(
      argc, argv, defaults, " [--source-id N]",
      [&](std::string_view a, const std::string& v) {
        if (a != "--source-id") return false;
        icf.source_id = static_cast<std::uint16_t>(std::stoul(v));
        return true;
      },
      [&] { return [icf] { return lab::vendor::make_icf_session(icf); }; });
}
