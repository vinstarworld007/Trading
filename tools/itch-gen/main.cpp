// itch-gen: writes a synthetic ITCH 5.0 day in Nasdaq's BinaryFILE format.
//
//   itch-gen synthetic.itch50.gz --symbols 200 --messages 5000000 --seed 7
//
// Useful for trying itch-dump and benchmarks before downloading a real day.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "lab/itch/file_reader.hpp"
#include "lab/itch/synthetic.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: itch-gen OUT[.gz] [--symbols N] [--messages N] [--seed S]\n");
    return 2;
  }
  const std::string out = argv[1];
  lab::itch::SyntheticConfig config;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const unsigned long long value = std::strtoull(argv[i + 1], nullptr, 10);
    if (flag == "--symbols")
      config.symbols = static_cast<std::uint32_t>(value);
    else if (flag == "--messages")
      config.book_messages = value;
    else if (flag == "--seed")
      config.seed = value;
    else {
      std::fprintf(stderr, "unknown option %s\n", flag.c_str());
      return 2;
    }
  }
  if (config.symbols == 0 || config.symbols > 65535) {
    std::fprintf(stderr, "--symbols must be 1 to 65535\n");
    return 2;
  }

  lab::itch::FileWriter writer{out};
  lab::itch::SyntheticDay day{config};
  day.generate([&](std::span<const std::byte> msg) { writer.write(msg); });
  if (!writer.close()) {
    std::fprintf(stderr, "itch-gen: %s\n", writer.error().c_str());
    return 1;
  }
  std::printf("wrote %llu messages for %u symbols to %s (%zu orders still live)\n",
              static_cast<unsigned long long>(day.messages_written()), config.symbols, out.c_str(),
              day.live_orders().size());
  return 0;
}
