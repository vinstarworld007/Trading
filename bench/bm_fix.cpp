// FIX 4.4 codec costs on fixgw's path: encoding an ExecutionReport (with
// BodyLength and CheckSum) and framing plus parsing a NewOrderSingle.

#include <benchmark/benchmark.h>

#include <array>
#include <string>

#include "lab/fix/codec.hpp"

namespace {

namespace fix = lab::fix;
namespace tag = lab::fix::tag;

void BM_FixEncodeExecutionReport(benchmark::State& state) {
  std::array<char, 1024> buf;
  std::uint64_t seq = 1;
  for (auto _ : state) {
    fix::Writer w{buf.data(), buf.size()};
    w.begin(fix::kBeginString, fix::msg_type::kExecutionReport);
    w.add(tag::kSenderCompID, "SIMEX");
    w.add(tag::kTargetCompID, "FIXA");
    w.add_uint(tag::kMsgSeqNum, seq++);
    w.add(tag::kSendingTime, "20261022-04:21:06.123");
    w.add_uint(tag::kOrderID, 123456789);
    w.add(tag::kClOrdID, "FIXA-1234567");
    w.add_uint(tag::kExecID, seq);
    w.add(tag::kExecType, 'F');
    w.add(tag::kOrdStatus, '1');
    w.add(tag::kSymbol, "AAPL");
    w.add(tag::kSide, '1');
    w.add_uint(tag::kOrderQty, 500);
    w.add_price(tag::kPrice, 1'002'500);
    w.add(tag::kOrdType, '2');
    w.add(tag::kTimeInForce, '0');
    w.add_uint(tag::kLastQty, 100);
    w.add_price(tag::kLastPx, 1'002'500);
    w.add_uint(tag::kLeavesQty, 400);
    w.add_uint(tag::kCumQty, 100);
    w.add_price(tag::kAvgPx, 1'002'500);
    w.add(tag::kTransactTime, "20261022-04:21:06.123");
    benchmark::DoNotOptimize(w.finish());
  }
}
BENCHMARK(BM_FixEncodeExecutionReport);

void BM_FixFrameAndParseNewOrder(benchmark::State& state) {
  std::array<char, 512> buf;
  fix::Writer w{buf.data(), buf.size()};
  w.begin(fix::kBeginString, fix::msg_type::kNewOrderSingle);
  w.add(tag::kSenderCompID, "FIXA");
  w.add(tag::kTargetCompID, "SIMEX");
  w.add_uint(tag::kMsgSeqNum, 42);
  w.add(tag::kSendingTime, "20261022-04:21:06.123");
  w.add(tag::kClOrdID, "FIXA-1234567");
  w.add(tag::kSymbol, "AAPL");
  w.add(tag::kSide, '1');
  w.add(tag::kTransactTime, "20261022-04:21:06.123");
  w.add_uint(tag::kOrderQty, 500);
  w.add(tag::kOrdType, '2');
  w.add_price(tag::kPrice, 1'002'500);
  w.add(tag::kTimeInForce, '0');
  const std::string wire{w.finish()};
  fix::StreamParser in;
  fix::Message m;
  for (auto _ : state) {
    in.feed(wire);
    const auto msg = in.next();
    benchmark::DoNotOptimize(m.parse(*msg));
    benchmark::DoNotOptimize(fix::parse_price(*m.get(tag::kPrice)));
  }
}
BENCHMARK(BM_FixFrameAndParseNewOrder);

}  // namespace
