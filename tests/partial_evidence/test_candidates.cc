#include "partial_evidence.h"

#include "lat/kaldi-lattice.h"
#include "lat/sausages.h"
#include <fst/symbol-table.h>

#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

using kaldi::CompactLattice;
using kaldi::CompactLatticeArc;
using kaldi::CompactLatticeWeight;
using kaldi::LatticeWeight;
using kaldi::MinimumBayesRisk;
using partial_evidence::Candidate;
using partial_evidence::ExtractParams;
using partial_evidence::Observation;
using partial_evidence::Reason;
using partial_evidence::Word;

static std::string failure;

static void Check(bool ok, const char *name)
{
    if (!ok && failure.empty()) {
        failure = std::string("candidate invariant failed: ") + name;
    }
}

// The words and the empty-wins scenario are FPE-1's read-aloud control decode.
static void AddWord(CompactLattice *lat, int from, int to, int word,
                    int frames, float graph, float acoustic)
{
    std::vector<kaldi::int32> align(static_cast<std::size_t>(frames), 1);
    CompactLatticeWeight weight(LatticeWeight(graph, acoustic), align);
    lat->AddArc(from, CompactLatticeArc(word, word, weight, to));
}

static fst::SymbolTable MakeSyms()
{
    fst::SymbolTable syms;
    syms.AddSymbol("<eps>", 0);
    syms.AddSymbol("e", 1);
    syms.AddSymbol("a", 2);
    syms.AddSymbol("if", 3);
    return syms;
}

static std::vector<std::string> Sequence(const Candidate &candidate)
{
    std::vector<std::string> words;
    for (std::size_t i = 0; i < candidate.words.size(); i++) {
        words.push_back(candidate.words[i].word);
    }
    return words;
}

static const Candidate *FindSequence(const Observation &observation,
                                     const std::vector<std::string> &target)
{
    for (std::size_t i = 0; i < observation.candidates.size(); i++) {
        if (Sequence(observation.candidates[i]) == target) {
            return &observation.candidates[i];
        }
    }
    return nullptr;
}

static int CountSequence(const Observation &observation,
                         const std::vector<std::string> &target)
{
    int count = 0;
    for (std::size_t i = 0; i < observation.candidates.size(); i++) {
        if (Sequence(observation.candidates[i]) == target) {
            count++;
        }
    }
    return count;
}

// [[rr:FVP-6]]
static void EmptyPrimaryAndDistinctRivals()
{
    CompactLattice lat;
    int s0 = lat.AddState();
    int mid = lat.AddState();
    int sf = lat.AddState();
    lat.SetStart(s0);
    lat.SetFinal(sf, CompactLatticeWeight::One());

    (void)mid;
    AddWord(&lat, s0, sf, 0, 8, 0.0f, 0.0f);
    AddWord(&lat, s0, sf, 1, 8, 10.0f, 10.0f);
    AddWord(&lat, s0, sf, 1, 8, 11.0f, 11.0f);
    AddWord(&lat, s0, sf, 2, 8, 10.5f, 10.0f);
    AddWord(&lat, s0, sf, 3, 8, 10.5f, 10.5f);

    MinimumBayesRisk mbr(lat);
    Check(mbr.GetOneBest().empty(), "constructed lattice has an empty MBR primary");

    fst::SymbolTable syms = MakeSyms();
    ExtractParams params;
    params.distinct_paths = 6;
    params.clock.base_sample = 0;
    params.clock.frame_offset = 0;
    params.clock.samples_per_frame = 480.0;

    Observation observation;
    partial_evidence::ExtractCandidates(lat, &mbr, &syms, params, &observation);

    Check(!observation.candidates.empty(), "candidates were produced");
    if (observation.candidates.empty()) {
        return;
    }

    const Candidate &primary = observation.candidates[0];
    Check(primary.primary, "the first candidate is the MBR primary");
    Check(primary.words.empty(), "empty primary was promoted to a word");
    Check(primary.empty, "the empty primary is marked empty");

    std::vector<std::string> single_e;
    single_e.push_back("e");
    Check(CountSequence(observation, single_e) == 1,
          "duplicate word paths collapse to one candidate");

    std::vector<std::string> rival_a;
    rival_a.push_back("a");
    const Candidate *a = FindSequence(observation, rival_a);
    Check(a != nullptr, "the genuine rival was extracted");
    if (a != nullptr) {
        const Word &w = a->words[0];
        Check(w.start_sample_known && w.end_sample_known, "the rival is timed");
        Check(w.end_sample > w.start_sample, "the rival interval is half-open and positive");
        Check(a->likelihood_known, "the rival keeps a raw likelihood");
        Check(a->likelihood < 0.0, "raw likelihood is not a probability");
        Check(w.null_p_known, "confusion supplies null evidence for the rival");
        Check(w.null_p >= 0.0 && w.null_p <= 1.0, "null posterior stays a probability");
        Check(w.reason == Reason::kNullWins || !w.ready,
              "null wins keep the rival out of the ready prefix");
    }

    int primaries = 0;
    for (std::size_t i = 0; i < observation.candidates.size(); i++) {
        if (observation.candidates[i].primary) {
            primaries++;
        }
    }
    Check(primaries == 1, "exactly one primary is identified");
}

