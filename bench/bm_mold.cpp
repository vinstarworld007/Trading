#include <benchmark/benchmark.h>

#include <vector>

#include "lab/mold/mold.hpp"
#include "lab/mold/retransmit_store.hpp"
#include "lab/mold/sequencer.hpp"

namespace {

using namespace lab::mold;

const Session kSession = make_session("BENCH");

// 64 packets of 40 ITCH-sized (30 byte) messages each, back to back.
std::vector<std::vector<std::byte>> make_packets(std::uint64_t first) {
  std::vector<std::vector<std::byte>> out;
  const std::vector<std::byte> msg(30, std::byte{0x41});
  PacketBuilder<> b;
  for (int p = 0; p < 64; ++p) {
    b.begin(kSession, first);
    for (int i = 0; i < 40; ++i) b.add(msg);
    first += 40;
    const auto pkt = b.finish();
    out.emplace_back(pkt.begin(), pkt.end());
  }
  return out;
}

void BM_PacketBuilderAdd(benchmark::State& state) {
  const std::vector<std::byte> msg(30, std::byte{0x41});
  PacketBuilder<> b;
  std::uint64_t seq = 1;
  for (auto _ : state) {
    if (!b.add(msg)) {
      benchmark::DoNotOptimize(b.finish().data());
      seq = b.next_sequence();
      b.begin(kSession, seq);
      b.add(msg);
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PacketBuilderAdd);

// In-order traffic: parse, sequence check and delivery, per message.
void BM_SequencerInOrder(benchmark::State& state) {
  SequencerConfig cfg;
  Sequencer seq{cfg};
  std::uint64_t first = 1;
  std::uint64_t bytes = 0;
  auto sink = [&bytes](std::uint64_t, std::span<const std::byte> m) { bytes += m.size(); };
  for (auto _ : state) {
    state.PauseTiming();
    const auto packets = make_packets(first);
    first += 64 * 40;
    state.ResumeTiming();
    for (const auto& p : packets) seq.on_packet(p.data(), p.size(), 0, sink);
  }
  benchmark::DoNotOptimize(bytes);
  state.SetItemsProcessed(state.iterations() * 64 * 40);
}
BENCHMARK(BM_SequencerInOrder);

// Every other packet arrives one place late: half the messages are held and
// delivered from the window.
void BM_SequencerReordered(benchmark::State& state) {
  SequencerConfig cfg;
  cfg.gap_delay = ~0ULL;  // never request
  Sequencer seq{cfg};
  std::uint64_t first = 1;
  std::uint64_t bytes = 0;
  auto sink = [&bytes](std::uint64_t, std::span<const std::byte> m) { bytes += m.size(); };
  for (auto _ : state) {
    state.PauseTiming();
    auto packets = make_packets(first);
    for (std::size_t i = 0; i + 1 < packets.size(); i += 2) std::swap(packets[i], packets[i + 1]);
    first += 64 * 40;
    state.ResumeTiming();
    for (const auto& p : packets) seq.on_packet(p.data(), p.size(), 0, sink);
  }
  benchmark::DoNotOptimize(bytes);
  state.SetItemsProcessed(state.iterations() * 64 * 40);
}
BENCHMARK(BM_SequencerReordered);

void BM_RetransmitStoreAppend(benchmark::State& state) {
  RetransmitStore store{64u << 20, 1u << 22};
  const std::vector<std::byte> msg(30, std::byte{0x41});
  for (auto _ : state) benchmark::DoNotOptimize(store.append(msg));
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RetransmitStoreAppend);

}  // namespace
