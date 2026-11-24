#pragma once

#include "detail/verify.hpp"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace proofbank_artifact {

inline std::string encode_bytes(const std::vector<std::uint8_t>& bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0xFU]);
    }
    return encoded;
}

// The caller exports only newly proved continuations. Each record independently
// binds the parent, chosen swap, all refill waves, and exactly one child proof.
template <class ProofRef>
void write_proven(std::ostream& output, std::string_view level_id, std::size_t expansion_number,
                  const m3::State& parent, const m3::Edge& natural_edge, const m3::Rules& rules,
                  ProofRef continuation)
{
    m3::verification::require(!level_id.empty() && level_id.find_first_not_of(
                                  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_") == std::string_view::npos,
                              "invalid proof bank level ID");
    m3::verification::require(continuation.certificate && continuation.index == continuation.certificate->root,
                              "proof bank entry must include a complete child certificate rooted at the successor");
    const auto& child = continuation.node().state;
    m3::verification::require(child.goal > 0 && child.moves > 0 && parent.board.rows == child.board.rows &&
                                  parent.board.cols == child.board.cols && parent.board.colors == child.board.colors,
                              "proof bank entry is not a playable successor of the same rule domain");

    std::ostringstream proof;
    m3::write_certificate(proof, rules, *continuation.certificate);
    const auto payload = std::move(proof).str();

    output << "M3PBK1 " << level_id << ' ' << expansion_number << ' ' << rules.refill_cap << ' '
           << rules.cascade_budget << ' ' << parent.board.rows << ' ' << parent.board.cols << ' '
           << parent.board.colors << ' ' << parent.goal << ' ' << parent.moves << ' ' << encode_bytes(parent.board.cells)
           << ' ' << natural_edge.swap.a << ' ' << natural_edge.swap.b << ' ' << natural_edge.refills.size();
    for (const auto& wave : natural_edge.refills) {
        output << ' ' << encode_bytes(wave);
    }
    output << ' ' << child.goal << ' ' << child.moves << ' ' << encode_bytes(child.board.cells) << ' ' << payload.size() << '\n';
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    m3::verification::require(static_cast<bool>(output), "could not append to proof bank archive");
}

} // namespace proofbank_artifact