// [[rr:FVP-6]]
static void RepeatedWordPositions()
{
    CompactLattice lat;
    int s0 = lat.AddState();
    int mid = lat.AddState();
    int sf = lat.AddState();
    lat.SetStart(s0);
    lat.SetFinal(sf, CompactLatticeWeight::One());
    AddWord(&lat, s0, mid, 1, 4, 6.0f, 6.0f);
    AddWord(&lat, mid, sf, 1, 4, 6.0f, 6.0f);
    AddWord(&lat, s0, sf, 1, 8, 30.0f, 30.0f);

    MinimumBayesRisk mbr(lat);
    fst::SymbolTable syms = MakeSyms();
    ExtractParams params;
    params.distinct_paths = 6;
    params.clock.base_sample = 0;
    params.clock.frame_offset = 0;
    params.clock.samples_per_frame = 480.0;

    Observation observation;
    partial_evidence::ExtractCandidates(lat, &mbr, &syms, params, &observation);

    std::vector<std::string> double_e;
    double_e.push_back("e");
    double_e.push_back("e");
    const Candidate *rep = FindSequence(observation, double_e);
    Check(rep != nullptr, "repeated word in distinct positions survives as its own reading");
    if (rep != nullptr) {
        Check(rep->words[0].start_sample_known && rep->words[1].start_sample_known,
              "repeated word positions carry sample timing");
        Check(rep->words[1].start_sample > rep->words[0].start_sample,
              "the second repeat begins after the first");
    }
}

// [[rr:FVP-6]]
static void ExtractionBoundTruncates()
{
    CompactLattice lat;
    int s0 = lat.AddState();
    int sf = lat.AddState();
    lat.SetStart(s0);
    lat.SetFinal(sf, CompactLatticeWeight::One());
    for (int i = 0; i < 25; i++) {
        AddWord(&lat, s0, sf, 1, 8, 10.0f + static_cast<float>(i) * 0.1f, 0.0f);
    }

    MinimumBayesRisk mbr(lat);
    fst::SymbolTable syms = MakeSyms();
    ExtractParams params;
    params.distinct_paths = 2;
    params.clock.samples_per_frame = 480.0;

    Observation observation;
    partial_evidence::ExtractCandidates(lat, &mbr, &syms, params, &observation);

    Check(observation.truncated,
          "the ten-times path bound reports truncation when distinct readings run short");
}

int main()
{
    EmptyPrimaryAndDistinctRivals();
    RepeatedWordPositions();
    ExtractionBoundTruncates();

    if (!failure.empty()) {
        std::fprintf(stderr, "%s\n", failure.c_str());
        return 1;
    }
    std::printf("test_candidates: 3 cases green\n");
    return 0;
}
