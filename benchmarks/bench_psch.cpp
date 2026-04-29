// bench_psch.cpp — psch schema-format perf: encode, view-construct,
// field_by_name latency, plus size comparison against typical
// alternatives.
//
// Measures the schema layer in isolation. Payload encode/decode
// (pSSZ) is unchanged; this is just "how fast is the schema header
// + how big is it".

#include <psio/psch.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;

template <typename Fn>
double bench_ns(std::size_t iters, Fn&& fn)
{
   for (std::size_t w = 0; w < std::min<std::size_t>(iters / 10, 100); ++w)
      fn();
   auto t0 = clk::now();
   for (std::size_t i = 0; i < iters; ++i) fn();
   auto t1 = clk::now();
   double dt =
      std::chrono::duration<double, std::nano>(t1 - t0).count();
   return dt / iters;
}

// Build a small 4-field schema (the format_perf "Bag"-equivalent).
std::vector<std::uint8_t> small_schema()
{
   psio::psch::writer w;
   auto u32 = w.add_u32();
   auto u64 = w.add_u64();
   auto str = w.add_bytes();
   auto bln = w.add_bool();
   auto p = w.add_container({{"name", str}, {"id", u32},
                             {"active", bln}, {"version", u32},
                             {"seq", u64}});
   return w.finalize(p);
}

// Build a BeaconState-class schema (~30 types, ~100 fields).
// Approximated; the exact schema is large to spell out.
std::vector<std::uint8_t> beacon_class_schema()
{
   psio::psch::writer w;
   auto u8t  = w.add_u8();
   auto u32t = w.add_u32();
   auto u64t = w.add_u64();
   auto bln  = w.add_bool();
   auto h32  = w.add_bytes_n(32);
   auto h48  = w.add_bytes_n(48);
   auto h96  = w.add_bytes_n(96);
   auto sigb = w.add_bytes_n(96);
   (void)u8t;

   // Validator (8 fields)
   auto validator = w.add_container(
      {{"pubkey", h48},
       {"withdrawal_credentials", h32},
       {"effective_balance", u64t},
       {"slashed", bln},
       {"activation_eligibility_epoch", u64t},
       {"activation_epoch", u64t},
       {"exit_epoch", u64t},
       {"withdrawable_epoch", u64t}});

   // Checkpoint (2)
   auto checkpoint = w.add_container({{"epoch", u64t}, {"root", h32}});

   // AttestationData (5)
   auto attestation_data = w.add_container(
      {{"slot", u64t}, {"index", u64t},
       {"beacon_block_root", h32},
       {"source", checkpoint}, {"target", checkpoint}});

   // Fork (3)
   auto fork = w.add_container(
      {{"previous_version", w.add_bytes_n(4)},
       {"current_version", w.add_bytes_n(4)},
       {"epoch", u64t}});

   // BeaconBlockHeader (5)
   auto block_header = w.add_container(
      {{"slot", u64t}, {"proposer_index", u64t},
       {"parent_root", h32}, {"state_root", h32},
       {"body_root", h32}});

   // ExecutionPayloadHeader (~14)
   auto exec_header = w.add_container(
      {{"parent_hash", h32},
       {"fee_recipient", w.add_bytes_n(20)},
       {"state_root", h32}, {"receipts_root", h32},
       {"logs_bloom", w.add_bytes_n(256)},
       {"prev_randao", h32}, {"block_number", u64t},
       {"gas_limit", u64t}, {"gas_used", u64t},
       {"timestamp", u64t},
       {"extra_data", w.add_bytes()},
       {"base_fee_per_gas", w.add_bytes_n(32)},
       {"block_hash", h32}, {"transactions_root", h32}});

   // SignedBeaconBlockHeader (2)
   auto signed_block_header = w.add_container(
      {{"message", block_header}, {"signature", sigb}});

   // List<Validator>
   auto validators_list = w.add_list(validator);

   // BeaconState root (~20 fields, abbreviated to keep this compact)
   auto root = w.add_container(
      {{"genesis_time", u64t},
       {"genesis_validators_root", h32},
       {"slot", u64t},
       {"fork", fork},
       {"latest_block_header", block_header},
       {"block_roots", w.add_vector(h32, 8192)},
       {"state_roots", w.add_vector(h32, 8192)},
       {"historical_roots", w.add_list(h32)},
       {"eth1_deposit_index", u64t},
       {"validators", validators_list},
       {"balances", w.add_list(u64t)},
       {"randao_mixes", w.add_vector(h32, 65536)},
       {"slashings", w.add_vector(u64t, 8192)},
       {"justification_bits", w.add_bytes_n(1)},
       {"previous_justified_checkpoint", checkpoint},
       {"current_justified_checkpoint", checkpoint},
       {"finalized_checkpoint", checkpoint},
       {"latest_execution_payload_header", exec_header},
       {"signed_block_header_history", w.add_list(signed_block_header)},
       {"attestation_data_history", w.add_list(attestation_data)}});
   (void)h96;
   return w.finalize(root);
}

}  // namespace

