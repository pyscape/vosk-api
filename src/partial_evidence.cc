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
    // [[rr:FVP-7]]
    energy_frames_.clear();
    support_.clear();
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

// [[rr:FVP-7]]
int64_t PartialEvidence::FrameSamples() const
{
    double frame = active_.sample_rate_hz * 0.01;
    if (!(frame >= 1.0)) {
        return 0;
    }
    return static_cast<int64_t>(frame + 0.5);
}

// [[rr:FVP-7]]
void PartialEvidence::AccumulateSample(int64_t sample_index, double amplitude)
{
    int64_t frame_samples = FrameSamples();
    if (frame_samples <= 0 || sample_index < 0) {
        return;
    }
    FrameEnergy &accum = energy_frames_[sample_index / frame_samples];
    accum.sum_squares += amplitude * amplitude;
    accum.count += 1;
}

// [[rr:FVP-7]]
void PartialEvidence::EvictOldFrames()
{
    int64_t frame_samples = FrameSamples();
    if (frame_samples <= 0 || accepted_samples_ <= 0) {
        return;
    }
    const int64_t retain_frames = 6000;
    int64_t frontier_frame = (accepted_samples_ - 1) / frame_samples;
    int64_t cutoff = frontier_frame - retain_frames + 1;
    while (!energy_frames_.empty() && energy_frames_.begin()->first < cutoff) {
        energy_frames_.erase(energy_frames_.begin());
    }
}

// [[rr:FVP-7]]
void PartialEvidence::AcceptPcm(const float *pcm, std::size_t count, int64_t start_sample)
{
    int64_t frontier = start_sample + static_cast<int64_t>(count);
    if (frontier > accepted_samples_) {
        accepted_samples_ = frontier;
    }
    if (configured_ && pcm != nullptr) {
        for (std::size_t i = 0; i < count; i++) {
            AccumulateSample(start_sample + static_cast<int64_t>(i),
                             static_cast<double>(pcm[i]));
        }
        EvictOldFrames();
    }
    if (!ever_published_) {
        published_.accepted_samples = accepted_samples_;
    }
}

// [[rr:FVP-7]]
void PartialEvidence::AcceptPcm(const int16_t *pcm, std::size_t count, int64_t start_sample)
{
    int64_t frontier = start_sample + static_cast<int64_t>(count);
    if (frontier > accepted_samples_) {
        accepted_samples_ = frontier;
    }
    if (configured_ && pcm != nullptr) {
        for (std::size_t i = 0; i < count; i++) {
            AccumulateSample(start_sample + static_cast<int64_t>(i),
                             static_cast<double>(pcm[i]));
        }
        EvictOldFrames();
    }
    if (!ever_published_) {
        published_.accepted_samples = accepted_samples_;
    }
}

// [[rr:FVP-7]]
Energy PartialEvidence::IntervalEnergy(int64_t start_sample, int64_t end_sample) const
{
    int64_t frame_samples = FrameSamples();
    if (!active_.quiet_dbfs_set || start_sample < 0 ||
        end_sample <= start_sample || frame_samples <= 0) {
        return Energy::kUnknown;
    }
    int64_t first = start_sample / frame_samples;
    int64_t last = (end_sample - 1) / frame_samples;
    bool all_quiet = true;
    for (int64_t frame = first; frame <= last; frame++) {
        std::map<int64_t, FrameEnergy>::const_iterator it = energy_frames_.find(frame);
        if (it == energy_frames_.end() || it->second.count < frame_samples) {
            return Energy::kUnknown;
        }
        double rms = std::sqrt(it->second.sum_squares /
                               static_cast<double>(it->second.count));
        double dbfs = rms <= 0.0 ? kMinQuietDbfs
                                 : 20.0 * std::log10(rms / 32768.0);
        if (dbfs > active_.quiet_dbfs) {
            all_quiet = false;
        }
    }
    return all_quiet ? Energy::kQuiet : Energy::kNonquiet;
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
    // [[rr:FVP-7]]
    for (std::size_t c = 0; c < published_.candidates.size(); c++) {
        std::vector<Word> &words = published_.candidates[c].words;
        for (std::size_t w = 0; w < words.size(); w++) {
            if (words[w].start_sample_known && words[w].end_sample_known) {
                words[w].energy = IntervalEnergy(words[w].start_sample,
                                                 words[w].end_sample);
            }
        }
    }
    // [[rr:FVP-7]]
    if (kind == Kind::kPartial) {
        SettlePartial(&published_);
    }
    lattice_published_ = observation.lattice_end_known;
    published_lattice_end_ = observation.lattice_end_sample;
}

