#include "partial_evidence.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using partial_evidence::Config;
using partial_evidence::Kind;
using partial_evidence::Mode;
using partial_evidence::Observation;
using partial_evidence::PartialEvidence;
using partial_evidence::Snapshot;

static std::string failure;

static void Check(bool ok, const char *name)
{
    if (!ok && failure.empty()) {
        failure = std::string("snapshot invariant failed: ") + name;
    }
}

static Config ProbeConfig(const char *profile_id)
{
    Config config;
    config.mode = Mode::kShadow;
    config.final_only = true;
    config.default_hold_ms = 0.0;
    config.quiet_dbfs_set = false;
    config.profile_id = profile_id;
    return config;
}

static Observation Frontier(uint64_t epoch, int64_t accepted, int64_t lattice_end)
{
    Observation observation;
    observation.epoch = epoch;
    observation.accepted_samples = accepted;
    observation.decoded_end_known = true;
    observation.decoded_end_sample = accepted;
    observation.lattice_end_known = true;
    observation.lattice_end_sample = lattice_end;
    return observation;
}

static std::vector<float> Pcm(std::size_t count)
{
    return std::vector<float>(count, 0.0f);
}

// [[rr:FVP-4]]
static void UnconfiguredEvidenceIsUnavailable()
{
    PartialEvidence evidence;
    evidence.BeginEpoch(1);
    std::vector<float> block = Pcm(320);
    evidence.AcceptPcm(block.data(), block.size(), 0);
    evidence.ObservePartial(Frontier(1, 320, 160));

    const Snapshot &snapshot = evidence.Read();
    Check(snapshot.schema == partial_evidence::kSchema, "unconfigured schema");
    Check(snapshot.kind == Kind::kUnavailable, "unconfigured kind");
    Check(snapshot.revision == 0, "unconfigured revision");
    Check(snapshot.accepted_samples == 320, "unconfigured accepted samples");
}

// [[rr:FVP-4]]
static void ConfigurationTakesEffectAtTheNextEpoch()
{
    PartialEvidence evidence;
    evidence.BeginEpoch(1);
    Check(evidence.Configure(ProbeConfig("fpe2")), "configure accepted");
    Check(evidence.Read().kind == Kind::kUnavailable, "configure defers to next epoch");

    evidence.BeginEpoch(2);
    const Snapshot &empty = evidence.Read();
    Check(empty.kind == Kind::kEmpty, "configured empty kind");
    Check(empty.profile_id == "fpe2", "configured profile id");
    Check(empty.epoch == 2, "configured epoch");
}

// [[rr:FVP-5]]
static void AcceptedSamplesStayMonotoneAcrossCleanup()
{
    PartialEvidence evidence;
    evidence.Configure(ProbeConfig("fpe2"));
    evidence.BeginEpoch(1);
    std::vector<float> block = Pcm(16000);
    evidence.AcceptPcm(block.data(), block.size(), 0);
    evidence.ObserveFinal(Frontier(1, 16000, 15000));
    int64_t before = evidence.Read().accepted_samples;

    evidence.BeginEpoch(2);
    evidence.AcceptPcm(block.data(), block.size(), 16000);
    evidence.ObservePartial(Frontier(2, 32000, 31000));
    int64_t after = evidence.Read().accepted_samples;

    Check(before == 16000, "accepted samples before cleanup");
    Check(after == 32000, "accepted samples after cleanup");
    Check(after >= before, "accepted samples monotone across cleanup");
}

// [[rr:FVP-5]]
static void SnapshotSurvivesCleanupUntilTheNextPublication()
{
    PartialEvidence evidence;
    evidence.Configure(ProbeConfig("fpe2"));
    evidence.BeginEpoch(1);
    evidence.ObserveFinal(Frontier(1, 16000, 15000));

    evidence.BeginEpoch(2);
    const Snapshot &carried = evidence.Read();
    Check(carried.kind == Kind::kFinal, "published final survives cleanup");
    Check(carried.epoch == 1, "published final keeps its epoch");

    evidence.ObservePartial(Frontier(2, 20000, 19000));
    const Snapshot &fresh = evidence.Read();
    Check(fresh.kind == Kind::kPartial, "next publication replaces the snapshot");
    Check(fresh.epoch == 2, "reset identity changes the epoch");
    Check(fresh.revision == 1, "revision restarts within the new epoch");
}

// [[rr:FVP-5]]
static void RepeatedReadsReturnTheSameSnapshot()
{
    PartialEvidence evidence;
    evidence.Configure(ProbeConfig("fpe2"));
    evidence.BeginEpoch(1);
    evidence.ObservePartial(Frontier(1, 8000, 7000));
    uint64_t first = evidence.Read().revision;
    uint64_t second = evidence.Read().revision;
    Check(first == second, "repeat read idempotence");

    evidence.ObservePartial(Frontier(1, 9000, 7000));
    Check(evidence.Read().revision == first, "same lattice does not advance revision");

    evidence.ObservePartial(Frontier(1, 9000, 8500));
    Check(evidence.Read().revision == first + 1, "new lattice frontier advances revision");
}

// [[rr:FVP-4]]
static void InvalidConfigurationLeavesThePreviousOneIntact()
{
    PartialEvidence evidence;
    evidence.Configure(ProbeConfig("fpe2"));
    evidence.BeginEpoch(1);

    Config negative = ProbeConfig("rejected");
    negative.default_hold_ms = -1.0;
    Check(!evidence.Configure(negative), "negative hold rejected");

    Config nonfinite = ProbeConfig("rejected");
    nonfinite.final_only = false;
    nonfinite.hold_ms["knight"] = std::numeric_limits<double>::infinity();
    Check(!evidence.Configure(nonfinite), "nonfinite hold rejected");

    Config loud = ProbeConfig("rejected");
    loud.quiet_dbfs_set = true;
    loud.quiet_dbfs = 12.0;
    Check(!evidence.Configure(loud), "out of range dbfs rejected");

    evidence.BeginEpoch(2);
    Check(evidence.config().profile_id == "fpe2", "rejected configuration is atomic");
    Check(evidence.Read().profile_id == "fpe2", "rejected configuration leaves the snapshot");
}

// [[rr:FVP-5]]
static void RecognizersKeepIndependentClocks()
{
    PartialEvidence first;
    PartialEvidence second;
    first.Configure(ProbeConfig("first"));
    first.BeginEpoch(1);
    second.BeginEpoch(1);

    std::vector<float> block = Pcm(4000);
    first.AcceptPcm(block.data(), block.size(), 0);
    first.ObserveFinal(Frontier(1, 4000, 3000));

    Check(first.Read().accepted_samples == 4000, "first recognizer clock");
    Check(second.Read().accepted_samples == 0, "second recognizer clock untouched");
    Check(second.Read().kind == Kind::kUnavailable, "second recognizer stays unconfigured");
    Check(second.Read().profile_id.empty(), "second recognizer has no profile");
}

int main()
{
    UnconfiguredEvidenceIsUnavailable();
    ConfigurationTakesEffectAtTheNextEpoch();
    AcceptedSamplesStayMonotoneAcrossCleanup();
    SnapshotSurvivesCleanupUntilTheNextPublication();
    RepeatedReadsReturnTheSameSnapshot();
    InvalidConfigurationLeavesThePreviousOneIntact();
    RecognizersKeepIndependentClocks();

    if (!failure.empty()) {
        std::fprintf(stderr, "%s\n", failure.c_str());
        return 1;
    }
    std::printf("test_snapshot: 7 cases green\n");
    return 0;
}
