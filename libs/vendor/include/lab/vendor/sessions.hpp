#pragma once

// Server-side sessions of the three lab vendors, plugged into Server
// (server.hpp). Each is one client connection speaking one protocol.

#include <memory>
#include <string>

#include "lab/vendor/server.hpp"

namespace lab::vendor {

struct IcfConfig {
  std::uint16_t source_id = 1;  // ENUM_SRC_ID of the venue this vendor carries
  std::string session = "LABICF";
};
// ICE Consolidated Feed look-alike over SoupBinTCP (docs/specs/vendor-icf.md).
std::unique_ptr<Session> make_icf_session(const IcfConfig& cfg);

struct OmmConfig {
  std::string service = "LAB_RT";
  std::uint64_t ping_interval_ns = 20'000'000'000ULL;
};
// Refinitiv Real-Time WebSocket API, OMM JSON (docs/specs/vendor-omm-json.md).
std::unique_ptr<Session> make_omm_session(const OmmConfig& cfg);

struct BpipeConfig {};
// Bloomberg B-PIPE look-alike (docs/specs/vendor-bpipe.md).
std::unique_ptr<Session> make_bpipe_session(const BpipeConfig& cfg);

}  // namespace lab::vendor
