// Copyright 2026 ChessAmis
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "partial_evidence.h"

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

using partial_evidence::Candidate;
using partial_evidence::Config;
using partial_evidence::Observation;
using partial_evidence::PartialEvidence;
using partial_evidence::Reason;
using partial_evidence::Snapshot;
using partial_evidence::Word;

static std::string failure;

static void Check(bool ok, const char *name)
{
    if (!ok && failure.empty()) {
        failure = std::string("readiness invariant failed: ") + name;
    }
}

static const double kHold = 30.0;
static const double kSampleRate = 16000.0;
static const int64_t kStep = 320;

static Config MakeConfig(double default_hold)
{
    Config config;
    config.quiet_dbfs_set = true;
    config.quiet_dbfs = -40.0;
    config.sample_rate_hz = kSampleRate;
    config.default_hold_ms = default_hold;
    config.profile_id = "readiness-test";
    return config;
}

static PartialEvidence MakeEvidence(double default_hold)
{
    PartialEvidence evidence;
    evidence.Configure(MakeConfig(default_hold));
    evidence.BeginEpoch(1);
    return evidence;
}

static void FeedLevel(PartialEvidence *evidence, std::size_t total, float amp)
{
    std::vector<float> signal(total, amp);
    evidence->AcceptPcm(signal.data(), signal.size(), 0);
}

static Word MakeWord(const std::string &text, int64_t start, int64_t end)
{
    Word word;
    word.word = text;
    word.start_sample_known = true;
    word.start_sample = start;
    word.end_sample_known = true;
    word.end_sample = end;
    word.bin_known = true;
    word.bin = 0;
    word.word_p_known = true;
    word.word_p = 0.8;
    word.null_p_known = true;
    word.null_p = 0.1;
    return word;
}

static Observation MakeObservation(int64_t frontier)
{
    Observation observation;
    observation.epoch = 1;
    observation.accepted_samples = frontier;
    observation.decoded_end_known = true;
    observation.decoded_end_sample = frontier;
    observation.lattice_end_known = true;
    observation.lattice_end_sample = frontier;
    return observation;
}

static void PushCandidate(Observation *observation, const std::vector<Word> &words)
{
    Candidate candidate;
    candidate.primary = true;
    candidate.empty = words.empty();
    candidate.words = words;
    observation->candidates.push_back(candidate);
}

static const Word &FirstWord(const PartialEvidence &evidence)
{
    return evidence.Read().candidates[0].words[0];
}

// [[rr:FVP-7]]
static void PositiveUninterruptedSettles()
{
    PartialEvidence evidence = MakeEvidence(kHold);
    FeedLevel(&evidence, 1600, 5000.0f);

    Observation o1 = MakeObservation(kStep);
    PushCandidate(&o1, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o1);
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kUnstable,
          "first support is not ready");

    Observation o2 = MakeObservation(2 * kStep);
    PushCandidate(&o2, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o2);
    Check(!FirstWord(evidence).ready,
          "one corroborating frontier is still short of the hold");

    Observation o3 = MakeObservation(3 * kStep);
    PushCandidate(&o3, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o3);
    Check(FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kReady,
          "an uninterrupted word whose fixed end is corroborated becomes ready");
}

// [[rr:FVP-7]]
static void AlternatingEmptyNeverSettles()
{
    PartialEvidence evidence = MakeEvidence(kHold);
    FeedLevel(&evidence, 1600, 5000.0f);

    for (int block = 1; block <= 9; block++) {
        Observation obs = MakeObservation(static_cast<int64_t>(block) * kStep);
        if (block % 2 == 1) {
            PushCandidate(&obs, {MakeWord("e", 0, 160)});
        } else {
            PushCandidate(&obs, {});
        }
        evidence.ObservePartial(obs);
    }

    const Snapshot &snapshot = evidence.Read();
    Check(!snapshot.candidates.empty() && !snapshot.candidates[0].words.empty(),
          "the ninth block publishes the surviving reading");
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kUnstable,
          "interrupted evidence settled a word");
}

