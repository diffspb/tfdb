// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

// Deterministic media fuzzer for the TFDB recovery and decode paths.
//
// It derives images from the committed conformance corpus by truncation and
// byte mutation, biased toward the structural regions where a decoder is most
// likely to trust a length or offset it should have checked, then opens each
// image lazily and strictly and drives scan, query, and inspection over it.
//
// A second stage fuzzes the compression codecs directly. It seeds from valid
// streams produced by the encoders themselves, so the mutator starts inside
// the grammar rather than from noise a decoder rejects on its first byte, and
// it drives every built-in decoder over every mutated stream. The oracle is
// that a decoder either fails or returns exactly the size the caller declared,
// and that an unmutated stream still round-trips.
//
// The oracle is deliberately narrow and mechanical: no image may crash, hang,
// trip a sanitizer, or produce a status outside the documented set. Deciding
// whether a given corrupted image *should* have been readable is the job of
// the shared corpus manifest, not of a random mutator.
//
// Every run is reproducible from (corpus, seed, iteration). A failing media
// image is written next to the corpus so it can be replayed with --image; a
// codec-stage finding is reproduced from the seed and iteration alone.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "tfdb/codec.hpp"
#include "tfdb/memory_storage.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"
#include "tfdb/version.hpp"

namespace {

struct Options {
  std::vector<std::string> corpora;
  std::string image;
  std::string artifact_dir;
  std::uint64_t iterations = 20000;
  std::uint64_t seed = 20260906;
  std::uint64_t report_every = 5000;
};

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) return std::vector<std::uint8_t>();
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!output) return false;
  if (!bytes.empty())
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

// Every status code the public API is documented to produce. A code outside
// this set means an unclassified failure escaped, which is itself a finding.
bool documented(tfdb::StatusCode code) {
  switch (code) {
    case tfdb::StatusCode::ok:
    case tfdb::StatusCode::invalid_argument:
    case tfdb::StatusCode::out_of_range:
    case tfdb::StatusCode::io_error:
    case tfdb::StatusCode::interrupted:
    case tfdb::StatusCode::no_space:
    case tfdb::StatusCode::corrupt:
    case tfdb::StatusCode::unsupported:
    case tfdb::StatusCode::not_found:
    case tfdb::StatusCode::busy:
    case tfdb::StatusCode::overwritten:
    case tfdb::StatusCode::generation_exhausted:
    case tfdb::StatusCode::closed:
    case tfdb::StatusCode::internal_error:
      return true;
  }
  return false;
}

struct Findings {
  std::string reason;
  bool failed() const { return !reason.empty(); }
};

void check(Findings* findings, const tfdb::Status& status, const char* where) {
  if (findings->failed()) return;
  if (!documented(status.code()))
    findings->reason = std::string("undocumented status code from ") + where;
}

// Drives one image through the read paths a diagnostic tool would use.
void exercise(const std::vector<std::uint8_t>& image, Findings* findings) {
  std::shared_ptr<tfdb::MemoryStorage> storage(
      new tfdb::MemoryStorage(image.size()));
  const tfdb::Status written = tfdb::write_exact(
      *storage, 0, tfdb::ByteView(image.data(), image.size()));
  if (!written.ok()) return;

  for (int strict = 0; strict != 2; ++strict) {
    tfdb::OpenOptions open;
    open.verify_payloads_on_open = strict != 0;
    std::unique_ptr<tfdb::RingStore> store;
    const tfdb::Status opened = tfdb::RingStore::open(storage, open, &store);
    check(findings, opened, "open");
    if (!opened.ok()) continue;

    tfdb::VolumeInfo info;
    check(findings, store->inspect(&info), "inspect");

    std::uint64_t blocks = 0;
    std::uint64_t raw_bytes = 0;
    tfdb::QueryOptions block_options;
    check(findings, store->scan_blocks(block_options,
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data) {
            ++blocks;
            raw_bytes += event.data.size();
          } else {
            check(findings, event.detail, "block gap");
          }
          return true;
        }), "scan_blocks");

    // Record decoding over both stored time domains plus the full range.
    const std::uint64_t domains[3] = {1, 2, 7};
    for (unsigned domain = 0; domain != 3; ++domain) {
      tfdb::RecordQuery query;
      query.time.begin_ns = -4611686018427387904ll;
      query.time.end_ns = 4611686018427387904ll;
      query.time.time_domain_id = domains[domain];
      tfdb::FramedRecordV1 profile;
      std::uint64_t records = 0;
      const tfdb::Status queried = tfdb::query_records(
          *store, query, profile, [&](const tfdb::RecordEvent& event) {
            if (event.kind == tfdb::BlockEventKind::data) ++records;
            else check(findings, event.detail, "record gap");
            return true;
          });
      check(findings, queried, "query_records");
    }

    // Reading through a store must never be able to leave the writer able to
    // claim health it does not have.
    const tfdb::WriterHealth health = store->health();
    if (!findings->failed() && health.writable)
      findings->reason = "read-only open reported a writable store";
  }
}