int main()
{
   using psio::psch::view;

   auto small  = small_schema();
   auto beacon = beacon_class_schema();

   std::printf("\n── psch wire size ───────────────────────────────\n");
   std::printf("  small (4-field container):  %5zu bytes\n",
               small.size());
   std::printf("  beacon-class (~30 types):   %5zu bytes\n",
               beacon.size());

   std::printf("\n── psch encode latency (build + finalize) ─────\n");
   {
      auto ns = bench_ns(1000, [] { auto b = small_schema(); (void)b; });
      std::printf("  small  encode:  %7.0f ns/op  (%zu bytes)\n",
                  ns, small.size());
   }
   {
      auto ns = bench_ns(200, [] { auto b = beacon_class_schema(); (void)b; });
      std::printf("  beacon encode: %7.0f ns/op  (%zu bytes)\n",
                  ns, beacon.size());
   }

   std::printf("\n── psch view construction (parse header) ──────\n");
   {
      auto ns = bench_ns(100000, [&] {
         view v(small.data(), small.size());
         (void)v;
      });
      std::printf("  small  view:   %7.1f ns/op\n", ns);
   }
   {
      auto ns = bench_ns(100000, [&] {
         view v(beacon.data(), beacon.size());
         (void)v;
      });
      std::printf("  beacon view:   %7.1f ns/op\n", ns);
   }

   std::printf("\n── psch field_by_name latency (PHF + verify) ───\n");
   {
      view v(small.data(), small.size());
      auto root = v.root_type_id();
      // Hit each of the 5 fields in turn.
      const char* names[] = {"name", "id", "active", "version", "seq"};
      auto ns = bench_ns(1000000, [&] {
         for (auto* n : names) {
            auto r = v.field_by_name(root, n);
            (void)r;
         }
      });
      std::printf("  small  5 lookups: %7.1f ns  (%.1f ns/access)\n",
                  ns, ns / 5.0);
   }
   {
      view v(beacon.data(), beacon.size());
      auto root = v.root_type_id();
      const char* names[] = {"genesis_time", "validators",
                             "finalized_checkpoint",
                             "latest_execution_payload_header"};
      auto ns = bench_ns(500000, [&] {
         for (auto* n : names) {
            auto r = v.field_by_name(root, n);
            (void)r;
         }
      });
      std::printf("  beacon 4 lookups: %7.1f ns  (%.1f ns/access)\n",
                  ns, ns / 4.0);
   }

   std::printf("\n── psch type_kind by id (indexed load) ────────\n");
   {
      view v(beacon.data(), beacon.size());
      std::size_t N = v.type_count();
      auto ns = bench_ns(2000000, [&] {
         for (std::size_t i = 0; i < N; ++i) {
            auto k = v.type_kind(static_cast<std::uint16_t>(i));
            (void)k;
         }
      });
      std::printf("  beacon %zu type_kind reads: %5.0f ns  (%.2f ns/each)\n",
                  N, ns, ns / static_cast<double>(N));
   }

   return 0;
}
