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

#include <cmath>

namespace partial_evidence {

static bool FiniteDuration(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

// [[rr:FVP-4]]
bool Validate(const Config &config)
{
    if (!FiniteDuration(config.default_hold_ms)) {
        return false;
    }
    for (std::map<std::string, double>::const_iterator it = config.hold_ms.begin();
         it != config.hold_ms.end(); ++it) {
        if (!FiniteDuration(it->second)) {
            return false;
        }
    }
    if (config.quiet_dbfs_set) {
        if (!std::isfinite(config.quiet_dbfs) ||
            config.quiet_dbfs > kMaxQuietDbfs ||
            config.quiet_dbfs < kMinQuietDbfs) {
            return false;
        }
    }
    return true;
}

// [[rr:FVP-4]]
bool PartialEvidence::Configure(const Config &config)
{
    if (!Validate(config)) {
        return false;
    }
    pending_config_ = config;
    pending_ = true;
    return true;
}

// [[rr:FVP-5]]
void PartialEvidence::BeginEpoch(uint64_t epoch)
{
    if (pending_) {
        active_ = pending_config_;
        configured_ = true;
        pending_ = false;
    }
    epoch_ = epoch;
    revision_ = 0;
    lattice_published_ = false;
    published_lattice_end_ = 0;
    if (!ever_published_) {
        published_ = Snapshot();
        published_.epoch = epoch_;
        published_.accepted_samples = accepted_samples_;
        published_.kind = configured_ ? Kind::kEmpty : Kind::kUnavailable;
        if (configured_) {
            published_.profile_id = active_.profile_id;
            published_.mode = active_.mode;
        }
    }
}

void PartialEvidence::AcceptPcm(const float *pcm, std::size_t count, int64_t start_sample)
{
    (void)pcm;
    int64_t frontier = start_sample + static_cast<int64_t>(count);
    if (frontier > accepted_samples_) {
        accepted_samples_ = frontier;
    }
    if (!ever_published_) {
        published_.accepted_samples = accepted_samples_;
    }
}

// [[rr:FVP-5]]
void PartialEvidence::ObservePartial(const Observation &observation)
{
    if (!configured_ || observation.epoch != epoch_) {
        return;
    }
    bool advance;
    if (observation.lattice_end_known) {
        advance = !lattice_published_ ||
                  observation.lattice_end_sample != published_lattice_end_;
    } else {
        advance = revision_ == 0;
    }
    if (!advance) {
        return;
    }
    Publish(observation, Kind::kPartial);
}

void PartialEvidence::ObserveFinal(const Observation &observation)
{
    if (!configured_ || observation.epoch != epoch_) {
        return;
    }
    Publish(observation, Kind::kFinal);
}

// [[rr:FVP-5]]
void PartialEvidence::Publish(const Observation &observation, Kind kind)
{
    if (observation.accepted_samples > accepted_samples_) {
        accepted_samples_ = observation.accepted_samples;
    }
    ++revision_;
    ever_published_ = true;
    published_ = Snapshot();
    published_.profile_id = active_.profile_id;
    published_.mode = active_.mode;
    published_.epoch = epoch_;
    published_.revision = revision_;
    published_.kind = kind;
    published_.accepted_samples = accepted_samples_;
    published_.decoded_end_known = observation.decoded_end_known;
    published_.decoded_end_sample = observation.decoded_end_sample;
    published_.lattice_end_known = observation.lattice_end_known;
    published_.lattice_end_sample = observation.lattice_end_sample;
    published_.candidates = observation.candidates;
    lattice_published_ = observation.lattice_end_known;
    published_lattice_end_ = observation.lattice_end_sample;
}

}  // namespace partial_evidence

#if defined(__has_include) && __has_include("lat/sausages.h")

#include "fstext/fstext-utils.h"
#include "lat/lattice-functions.h"
#include <fst/shortest-path.h>

#include <algorithm>
#include <set>
#include <utility>

namespace partial_evidence {

typedef kaldi::int32 int32;
typedef kaldi::BaseFloat BaseFloat;

typedef std::vector<std::pair<float, float> > BinTimes;
typedef std::vector<std::vector<std::pair<int32, BaseFloat> > > BinStats;

static bool LinearWordAlignment(const kaldi::CompactLattice &clat,
                                std::vector<int32> *words,
                                std::vector<int32> *begin_frames,
                                std::vector<int32> *lengths,
                                double *cost)
{
    typedef kaldi::CompactLattice::Arc Arc;
    typedef kaldi::CompactLattice::Weight Weight;
    words->clear();
    begin_frames->clear();
    lengths->clear();
    *cost = 0.0;
    kaldi::CompactLattice::StateId state = clat.Start();
    if (state == fst::kNoStateId) {
        return false;
    }
    double total = 0.0;
    int32 cursor = 0;
    while (true) {
        Weight final = clat.Final(state);
        if (final != Weight::Zero()) {
            if (clat.NumArcs(state) != 0) {
                return false;
            }
            total += final.Weight().Value1() + final.Weight().Value2();
            *cost = total;
            return true;
        }
        if (clat.NumArcs(state) != 1) {
            return false;
        }
        fst::ArcIterator<kaldi::CompactLattice> aiter(clat, state);
        const Arc &arc = aiter.Value();
        words->push_back(arc.ilabel);
        begin_frames->push_back(cursor);
        lengths->push_back(static_cast<int32>(arc.weight.String().size()));
        total += arc.weight.Weight().Value1() + arc.weight.Weight().Value2();
        cursor += static_cast<int32>(arc.weight.String().size());
        state = arc.nextstate;
    }
}

// [[rr:FVP-6]]
static int MaxOverlapBin(double begin, double end, const BinTimes &times)
{
    int best = -1;
    double best_overlap = 0.0;
    bool tied = false;
    for (std::size_t b = 0; b < times.size(); b++) {
        double lo = std::max(static_cast<double>(times[b].first), begin);
        double hi = std::min(static_cast<double>(times[b].second), end);
        double overlap = hi - lo;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            best = static_cast<int>(b);
            tied = false;
        } else if (overlap == best_overlap && overlap > 0.0) {
            tied = true;
        }
    }
    if (best_overlap <= 0.0 || tied) {
        return -1;
    }
    return best;
}

// [[rr:FVP-6]]
static void ResolveWord(Word *word, int32 word_id, double begin_frame,
                        double end_frame, const SampleClock &clock,
                        const BinTimes &bin_times, const BinStats &bin_stats)
{
    word->start_sample_known = true;
    word->start_sample = clock.ToSample(begin_frame);
    word->end_sample_known = true;
    word->end_sample = clock.ToSample(end_frame);

    int bin = MaxOverlapBin(begin_frame, end_frame, bin_times);
    if (bin < 0 || static_cast<std::size_t>(bin) >= bin_stats.size()) {
        word->ready = false;
        word->reason = Reason::kUnknownAlignment;
        return;
    }
    word->bin_known = true;
    word->bin = bin;
    for (std::size_t k = 0; k < bin_stats[bin].size(); k++) {
        int32 id = bin_stats[bin][k].first;
        if (id == word_id) {
            word->word_p_known = true;
            word->word_p = bin_stats[bin][k].second;
        }
        if (id == 0) {
            word->null_p_known = true;
            word->null_p = bin_stats[bin][k].second;
        }
    }
    bool eligible = word->word_p_known && word->null_p_known &&
                    word->word_p > word->null_p;
    word->ready = eligible;
    word->reason = eligible ? Reason::kReady : Reason::kNullWins;
}

// [[rr:FVP-6]]
static void FinalizeReadyPrefix(Candidate *candidate)
{
    std::size_t prefix = 0;
    while (prefix < candidate->words.size() && candidate->words[prefix].ready) {
        prefix++;
    }
    candidate->ready_prefix = prefix;
}

// [[rr:FVP-6]]
void ExtractCandidates(const kaldi::CompactLattice &aligned_lat,
                       kaldi::MinimumBayesRisk *mbr,
                       const fst::SymbolTable *word_syms,
                       const ExtractParams &params,
                       Observation *observation)
{
    observation->candidates.clear();
    observation->truncated = false;

    BinTimes bin_times = mbr->GetSausageTimes();
    const BinStats &bin_stats = mbr->GetSausageStats();

    Candidate primary;
    primary.id = 0;
    primary.primary = true;
    const std::vector<int32> &best = mbr->GetOneBest();
    const std::vector<std::pair<BaseFloat, BaseFloat> > &best_times =
        mbr->GetOneBestTimes();
    std::vector<int32> primary_seq;
    for (std::size_t i = 0; i < best.size(); i++) {
        Word word;
        word.word = word_syms->Find(best[i]);
        double begin = (i < best_times.size()) ? best_times[i].first : 0.0;
        double end = (i < best_times.size()) ? best_times[i].second : 0.0;
        ResolveWord(&word, best[i], begin, end, params.clock, bin_times, bin_stats);
        primary.words.push_back(word);
        primary_seq.push_back(best[i]);
    }
    primary.empty = primary.words.empty();
    FinalizeReadyPrefix(&primary);
    observation->candidates.push_back(primary);

    int requested = params.distinct_paths > 0 ? params.distinct_paths : 1;
    int bound = requested * 10;

    kaldi::Lattice lat;
    ConvertLattice(aligned_lat, &lat);
    kaldi::Lattice nbest;
    fst::ShortestPath(lat, &nbest, bound);
    std::vector<kaldi::Lattice> paths;
    fst::ConvertNbestToVector(nbest, &paths);

    std::set<std::vector<int32> > seen;
    seen.insert(primary_seq);

    int distinct = 1;
    for (std::size_t k = 0; k < paths.size() && distinct < requested; k++) {
        kaldi::CompactLattice path_clat;
        ConvertLattice(paths[k], &path_clat);
        std::vector<int32> ids, begins, lengths;
        double cost = 0.0;
        if (!LinearWordAlignment(path_clat, &ids, &begins, &lengths, &cost)) {
            continue;
        }
        std::vector<int32> seq;
        for (std::size_t i = 0; i < ids.size(); i++) {
            if (ids[i] != 0) {
                seq.push_back(ids[i]);
            }
        }
        if (seq.empty() || seen.count(seq)) {
            continue;
        }
        seen.insert(seq);

        Candidate candidate;
        candidate.id = distinct;
        candidate.likelihood_known = true;
        candidate.likelihood = -cost;
        for (std::size_t i = 0; i < ids.size(); i++) {
            if (ids[i] == 0) {
                continue;
            }
            Word word;
            word.word = word_syms->Find(ids[i]);
            double begin = begins[i];
            double end = begins[i] + lengths[i];
            ResolveWord(&word, ids[i], begin, end, params.clock, bin_times, bin_stats);
            candidate.words.push_back(word);
        }
        FinalizeReadyPrefix(&candidate);
        observation->candidates.push_back(candidate);
        distinct++;
    }

    if (distinct < requested && static_cast<int>(paths.size()) >= bound) {
        observation->truncated = true;
    }
}

}  // namespace partial_evidence

#endif  // Kaldi available
