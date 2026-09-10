#include "partial_evidence.h"

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

using partial_evidence::Candidate;
using partial_evidence::Config;
using partial_evidence::Energy;
using partial_evidence::Observation;
using partial_evidence::PartialEvidence;
using partial_evidence::Snapshot;
using partial_evidence::Word;

static std::string failure;

static void Check(bool ok, const char *name)
{
    if (!ok && failure.empty()) {
        failure = std::string("audio support invariant failed: ") + name;
    }
}

struct Interval {
    int64_t a;
    int64_t b;
};

static const Interval kQueries[] = {
    {0, 160}, {160, 320}, {320, 800}, {800, 960}, {0, 1600}, {700, 900}};

static Config MakeConfig(double quiet_dbfs)
{
    Config config;
    config.quiet_dbfs_set = true;
    config.quiet_dbfs = quiet_dbfs;
    config.sample_rate_hz = 16000.0;
    config.profile_id = "audio-support-test";
    return config;
}

static PartialEvidence MakeEvidence(double quiet_dbfs, uint64_t epoch)
{
    PartialEvidence evidence;
    evidence.Configure(MakeConfig(quiet_dbfs));
    evidence.BeginEpoch(epoch);
    return evidence;
}

static std::vector<float> LoudThenQuiet(std::size_t loud, std::size_t total, float amp)
{
    std::vector<float> signal(total, 0.0f);
    for (std::size_t i = 0; i < loud && i < total; i++) {
        signal[i] = amp;
    }
    return signal;
}

static void FeedFloat(PartialEvidence *evidence, const std::vector<float> &signal,
                      std::size_t block)
{
    if (block == 0) {
        block = signal.size();
    }
    for (std::size_t at = 0; at < signal.size(); at += block) {
        std::size_t remaining = signal.size() - at;
        std::size_t n = remaining < block ? remaining : block;
        evidence->AcceptPcm(signal.data() + at, n, static_cast<int64_t>(at));
    }
}

static void FeedInt16(PartialEvidence *evidence, const std::vector<int16_t> &signal,
                      std::size_t block)
{
    if (block == 0) {
        block = signal.size();
    }
    for (std::size_t at = 0; at < signal.size(); at += block) {
        std::size_t remaining = signal.size() - at;
        std::size_t n = remaining < block ? remaining : block;
        evidence->AcceptPcm(signal.data() + at, n, static_cast<int64_t>(at));
    }
}

// [[rr:FVP-7]]
static void BasicSupport()
{
    PartialEvidence evidence = MakeEvidence(-40.0, 1);
    std::vector<float> signal = LoudThenQuiet(800, 1600, 5000.0f);
    FeedFloat(&evidence, signal, 640);

    Check(evidence.IntervalEnergy(0, 160) == Energy::kNonquiet,
          "observed loud audio reads nonquiet");
    Check(evidence.IntervalEnergy(800, 960) == Energy::kQuiet,
          "observed digital-zero audio reads quiet");
    Check(evidence.IntervalEnergy(1600, 1760) == Energy::kUnknown,
          "an interval beyond the accepted frontier is unknown");
    Check(evidence.IntervalEnergy(700, 900) == Energy::kNonquiet,
          "an interval spanning any loud frame is nonquiet");
    Check(evidence.IntervalEnergy(0, 0) == Energy::kUnknown,
          "an empty interval is unknown");
}

// [[rr:FVP-7]]
static void PartitionInvariance()
{
    std::vector<float> signal = LoudThenQuiet(800, 1600, 5000.0f);
    PartialEvidence reference = MakeEvidence(-40.0, 1);
    FeedFloat(&reference, signal, 0);

    const std::size_t blocks[] = {640, 800};
    for (std::size_t b = 0; b < 2; b++) {
        PartialEvidence evidence = MakeEvidence(-40.0, 1);
        FeedFloat(&evidence, signal, blocks[b]);
        for (std::size_t q = 0; q < sizeof(kQueries) / sizeof(kQueries[0]); q++) {
            Check(evidence.IntervalEnergy(kQueries[q].a, kQueries[q].b) ==
                      reference.IntervalEnergy(kQueries[q].a, kQueries[q].b),
                  "block partition does not change interval energy");
        }
    }

    PartialEvidence irregular = MakeEvidence(-40.0, 1);
    const std::size_t chunks[] = {100, 250, 290, 7, 640, 313};
    std::size_t at = 0;
    for (std::size_t c = 0; c < 6 && at < signal.size(); c++) {
        std::size_t remaining = signal.size() - at;
        std::size_t n = remaining < chunks[c] ? remaining : chunks[c];
        irregular.AcceptPcm(signal.data() + at, n, static_cast<int64_t>(at));
        at += n;
    }
    if (at < signal.size()) {
        irregular.AcceptPcm(signal.data() + at, signal.size() - at,
                            static_cast<int64_t>(at));
    }
    for (std::size_t q = 0; q < sizeof(kQueries) / sizeof(kQueries[0]); q++) {
        Check(irregular.IntervalEnergy(kQueries[q].a, kQueries[q].b) ==
                  reference.IntervalEnergy(kQueries[q].a, kQueries[q].b),
              "an irregular split does not change interval energy");
    }
}