// [[rr:FVP-5]]
static void DuplicatePollAddsNoDuration()
{
    PartialEvidence evidence = MakeEvidence(15.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    Observation o1 = MakeObservation(kStep);
    PushCandidate(&o1, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o1);

    Observation dup = MakeObservation(kStep);
    PushCandidate(&dup, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(dup);
    Check(!FirstWord(evidence).ready,
          "a duplicate poll of the same lattice frontier adds no duration");

    Observation o2 = MakeObservation(2 * kStep);
    PushCandidate(&o2, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o2);
    Check(FirstWord(evidence).ready,
          "a genuinely fresh frontier still settles the word");
}

// [[rr:FVP-5]]
static void DecodedOnlyProgressIsStale()
{
    PartialEvidence evidence = MakeEvidence(15.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    Observation o1 = MakeObservation(kStep);
    PushCandidate(&o1, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(o1);
    uint64_t revision_after_first = evidence.Read().revision;

    Observation stale = MakeObservation(kStep);
    stale.decoded_end_sample = 4 * kStep;
    PushCandidate(&stale, {MakeWord("e", 0, 160)});
    evidence.ObservePartial(stale);
    Check(evidence.Read().revision == revision_after_first &&
              !FirstWord(evidence).ready,
          "decoded progress over a stale lattice is not new support");
}

// [[rr:FVP-6]]
static void NullVictoryStaysUnready()
{
    PartialEvidence evidence = MakeEvidence(1.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    for (int block = 1; block <= 4; block++) {
        Observation obs = MakeObservation(static_cast<int64_t>(block) * kStep);
        Word word = MakeWord("e", 0, 160);
        word.word_p = 0.1;
        word.null_p = 0.8;
        PushCandidate(&obs, {word});
        evidence.ObservePartial(obs);
    }
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kNullWins,
          "a word whose null evidence wins never settles");
}

// [[rr:FVP-7]]
static void QuietVetoStaysUnready()
{
    PartialEvidence evidence = MakeEvidence(1.0);
    FeedLevel(&evidence, 1600, 0.0f);

    for (int block = 1; block <= 4; block++) {
        Observation obs = MakeObservation(static_cast<int64_t>(block) * kStep);
        PushCandidate(&obs, {MakeWord("e", 0, 160)});
        evidence.ObservePartial(obs);
    }
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kQuiet,
          "a quiet observation vetoes readiness");
}

// [[rr:FVP-6]]
static void AlignmentUncertaintyStaysUnready()
{
    PartialEvidence evidence = MakeEvidence(1.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    for (int block = 1; block <= 4; block++) {
        Observation obs = MakeObservation(static_cast<int64_t>(block) * kStep);
        Word word = MakeWord("e", 0, 160);
        word.bin_known = false;
        PushCandidate(&obs, {word});
        evidence.ObservePartial(obs);
    }
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kUnknownAlignment,
          "an unaligned word cannot settle");
}

// [[rr:FVP-7]]
static void PrefixReplacementClearsSuffixKeepsPrefix()
{
    PartialEvidence evidence = MakeEvidence(15.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    Observation o1 = MakeObservation(kStep);
    PushCandidate(&o1, {MakeWord("e", 0, 160), MakeWord("rook", 160, 480)});
    evidence.ObservePartial(o1);

    Observation o2 = MakeObservation(2 * kStep);
    PushCandidate(&o2, {MakeWord("e", 0, 160), MakeWord("rook", 160, 480)});
    evidence.ObservePartial(o2);
    Check(evidence.Read().candidates[0].ready_prefix == 2,
          "two corroborated words settle a two-word prefix");

    Observation o3 = MakeObservation(3 * kStep);
    PushCandidate(&o3, {MakeWord("e", 0, 160), MakeWord("rook", 800, 960)});
    evidence.ObservePartial(o3);
    const Candidate &candidate = evidence.Read().candidates[0];
    Check(candidate.words[0].ready,
          "clearing a suffix preserves the independently supported prefix");
    Check(!candidate.words[1].ready &&
              candidate.ready_prefix == 1,
          "a nonoverlapping occurrence clears the suffix word");
}

// [[rr:FVP-6]]
static void BlockedPrefixHoldsLaterWord()
{
    PartialEvidence evidence = MakeEvidence(1.0);
    FeedLevel(&evidence, 1600, 5000.0f);

    for (int block = 1; block <= 4; block++) {
        Observation obs = MakeObservation(static_cast<int64_t>(block) * kStep);
        Word blocked = MakeWord("e", 0, 160);
        blocked.word_p = 0.1;
        blocked.null_p = 0.8;
        PushCandidate(&obs, {blocked, MakeWord("rook", 160, 480)});
        evidence.ObservePartial(obs);
    }
    const Candidate &candidate = evidence.Read().candidates[0];
    Check(!candidate.words[0].ready &&
              candidate.words[0].reason == Reason::kNullWins,
          "the earlier blocked word stays unready");
    Check(!candidate.words[1].ready && candidate.ready_prefix == 0,
          "a later word cannot dispatch through an earlier hole");
}

int main()
{
    PositiveUninterruptedSettles();
    AlternatingEmptyNeverSettles();
    DuplicatePollAddsNoDuration();
    DecodedOnlyProgressIsStale();
    NullVictoryStaysUnready();
    QuietVetoStaysUnready();
    AlignmentUncertaintyStaysUnready();
    PrefixReplacementClearsSuffixKeepsPrefix();
    BlockedPrefixHoldsLaterWord();

    if (!failure.empty()) {
        std::fprintf(stderr, "%s\n", failure.c_str());
        return 1;
    }
    std::printf("test_readiness: 9 cases green\n");
    return 0;
}