// Valid seed streams paired with the payload they came from.
struct CodecSeed {
  std::shared_ptr<const tfdb::CompressionCodec> codec;
  std::vector<std::uint8_t> raw;
  std::vector<std::uint8_t> stored;
};

std::vector<std::vector<std::uint8_t>> codec_payloads() {
  std::vector<std::vector<std::uint8_t>> payloads;
  // Sizes around the thirteen-byte threshold below which LZ4 cannot start a
  // match, plus shapes that saturate each length nibble.
  for (std::size_t size : {std::size_t(0), std::size_t(1), std::size_t(12),
                           std::size_t(13), std::size_t(20)})
    payloads.push_back(std::vector<std::uint8_t>(size, 0x61));
  std::vector<std::uint8_t> run(1500, 0x2a);
  payloads.push_back(run);
  std::vector<std::uint8_t> cycles(1500);
  for (std::size_t i = 0; i != cycles.size(); ++i)
    cycles[i] = static_cast<std::uint8_t>((i % 16) + (i / 64));
  payloads.push_back(cycles);
  std::vector<std::uint8_t> extended;
  for (unsigned i = 0; i != 40; ++i)
    extended.push_back(static_cast<std::uint8_t>(i * 7 + 1));
  for (unsigned i = 0; i != 1200; ++i)
    extended.push_back(static_cast<std::uint8_t>(i % 4));
  payloads.push_back(extended);
  std::vector<std::uint8_t> noise(1500);
  std::uint32_t state = 0x1234567u;
  for (std::uint8_t& byte : noise) {
    state = state * 1103515245u + 12345u;
    byte = static_cast<std::uint8_t>(state >> 16);
  }
  payloads.push_back(noise);
  return payloads;
}

std::vector<std::shared_ptr<const tfdb::CompressionCodec>> built_in_codecs() {
  std::vector<std::shared_ptr<const tfdb::CompressionCodec>> codecs;
  const tfdb::CompressionId ids[] = {tfdb::CompressionId::none,
                                     tfdb::CompressionId::packbits,
                                     tfdb::CompressionId::lz4_block};
  for (tfdb::CompressionId id : ids) codecs.push_back(tfdb::built_in_codec(id));
  return codecs;
}

std::vector<CodecSeed> build_codec_seeds(Findings* findings) {
  std::vector<CodecSeed> seeds;
  const std::vector<std::vector<std::uint8_t>> payloads = codec_payloads();
  for (const std::shared_ptr<const tfdb::CompressionCodec>& codec :
       built_in_codecs()) {
    for (const std::vector<std::uint8_t>& raw : payloads) {
      CodecSeed seed;
      seed.codec = codec;
      seed.raw = raw;
      const tfdb::Status status =
          codec->compress(tfdb::ByteView(raw), &seed.stored);
      check(findings, status, "seed compress");
      if (!status.ok()) return seeds;
      if (seed.stored.size() > codec->max_compressed_size(raw.size())) {
        findings->reason = "encoder exceeded its own max_compressed_size";
        return seeds;
      }
      seeds.push_back(seed);
    }
  }
  return seeds;
}

