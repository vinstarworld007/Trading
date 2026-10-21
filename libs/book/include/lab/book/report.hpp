#pragma once

// Plain-text reports shared by the tools that rebuild books (itch-dump from a
// file, itch-fh from a live feed), so both print the same checks the same way.

#include <cstdint>
#include <cstdio>
#include <string>

#include "lab/book/book_builder.hpp"

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

}  // namespace lab::book
