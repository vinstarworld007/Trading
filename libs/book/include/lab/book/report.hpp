#pragma once

// Plain-text reports shared by the tools that rebuild books (itch-dump from a
// file, itch-fh from a live feed), so both print the same checks the same way.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "lab/book/book_builder.hpp"
#include "lab/book/implied.hpp"

namespace lab::book {

// "123.4500", or "-" for 0 (no price).
std::string format_price(std::uint32_t price);
// "09:30:00.000000123" from ITCH nanoseconds since midnight.
std::string format_feed_time(std::uint64_t ns_since_midnight);
// "add order", "order executed", ...; "unknown" for anything else.
const char* message_type_name(char type);

// Message counts by type, and the system events seen.
void print_message_types(std::FILE* out, const BookBuilder& b);
// One OK / ERROR / INFO line per integrity check.
void print_integrity(std::FILE* out, const IntegrityChecks& c);
// Top `depth` levels of one instrument's book.
void print_book(std::FILE* out, const BookBuilder& b, std::uint16_t locate, std::size_t depth);
// Top `n` instruments by matched volume.
void print_top(std::FILE* out, const BookBuilder& b, std::size_t n);
// Market scenarios seen: circuit breakers, and per instrument its crosses,
// halts and pauses, latest imbalance, LULD collar, IPO and operational
// halts. Prints nothing on a day without any.
void print_scenarios(std::FILE* out, const BookBuilder& b);
// Each strip's own top of book next to its implied-out depth from the legs,
// and each leg's implied-in quote from the strip and the other leg.
void print_implied(std::FILE* out, const BookBuilder& b, const std::vector<StripDef>& strips,
                   std::size_t depth);

}  // namespace lab::book