// [[rr:FVP-7]]
static void ShortLastBlock()
{
    PartialEvidence evidence = MakeEvidence(-40.0, 1);
    std::vector<float> signal(1500, 5000.0f);
    FeedFloat(&evidence, signal, 640);

    Check(evidence.IntervalEnergy(1440, 1600) == Energy::kUnknown,
          "an interval touching the short last frame is unknown");
    Check(evidence.IntervalEnergy(1280, 1440) == Energy::kNonquiet,
          "a fully observed frame before the short tail stays classified");
}

// [[rr:FVP-7]]
static void MissingRange()
{
    PartialEvidence evidence = MakeEvidence(-40.0, 1);
    std::vector<float> before(320, 5000.0f);
    std::vector<float> after(320, 5000.0f);
    evidence.AcceptPcm(before.data(), 320, 0);
    evidence.AcceptPcm(after.data(), 320, 640);

    Check(evidence.IntervalEnergy(0, 160) == Energy::kNonquiet,
          "audio before a gap is classified");
    Check(evidence.IntervalEnergy(320, 480) == Energy::kUnknown,
          "an interval inside a missing range is unknown");
    Check(evidence.IntervalEnergy(640, 800) == Energy::kNonquiet,
          "audio after a gap is classified");
    Check(evidence.IntervalEnergy(160, 700) == Energy::kUnknown,
          "an interval spanning a missing range is unknown");
}

// [[rr:FVP-7]]
static void OverloadIdentity()
{
    std::vector<float> as_float = LoudThenQuiet(800, 1600, 5000.0f);
    std::vector<int16_t> as_int16(as_float.size());
    for (std::size_t i = 0; i < as_float.size(); i++) {
        as_int16[i] = static_cast<int16_t>(as_float[i]);
    }

    PartialEvidence from_float = MakeEvidence(-40.0, 1);
    FeedFloat(&from_float, as_float, 640);
    PartialEvidence from_int16 = MakeEvidence(-40.0, 1);
    FeedInt16(&from_int16, as_int16, 800);

    for (std::size_t q = 0; q < sizeof(kQueries) / sizeof(kQueries[0]); q++) {
        Check(from_float.IntervalEnergy(kQueries[q].a, kQueries[q].b) ==
                  from_int16.IntervalEnergy(kQueries[q].a, kQueries[q].b),
              "float and int16 overloads agree on interval energy");
    }
}

// [[rr:FVP-7]]
static void CleanupReleasesPriorSummaries()
{
    PartialEvidence evidence = MakeEvidence(-40.0, 1);
    std::vector<float> signal(800, 5000.0f);
    FeedFloat(&evidence, signal, 640);

    Check(evidence.IntervalEnergy(0, 160) == Energy::kNonquiet,
          "before cleanup the summary classifies");
    evidence.BeginEpoch(2);
    Check(evidence.IntervalEnergy(0, 160) == Energy::kUnknown,
          "cleanup releases prior summaries to unknown");
}

// [[rr:FVP-7]]
static void TwoRecognizersAreIndependent()
{
    PartialEvidence loud = MakeEvidence(-40.0, 1);
    PartialEvidence quiet = MakeEvidence(-40.0, 1);
    std::vector<float> hot(320, 6000.0f);
    std::vector<float> cold(320, 0.0f);
    loud.AcceptPcm(hot.data(), 320, 0);
    quiet.AcceptPcm(cold.data(), 320, 0);

    Check(loud.IntervalEnergy(0, 160) == Energy::kNonquiet,
          "the loud recognizer reads its own audio nonquiet");
    Check(quiet.IntervalEnergy(0, 160) == Energy::kQuiet,
          "a second recognizer does not see the first's audio");
}

// [[rr:FVP-7]]
static void EachCandidateWordCarriesItsOwnEnergy()
{
    PartialEvidence evidence = MakeEvidence(-40.0, 1);
    std::vector<float> signal = LoudThenQuiet(160, 320, 5000.0f);
    FeedFloat(&evidence, signal, 320);

    Observation observation;
    observation.epoch = 1;
    Candidate candidate;
    Word loud;
    loud.word = "loud";
    loud.start_sample_known = true;
    loud.start_sample = 0;
    loud.end_sample_known = true;
    loud.end_sample = 160;
    Word quiet;
    quiet.word = "quiet";
    quiet.start_sample_known = true;
    quiet.start_sample = 160;
    quiet.end_sample_known = true;
    quiet.end_sample = 320;
    candidate.words.push_back(loud);
    candidate.words.push_back(quiet);
    observation.candidates.push_back(candidate);

    evidence.ObserveFinal(observation);
    const Snapshot &snapshot = evidence.Read();
    Check(!snapshot.candidates.empty() && snapshot.candidates[0].words.size() == 2,
          "the final snapshot carries the observed words");
    if (!snapshot.candidates.empty() && snapshot.candidates[0].words.size() == 2) {
        Check(snapshot.candidates[0].words[0].energy == Energy::kNonquiet,
              "each candidate word carries its own audio energy");
        Check(snapshot.candidates[0].words[1].energy == Energy::kQuiet,
              "a quiet candidate word is vetoed by its own interval");
    }
}

int main()
{
    BasicSupport();
    PartitionInvariance();
    ShortLastBlock();
    MissingRange();
    OverloadIdentity();
    CleanupReleasesPriorSummaries();
    TwoRecognizersAreIndependent();
    EachCandidateWordCarriesItsOwnEnergy();

    if (!failure.empty()) {
        std::fprintf(stderr, "%s\n", failure.c_str());
        return 1;
    }
    std::printf("test_audio_support: 8 cases green\n");
    return 0;
}
