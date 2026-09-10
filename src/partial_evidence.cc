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