// Drives one mutated codec stream through every built-in decoder.
void exercise_codecs(const std::vector<CodecSeed>& seeds,
                     const std::vector<std::shared_ptr<
                         const tfdb::CompressionCodec>>& codecs,
                     std::mt19937_64* rng, Findings* findings) {
  const CodecSeed& seed = seeds[static_cast<std::size_t>((*rng)() %
                                                         seeds.size())];
  std::vector<std::uint8_t> stream = seed.stored;
  bool mutated = false;
  const int shape = static_cast<int>((*rng)() % 8);
  if (shape == 0 && !stream.empty()) {
    stream.resize(static_cast<std::size_t>(
        (*rng)() % (stream.size() + 1)));
    mutated = true;
  } else if (shape < 6 && !stream.empty()) {
    const int mutations = static_cast<int>(1 + (*rng)() % 4);
    for (int i = 0; i != mutations; ++i) {
      const std::size_t offset =
          static_cast<std::size_t>((*rng)() % stream.size());
      // Single-bit flips reach a length nibble; whole-byte writes reach an
      // offset or an extended-length run.
      if ((*rng)() % 2 == 0)
        stream[offset] ^= static_cast<std::uint8_t>(1u << ((*rng)() % 8));
      else
        stream[offset] = static_cast<std::uint8_t>((*rng)() % 256);
    }
    mutated = true;
  } else if (shape == 6) {
    stream.push_back(static_cast<std::uint8_t>((*rng)() % 256));
    mutated = true;
  }

  // Usually the size the block header would carry, sometimes a neighbouring
  // or unrelated one, which is what a corrupt header would supply.
  std::size_t expected = seed.raw.size();
  const int size_shape = static_cast<int>((*rng)() % 8);
  if (size_shape == 6) {
    expected = static_cast<std::size_t>((*rng)() % 4096);
  } else if (size_shape == 7) {
    const std::size_t delta = static_cast<std::size_t>((*rng)() % 3);
    expected = expected + 1 >= delta ? expected + 1 - delta : 0;
  }

  for (const std::shared_ptr<const tfdb::CompressionCodec>& codec : codecs) {
    std::vector<std::uint8_t> output;
    const tfdb::Status status =
        codec->decompress(tfdb::ByteView(stream), expected, &output);
    check(findings, status, "codec decompress");
    if (findings->failed()) return;
    if (!status.ok()) continue;
    if (output.size() != expected) {
      findings->reason = "decoder succeeded with a size the caller never asked for";
      return;
    }
    // An untouched stream must still decode to exactly what was encoded.
    if (!mutated && expected == seed.raw.size() && codec == seed.codec &&
        output != seed.raw) {
      findings->reason = "encoder and decoder disagree on an unmutated stream";
      return;
    }
  }
}

std::size_t pick_offset(std::mt19937_64* rng, std::size_t size) {
  // Bias toward the structural prefix: the two volume header copies, the
  // partition header regions, and the first block frames. Uniform mutation
  // alone rarely reaches a length or offset field.
  std::uniform_int_distribution<int> mode(0, 3);
  std::uniform_int_distribution<std::size_t> anywhere(0, size - 1);
  switch (mode(*rng)) {
    case 0: {
      const std::size_t limit = size < 8192 ? size : 8192;
      return std::uniform_int_distribution<std::size_t>(0, limit - 1)(*rng);
    }
    case 1: {
      const std::size_t limit = size < 16384 ? size : 16384;
      return std::uniform_int_distribution<std::size_t>(0, limit - 1)(*rng);
    }
    default:
      return anywhere(*rng);
  }
}

std::vector<std::uint8_t> derive(const std::vector<std::uint8_t>& corpus,
                                 std::mt19937_64* rng) {
  std::vector<std::uint8_t> image = corpus;
  std::uniform_int_distribution<int> shape(0, 9);
  if (shape(*rng) == 0) {
    std::uniform_int_distribution<std::size_t> cut(0, corpus.size());
    image.resize(cut(*rng));
  }
  if (image.empty()) return image;

  std::uniform_int_distribution<int> count(1, 24);
  std::uniform_int_distribution<int> byte_value(0, 255);
  std::uniform_int_distribution<int> style(0, 2);
  const int mutations = count(*rng);
  for (int i = 0; i != mutations; ++i) {
    const std::size_t offset = pick_offset(rng, image.size());
    switch (style(*rng)) {
      case 0:  // Single bit flip: the cheapest way past a CRC-free field.
        image[offset] ^= static_cast<std::uint8_t>(
            1u << std::uniform_int_distribution<int>(0, 7)(*rng));
        break;
      case 1:
        image[offset] = static_cast<std::uint8_t>(byte_value(*rng));
        break;
      default: {  // Splat a run, modelling a torn or reused sector.
        const std::size_t run = std::min<std::size_t>(
            image.size() - offset,
            std::uniform_int_distribution<std::size_t>(1, 64)(*rng));
        const std::uint8_t value = static_cast<std::uint8_t>(byte_value(*rng));
        for (std::size_t j = 0; j != run; ++j) image[offset + j] = value;
        break;
      }
    }
  }
  return image;
}

void usage() {
  std::fprintf(stderr,
               "usage: tfdb_fuzz_media --corpus PATH [--corpus PATH ...] "
               "[--iterations N] "
               "[--seed N] [--artifacts DIR] [--report-every N]\n"
               "       tfdb_fuzz_media --image PATH\n");
}

