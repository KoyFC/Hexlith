#include "domain/analysis/provenance.h"

namespace hexlith::domain::analysis {

std::string_view Describe(Provenance source) {
    switch (source) {
    case Provenance::CrsExport:
        return "crs-export";
    case Provenance::ExHeader:
        return "exheader";
    case Provenance::Relocation:
        return "relocation";
    case Provenance::Ghidra:
        return "ghidra";
    case Provenance::Heuristic:
        return "heuristic";
    case Provenance::Manual:
        return "manual";
    }
    return "unknown";
}

std::optional<Provenance> ParseProvenance(std::string_view text) {
    if (text == "crs-export") {
        return Provenance::CrsExport;
    }
    if (text == "exheader") {
        return Provenance::ExHeader;
    }
    if (text == "relocation") {
        return Provenance::Relocation;
    }
    if (text == "ghidra") {
        return Provenance::Ghidra;
    }
    if (text == "heuristic") {
        return Provenance::Heuristic;
    }
    if (text == "manual") {
        return Provenance::Manual;
    }
    return std::nullopt;
}

bool IsDerivedFromTheImage(Provenance source) {
    switch (source) {
    case Provenance::CrsExport:
    case Provenance::ExHeader:
    case Provenance::Relocation:
    case Provenance::Heuristic:
        return true;
    case Provenance::Ghidra:
    case Provenance::Manual:
        return false;
    }
    return false;
}

}  // namespace hexlith::domain::analysis
