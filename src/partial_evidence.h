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

#ifndef VOSK_PARTIAL_EVIDENCE_H
#define VOSK_PARTIAL_EVIDENCE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace partial_evidence {

const int kSchema = 1;
const double kMinQuietDbfs = -200.0;
const double kMaxQuietDbfs = 0.0;

enum class Mode { kShadow, kEnforce };

enum class Kind { kUnavailable, kEmpty, kPartial, kFinal };

enum class Energy { kUnknown, kQuiet, kNonquiet };

enum class Reason {
    kReady,
    kUnstable,
    kNullWins,
    kQuiet,
    kUnknownAlignment,
    kUnknownAudio,
    kFinalOnly,
    kStale
};

// [[rr:FVP-4]]
struct Config {
    Mode mode = Mode::kShadow;
    bool final_only = true;
    std::map<std::string, double> hold_ms;
    double default_hold_ms = 0.0;
    bool quiet_dbfs_set = false;
    double quiet_dbfs = 0.0;
    double sample_rate_hz = 16000.0;
    std::string profile_id;
};

// [[rr:FVP-5]]
struct Word {
    std::string word;
    bool start_sample_known = false;
    int64_t start_sample = 0;
    bool end_sample_known = false;
    int64_t end_sample = 0;
    bool bin_known = false;
    int64_t bin = 0;
    bool word_p_known = false;
    double word_p = 0.0;
    bool null_p_known = false;
    double null_p = 0.0;
    Energy energy = Energy::kUnknown;
    bool stable_ms_known = false;
    double stable_ms = 0.0;
    bool ready = false;
    Reason reason = Reason::kUnknownAlignment;
};

struct Candidate {
    int id = 0;
    std::vector<Word> words;
    std::size_t ready_prefix = 0;
    bool primary = false;
    bool empty = false;
    bool likelihood_known = false;
    double likelihood = 0.0;
};

struct Observation {
    uint64_t epoch = 0;
    int64_t accepted_samples = 0;
    bool decoded_end_known = false;
    int64_t decoded_end_sample = 0;
    bool lattice_end_known = false;
    int64_t lattice_end_sample = 0;
    std::vector<Candidate> candidates;
    bool truncated = false;
};

// [[rr:FVP-5]]
struct Snapshot {
    int schema = kSchema;
    std::string profile_id;
    Mode mode = Mode::kShadow;
    uint64_t epoch = 0;
    uint64_t revision = 0;
    Kind kind = Kind::kUnavailable;
    int64_t accepted_samples = 0;
    bool decoded_end_known = false;
    int64_t decoded_end_sample = 0;
    bool lattice_end_known = false;
    int64_t lattice_end_sample = 0;
    std::vector<Candidate> candidates;
    bool truncated = false;
};

bool Validate(const Config &config);

// [[rr:FVP-4]]
class PartialEvidence {
    public:
        bool Configure(const Config &config);
        void BeginEpoch(uint64_t epoch);
        void AcceptPcm(const float *pcm, std::size_t count, int64_t start_sample);
        void AcceptPcm(const int16_t *pcm, std::size_t count, int64_t start_sample);
        Energy IntervalEnergy(int64_t start_sample, int64_t end_sample) const;
        void ObservePartial(const Observation &observation);
        void ObserveFinal(const Observation &observation);
        const Snapshot &Read() const { return published_; }

        bool configured() const { return configured_; }
        const Config &config() const { return active_; }

    private:
        struct FrameEnergy {
            double sum_squares = 0.0;
            int64_t count = 0;
        };

        struct WordSupport {
            bool active = false;
            std::vector<std::string> prefix;
            int64_t start_sample = 0;
            int64_t end_sample = 0;
            double stable_ms = 0.0;
            int64_t frontier = 0;
        };

        void Publish(const Observation &observation, Kind kind);
        int64_t FrameSamples() const;
        void AccumulateSample(int64_t sample_index, double amplitude);
        void EvictOldFrames();
        void SettlePartial(Snapshot *snapshot);
        void SettlePrimary(Candidate *candidate, const Snapshot &snapshot);
        void SettleFinal(Snapshot *snapshot);
        double HoldMs(const std::string &word) const;

        std::map<int64_t, FrameEnergy> energy_frames_;
        std::vector<WordSupport> support_;
        bool configured_ = false;
        bool pending_ = false;
        bool ever_published_ = false;
        Config active_;
        Config pending_config_;
        uint64_t epoch_ = 0;
        uint64_t revision_ = 0;
        int64_t accepted_samples_ = 0;
        bool lattice_published_ = false;
        int64_t published_lattice_end_ = 0;
        Snapshot published_;
};

}  // namespace partial_evidence

#if defined(__has_include) && __has_include("lat/sausages.h")

#include "lat/sausages.h"
#include "lat/kaldi-lattice.h"

namespace partial_evidence {

// [[rr:FVP-5]]
struct SampleClock {
    int64_t base_sample = 0;
    int64_t frame_offset = 0;
    double samples_per_frame = 0.0;

    int64_t ToSample(double frame) const {
        return base_sample +
               static_cast<int64_t>((static_cast<double>(frame_offset) + frame) *
                                    samples_per_frame);
    }
};

// [[rr:FVP-6]]
struct ExtractParams {
    int distinct_paths = 4;
    SampleClock clock;
};

// [[rr:FVP-6]]
void ExtractCandidates(const kaldi::CompactLattice &aligned_lat,
                       kaldi::MinimumBayesRisk *mbr,
                       const fst::SymbolTable *word_syms,
                       const ExtractParams &params,
                       Observation *observation);

}  // namespace partial_evidence

#endif  // Kaldi available

#endif /* VOSK_PARTIAL_EVIDENCE_H */
