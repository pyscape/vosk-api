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
using partial_evidence::Kind;
using partial_evidence::Observation;
using partial_evidence::PartialEvidence;
using partial_evidence::Reason;
using partial_evidence::Snapshot;
using partial_evidence::Word;

static std::string failure;

static void Check(bool ok, const char *name)
{
    if (!ok && failure.empty()) {
        failure = std::string(name);
    }
}

static const double kSampleRate = 16000.0;

static Config MakeConfig(double default_hold)
{
    Config config;
    config.quiet_dbfs_set = true;
    config.quiet_dbfs = -40.0;
    config.sample_rate_hz = kSampleRate;
    config.default_hold_ms = default_hold;
    config.profile_id = "final-test";
    return config;
}

static PartialEvidence MakeEvidence(double default_hold)
{
    PartialEvidence evidence;
    evidence.Configure(MakeConfig(default_hold));
    evidence.BeginEpoch(1);
    return evidence;
}

static void FeedRange(PartialEvidence *evidence, int64_t start, std::size_t count,
                      float amp)
{
    std::vector<float> signal(count, amp);
    evidence->AcceptPcm(signal.data(), signal.size(), start);
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

static Observation MakeFinal(uint64_t epoch, int64_t frontier,
                             const std::vector<Word> &words)
{
    Observation observation;
    observation.epoch = epoch;
    observation.accepted_samples = frontier;
    observation.decoded_end_known = true;
    observation.decoded_end_sample = frontier;
    observation.lattice_end_known = true;
    observation.lattice_end_sample = frontier;
    Candidate candidate;
    candidate.primary = true;
    candidate.empty = words.empty();
    candidate.words = words;
    observation.candidates.push_back(candidate);
    return observation;
}

static const Word &FirstWord(const PartialEvidence &evidence)
{
    return evidence.Read().candidates[0].words[0];
}

// [[rr:FVP-5]]
static void FinalEvidenceDoesNotBorrowNextUtterance()
{
    PartialEvidence evidence = MakeEvidence(1000.0);

    FeedRange(&evidence, 0, 1600, 5000.0f);
    evidence.ObserveFinal(MakeFinal(1, 1600, {MakeWord("e", 0, 160)}));
    Check(evidence.Read().kind == Kind::kFinal && evidence.Read().epoch == 1,
          "the first breath finalizes on its own epoch");

    uint64_t first_read = evidence.Read().revision;
    uint64_t second_read = evidence.Read().revision;
    Check(first_read == second_read && evidence.Read().epoch == 1,
          "reading the final twice does not advance it");

    evidence.BeginEpoch(2);
    Check(evidence.Read().epoch == 1,
          "the last final is retained until the next publication");

    FeedRange(&evidence, 0, 1600, 5000.0f);
    evidence.ObserveFinal(MakeFinal(2, 1600, {MakeWord("rook", 0, 160)}));
    Check(evidence.Read().epoch == 2 && FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kFinalOnly,
          "final evidence borrowed the next utterance");

    evidence.BeginEpoch(3);
    Observation empty = MakeFinal(3, 1600, {});
    empty.candidates.clear();
    evidence.ObserveFinal(empty);
    Check(evidence.Read().kind == Kind::kFinal && evidence.Read().epoch == 3 &&
              evidence.Read().candidates.empty(),
          "an empty final after reset fabricates nothing");
}

// [[rr:FVP-7]]
static void FinalSettlesWithoutHold()
{
    PartialEvidence evidence = MakeEvidence(1000.0);
    FeedRange(&evidence, 0, 1600, 5000.0f);
    evidence.ObserveFinal(MakeFinal(1, 1600, {MakeWord("e", 0, 160)}));
    Check(FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kFinalOnly,
          "a supported final word is ready with no stability delay");
}

// [[rr:FVP-7]]
static void QuietFinalVetoed()
{
    PartialEvidence evidence = MakeEvidence(1000.0);
    FeedRange(&evidence, 0, 160, 0.0f);
    evidence.ObserveFinal(MakeFinal(1, 160, {MakeWord("e", 0, 160)}));
    Check(!FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kQuiet,
          "a confirmed quiet final word is vetoed");
}

// [[rr:FVP-7]]
static void UnknownEnergyFinalContinues()
{
    PartialEvidence evidence = MakeEvidence(1000.0);
    evidence.ObserveFinal(MakeFinal(1, 1600, {MakeWord("e", 0, 160)}));
    Check(FirstWord(evidence).ready &&
              FirstWord(evidence).reason == Reason::kFinalOnly,
          "an unknown-energy final continues, distinguished from quiet");
}

// [[rr:FVP-7]]
static void QuietWordRejectsWholeFinal()
{
    PartialEvidence evidence = MakeEvidence(1000.0);
    FeedRange(&evidence, 0, 160, 5000.0f);
    FeedRange(&evidence, 160, 320, 0.0f);
    evidence.ObserveFinal(
        MakeFinal(1, 480, {MakeWord("e", 0, 160), MakeWord("rook", 160, 480)}));
    const Candidate &candidate = evidence.Read().candidates[0];
    Check(candidate.words[1].reason == Reason::kQuiet,
          "the quiet final word carries the quiet reason");
    Check(!candidate.words[0].ready && candidate.ready_prefix == 0,
          "a quiet-vetoed final is rejected as a whole");
}

int main()
{
    FinalEvidenceDoesNotBorrowNextUtterance();
    FinalSettlesWithoutHold();
    QuietFinalVetoed();
    UnknownEnergyFinalContinues();
    QuietWordRejectsWholeFinal();

    if (!failure.empty()) {
        std::fprintf(stderr, "%s\n", failure.c_str());
        return 1;
    }
    std::printf("test_final_evidence: 5 cases green\n");
    return 0;
}
