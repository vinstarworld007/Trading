#pragma once

// Limit Up-Limit Down price bands, shared by simex (which enforces them) and
// by anyone rebuilding them from the feed (the vendors), so both get the same
// numbers from the same ITCH messages.
//
// The percentages are those of the LULD Plan for Tier 1 securities: 5% for a
// reference price above $3.00, 20% from $0.75 to $3.00, and the lesser of
// $0.15 or 75% below $0.75. The lab simplifies the rest of the plan:
//   - the reference price is the latest cross that printed shares (opening,
//     halt, IPO or pause reopening; ITCH Q), else the day's first trade,
//     and after a reopening cross that printed nothing the book's midpoint
//     rounded down to the cent, instead of a five-minute average published
//     by the SIP;
//   - bands are not doubled near the open and close;
//   - simex can be told to use a narrower percentage (--luld-band-pct) so a
//     short simulated day reaches its bands.
// Prices are Price(4); bands are rounded to the cent, never past the reference.

#include <cstdint>

namespace lab::book {

struct Band {
  std::uint32_t lower = 0;
  std::uint32_t upper = 0;
};

// Band width in Price(4) for `reference`; `pct_override` (whole percent,
// 0 for the plan's own) replaces the percentage for references above $3.00.
inline std::uint32_t luld_width(std::uint32_t reference, std::uint32_t pct_override = 0) noexcept {
  const std::uint64_t ref = reference;
  if (reference > 30'000)
    return static_cast<std::uint32_t>(ref * (pct_override ? pct_override : 5) / 100);
  if (reference >= 7'500) return static_cast<std::uint32_t>(ref * 20 / 100);
  const std::uint64_t pct = ref * 75 / 100;
  return static_cast<std::uint32_t>(pct < 1'500 ? pct : 1'500);
}

inline Band luld_band(std::uint32_t reference, std::uint32_t pct_override = 0) noexcept {
  if (reference == 0) return {};
  const std::uint32_t w = luld_width(reference, pct_override) / 100 * 100;  // whole cents
  Band b;
  b.upper = reference + w;
  b.lower = reference > w ? reference - w : 100;  // never below one cent
  return b;
}

}  // namespace lab::book
