// Deterministic media fuzzer for the TFDB recovery and decode paths.
//
// It derives images from the committed conformance corpus by truncation and
// byte mutation, biased toward the structural regions where a decoder is most
// likely to trust a length or offset it should have checked, then opens each
// image lazily and strictly and drives scan, query, and inspection over it.
//
// The oracle is deliberately narrow and mechanical: no image may crash, hang,
// trip a sanitizer, or produce a status outside the documented set. Deciding
// whether a given corrupted image *should* have been readable is the job of
// the shared corpus manifest, not of a random mutator.
//
// Every run is reproducible from (corpus, seed, iteration). A failing image is
// written next to the corpus so it can be replayed with --image.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "tfdb/memory_storage.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"
#include "tfdb/version.hpp"

namespace {

struct Options {
  std::string corpus;
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
               "usage: tfdb_fuzz_media --corpus PATH [--iterations N] "
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
    if (argument == "--corpus" && has_value) options.corpus = argv[++i];
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

  if (options.corpus.empty() || options.iterations == 0) { usage(); return 2; }
  const std::vector<std::uint8_t> corpus = read_file(options.corpus);
  if (corpus.empty()) {
    std::fprintf(stderr, "cannot read corpus %s\n", options.corpus.c_str());
    return 2;
  }
  if (options.artifact_dir.empty()) options.artifact_dir = ".";

  std::printf("tfdb_fuzz_media %s corpus=%s bytes=%zu iterations=%llu "
              "seed=%llu\n",
              TFDB_VERSION_STRING, options.corpus.c_str(), corpus.size(),
              static_cast<unsigned long long>(options.iterations),
              static_cast<unsigned long long>(options.seed));

  // The valid corpus itself must always pass, so a broken harness fails
  // immediately rather than after a long run of mutated images.
  Findings baseline;
  exercise(corpus, &baseline);
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
    const std::vector<std::uint8_t> image = derive(corpus, &local);
    if (image.empty()) continue;
    Findings findings;
    exercise(image, &findings);
    if (findings.failed()) {
      char name[256];
      std::snprintf(name, sizeof name, "%s/tfdb-fuzz-%llu-%llu.tfdb",
                    options.artifact_dir.c_str(),
                    static_cast<unsigned long long>(options.seed),
                    static_cast<unsigned long long>(iteration));
      const bool saved = write_file(name, image);
      std::fprintf(stderr,
                   "FAILED seed=%llu iteration=%llu: %s\n"
                   "image %s%s\n"
                   "replay with: tfdb_fuzz_media --image %s\n",
                   static_cast<unsigned long long>(options.seed),
                   static_cast<unsigned long long>(iteration),
                   findings.reason.c_str(), name,
                   saved ? " saved" : " COULD NOT BE SAVED", name);
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