bool parse_u64(const char* text, std::uint64_t* output) {
  if (!text) return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (errno != 0 || !end || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // Redirected stdout is fully buffered by default, so a sharded or CI run
  // shows nothing at all until a shard's buffer fills or it exits. Progress
  // reporting is the point of a long run; make it line-buffered instead.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--corpus" && has_value)
      options.corpora.push_back(argv[++i]);
    else if (argument == "--image" && has_value) options.image = argv[++i];
    else if (argument == "--artifacts" && has_value) options.artifact_dir = argv[++i];
    else if (argument == "--iterations" && has_value) {
      if (!parse_u64(argv[++i], &options.iterations)) { usage(); return 2; }
    } else if (argument == "--seed" && has_value) {
      if (!parse_u64(argv[++i], &options.seed)) { usage(); return 2; }
    } else if (argument == "--report-every" && has_value) {
      if (!parse_u64(argv[++i], &options.report_every)) { usage(); return 2; }
    } else { usage(); return 2; }
  }

  if (!options.image.empty()) {
    const std::vector<std::uint8_t> image = read_file(options.image);
    if (image.empty()) {
      std::fprintf(stderr, "cannot read image %s\n", options.image.c_str());
      return 2;
    }
    Findings findings;
    exercise(image, &findings);
    if (findings.failed()) {
      std::fprintf(stderr, "replay FAILED: %s\n", findings.reason.c_str());
      return 1;
    }
    std::printf("replay ok: %s\n", options.image.c_str());
    return 0;
  }

  if (options.corpora.empty() || options.iterations == 0) { usage(); return 2; }
  std::vector<std::vector<std::uint8_t>> corpora;
  for (const std::string& path : options.corpora) {
    const std::vector<std::uint8_t> image = read_file(path);
    if (image.empty()) {
      std::fprintf(stderr, "cannot read corpus %s\n", path.c_str());
      return 2;
    }
    corpora.push_back(image);
  }
  if (options.artifact_dir.empty()) options.artifact_dir = ".";

  for (std::size_t i = 0; i != corpora.size(); ++i) {
    std::printf("tfdb_fuzz_media %s corpus=%s bytes=%zu iterations=%llu "
                "seed=%llu\n",
                TFDB_VERSION_STRING, options.corpora[i].c_str(),
                corpora[i].size(),
                static_cast<unsigned long long>(options.iterations),
                static_cast<unsigned long long>(options.seed));
  }

  // The valid corpus and the codec seeds must always pass, so a broken harness
  // fails immediately rather than after a long run of mutated images.
  Findings baseline;
  for (const std::vector<std::uint8_t>& image : corpora)
    exercise(image, &baseline);
  const std::vector<std::shared_ptr<const tfdb::CompressionCodec>> codecs =
      built_in_codecs();
  const std::vector<CodecSeed> seeds = build_codec_seeds(&baseline);
  if (baseline.failed()) {
    std::fprintf(stderr, "FAILED on the unmodified corpus: %s\n",
                 baseline.reason.c_str());
    return 1;
  }

  std::mt19937_64 rng(options.seed);
  for (std::uint64_t iteration = 0; iteration != options.iterations;
       ++iteration) {
    // Reseeding per iteration keeps any single image reproducible from
    // (seed, iteration) without replaying everything before it.
    std::mt19937_64 local(rng());
    Findings findings;
    exercise_codecs(seeds, codecs, &local, &findings);
    // A codec finding is reproduced from (seed, iteration) alone; there is no
    // media image to save, and pointing at one would send the reader to the
    // wrong stage.
    const bool from_codec_stage = findings.failed();
    const std::vector<std::uint8_t>& corpus =
        corpora[static_cast<std::size_t>(local() % corpora.size())];
    const std::vector<std::uint8_t> image = derive(corpus, &local);
    if (!from_codec_stage && !image.empty()) exercise(image, &findings);
    if (findings.failed()) {
      std::fprintf(stderr, "FAILED seed=%llu iteration=%llu: %s\n",
                   static_cast<unsigned long long>(options.seed),
                   static_cast<unsigned long long>(iteration),
                   findings.reason.c_str());
      if (from_codec_stage) {
        std::fprintf(stderr,
                     "codec stage; rerun this shard with --seed %llu\n",
                     static_cast<unsigned long long>(options.seed));
        return 1;
      }
      char name[256];
      std::snprintf(name, sizeof name, "%s/tfdb-fuzz-%llu-%llu.tfdb",
                    options.artifact_dir.c_str(),
                    static_cast<unsigned long long>(options.seed),
                    static_cast<unsigned long long>(iteration));
      const bool saved = write_file(name, image);
      std::fprintf(stderr, "image %s%s\nreplay with: tfdb_fuzz_media --image %s\n",
                   name, saved ? " saved" : " COULD NOT BE SAVED", name);
      return 1;
    }
    if (options.report_every != 0 &&
        (iteration + 1) % options.report_every == 0) {
      std::printf("iteration %llu ok\n",
                  static_cast<unsigned long long>(iteration + 1));
    }
  }

  std::printf("tfdb_fuzz_media: %llu iterations, no crash, hang, sanitizer "
              "report, or undocumented status\n",
              static_cast<unsigned long long>(options.iterations));
  return 0;
}