// [[rr:FVP-7]]
double PartialEvidence::HoldMs(const std::string &word) const
{
    std::map<std::string, double>::const_iterator it = active_.hold_ms.find(word);
    if (it != active_.hold_ms.end()) {
        return it->second;
    }
    return active_.default_hold_ms;
}

// [[rr:FVP-7]]
static bool Eligible(const Word &word, Reason *reason)
{
    if (!(word.start_sample_known && word.end_sample_known && word.bin_known)) {
        *reason = Reason::kUnknownAlignment;
        return false;
    }
    if (word.energy == Energy::kUnknown) {
        *reason = Reason::kUnknownAudio;
        return false;
    }
    if (!(word.word_p_known && word.null_p_known && word.word_p > word.null_p)) {
        *reason = Reason::kNullWins;
        return false;
    }
    if (word.energy == Energy::kQuiet) {
        *reason = Reason::kQuiet;
        return false;
    }
    return true;
}

// [[rr:FVP-7]]
static void MarkUnsettled(std::vector<Word> *words)
{
    for (std::size_t i = 0; i < words->size(); i++) {
        Word &word = (*words)[i];
        Reason reason;
        bool eligible = Eligible(word, &reason);
        word.ready = false;
        word.reason = eligible ? Reason::kUnstable : reason;
    }
}

// [[rr:FVP-7]]
void PartialEvidence::SettlePrimary(Candidate *candidate, const Snapshot &snapshot)
{
    std::vector<Word> &words = candidate->words;
    if (words.empty()) {
        support_.clear();
        return;
    }
    bool hole = false;
    for (std::size_t i = 0; i < words.size(); i++) {
        Word &word = words[i];
        Reason reason;
        bool eligible = Eligible(word, &reason);
        if (hole || !eligible) {
            word.ready = false;
            word.reason = eligible ? Reason::kUnstable : reason;
            if (i < support_.size()) {
                support_[i] = WordSupport();
            }
            hole = true;
            continue;
        }
        std::vector<std::string> prefix;
        for (std::size_t p = 0; p < i; p++) {
            prefix.push_back(words[p].word);
        }
        if (support_.size() <= i) {
            support_.resize(i + 1);
        }
        WordSupport &entry = support_[i];
        bool overlaps = entry.start_sample < word.end_sample &&
                        word.start_sample < entry.end_sample;
        bool continues = entry.active && entry.prefix == prefix && overlaps;
        if (continues) {
            if (snapshot.lattice_end_known &&
                snapshot.lattice_end_sample != entry.frontier) {
                double samples = static_cast<double>(snapshot.lattice_end_sample -
                                                     entry.frontier);
                entry.stable_ms += samples / active_.sample_rate_hz * 1000.0;
                entry.frontier = snapshot.lattice_end_sample;
            }
            entry.start_sample = word.start_sample;
            entry.end_sample = word.end_sample;
        } else {
            entry.active = true;
            entry.prefix = prefix;
            entry.start_sample = word.start_sample;
            entry.end_sample = word.end_sample;
            entry.stable_ms = 0.0;
            entry.frontier = snapshot.lattice_end_known
                                 ? snapshot.lattice_end_sample
                                 : 0;
            support_.resize(i + 1);
        }
        word.stable_ms_known = true;
        word.stable_ms = entry.stable_ms;
        if (entry.stable_ms >= HoldMs(word.word)) {
            word.ready = true;
            word.reason = Reason::kReady;
        } else {
            word.ready = false;
            word.reason = Reason::kUnstable;
        }
    }
    if (support_.size() > words.size()) {
        support_.resize(words.size());
    }
}

// [[rr:FVP-7]]
void PartialEvidence::SettlePartial(Snapshot *snapshot)
{
    int primary = -1;
    for (std::size_t c = 0; c < snapshot->candidates.size(); c++) {
        if (snapshot->candidates[c].primary) {
            primary = static_cast<int>(c);
            break;
        }
    }
    if (primary < 0 && !snapshot->candidates.empty()) {
        primary = 0;
    }
    for (std::size_t c = 0; c < snapshot->candidates.size(); c++) {
        if (static_cast<int>(c) != primary) {
            MarkUnsettled(&snapshot->candidates[c].words);
        }
    }
    if (primary >= 0) {
        SettlePrimary(&snapshot->candidates[primary], *snapshot);
    } else {
        support_.clear();
    }
    for (std::size_t c = 0; c < snapshot->candidates.size(); c++) {
        Candidate &candidate = snapshot->candidates[c];
        std::size_t prefix = 0;
        while (prefix < candidate.words.size() && candidate.words[prefix].ready) {
            prefix++;
        }
        candidate.ready_prefix = prefix;
    }
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
