#include "match3_strict/solver.hpp"
#include "detail/verify.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace strict = match3_strict;

void ensure(bool value, std::string_view message)
{
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

int nibble(char symbol)
{
    if (symbol >= '0' && symbol <= '9') {
        return symbol - '0';
    }
    ensure(symbol >= 'a' && symbol <= 'f', "invalid hexadecimal value");
    return symbol - 'a' + 10;
}

std::vector<std::uint8_t> decode(std::string_view encoded, int colors)
{
    ensure(encoded.size() % 2 == 0, "odd hexadecimal byte length");
    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t position = 0; position < encoded.size(); position += 2) {
        const auto tile = nibble(encoded[position]) * 16 + nibble(encoded[position + 1]);
        ensure(tile < colors, "tile value out of palette");
        decoded.push_back(static_cast<std::uint8_t>(tile));
    }
    return decoded;
}

struct Record {
    std::string level;
    std::size_t ordinal;
    m3::Rules rules;
    m3::State child;
    std::string child_key;
    std::string payload;
};

Record parse_record(std::istream& archive, const std::string& header)
{
    ensure(header.size() <= 4096, "proof bank header exceeds supported size");
    std::istringstream stream(header);
    std::string magic, level, parent_tiles, child_tiles;
    std::size_t ordinal = 0;
    int rows = 0, cols = 0, colors = 0, parent_goal = 0, parent_moves = 0, wave_count = 0;
    m3::Rules rules;
    m3::Edge transition{};
    ensure(static_cast<bool>(stream >> magic >> level >> ordinal >> rules.refill_cap >> rules.cascade_budget
                             >> rows >> cols >> colors >> parent_goal >> parent_moves >> parent_tiles
                             >> transition.swap.a >> transition.swap.b >> wave_count),
           "truncated proof bank header");
    ensure(magic == "M3PBK1" && !level.empty() && level.find_first_not_of(
               "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_") == std::string::npos,
           "unknown proof bank schema or unsafe level ID");
    ensure(rows >= 5 && rows <= 16 && cols >= 5 && cols <= 16 && colors >= 2 && colors <= 8,
           "invalid board dimensions or palette");
    ensure(rules.refill_cap > 0 && rules.refill_cap <= 16 && rules.cascade_budget > 0 && rules.cascade_budget <= 32,
           "invalid refill or cascade rule");
    ensure(parent_goal > 0 && parent_goal <= 1'000'000 && parent_moves > 1 && parent_moves <= 20,
           "invalid nonterminal parent target");
    ensure(wave_count > 0 && wave_count <= rules.cascade_budget, "invalid refill wave count");

    for (int i = 0; i < wave_count; ++i) {
        std::string wave;
        ensure(static_cast<bool>(stream >> wave) && !wave.empty() &&
                   wave.size() <= 2 * static_cast<std::size_t>(rules.refill_cap),
               "invalid archived refill wave capacity");
        transition.refills.push_back(decode(wave, colors));
    }
    int child_goal = 0, child_moves = 0;
    std::size_t proof_bytes = 0;
    ensure(static_cast<bool>(stream >> child_goal >> child_moves >> child_tiles >> proof_bytes),
           "truncated child proof frame");
    stream >> std::ws;
    ensure(stream.eof(), "unexpected proof bank header suffix");
    ensure(child_goal > 0 && child_goal < parent_goal && child_moves == parent_moves - 1,
           "invalid child target or remaining moves");
    ensure(proof_bytes > 0 && proof_bytes <= 32 * 1024 * 1024, "invalid proof payload length");

    m3::Board parent_board(rows, cols, colors), child_board(rows, cols, colors);
    parent_board.cells = decode(parent_tiles, colors);
    child_board.cells = decode(child_tiles, colors);
    ensure(m3::verification::stable_board(parent_board) && m3::verification::stable_board(child_board),
           "archive contains an unstable or malformed board");
    const auto swaps = m3::verification::swaps(parent_board);
    ensure(std::find(swaps.begin(), swaps.end(), transition.swap) != swaps.end(),
           "archived action is not a legal swap on the parent");

    std::string payload(proof_bytes, '\0');
    ensure(static_cast<bool>(archive.read(payload.data(), static_cast<std::streamsize>(proof_bytes))),
           "truncated certificate payload");
    ensure(payload.back() == '\n', "missing certificate framing newline");

    m3::State parent{std::move(parent_board), parent_goal, parent_moves};
    m3::State child{std::move(child_board), child_goal, child_moves};
    ensure(m3::verification::replay_edge(parent, rules, transition, child),
           "independent replay disagrees with archived successor");
    const strict::Problem problem{{rows, cols, colors, child.board.cells}, child_goal, child_moves, {rules.refill_cap}};
    ensure(strict::verify_certificate(problem, payload, rules.cascade_budget),
           "archived Strict certificate failed independent public verification");
    const auto domain = std::to_string(rows) + '/' + std::to_string(cols) + '/' + std::to_string(colors) + '/' +
                        std::to_string(rules.refill_cap) + '/' + std::to_string(rules.cascade_budget) + '/';
    return {std::move(level), ordinal, rules, std::move(child),
            domain + std::to_string(child_goal) + '/' + std::to_string(child_moves) + '/' + child_tiles,
            std::move(payload)};
}

void save_cli_sample(const Record& sample, const fs::path& samples)
{
    const auto name = sample.level + "-" + std::to_string(sample.ordinal);
    std::ofstream board_file(samples / (name + ".txt"), std::ios::binary);
    const auto& board = sample.child.board;
    for (int row = 0; row < board.rows; ++row) {
        for (int col = 0; col < board.cols; ++col) {
            board_file << static_cast<int>(board.cells.at(static_cast<std::size_t>(row * board.cols + col)))
                       << (col + 1 == board.cols ? '\n' : ' ');
        }
    }
    std::ofstream proof_file(samples / (name + ".m3cert"), std::ios::binary);
    proof_file.write(sample.payload.data(), static_cast<std::streamsize>(sample.payload.size()));
    board_file.close();
    proof_file.close();
    ensure(static_cast<bool>(board_file) && static_cast<bool>(proof_file), "could not write CLI audit sample");
}

void inspect_board(const fs::path& path, const fs::path& samples)
{
    const auto level = path.stem().string();
    const std::string err_prefix = "Bank for " + level + ": ";
    try {
        std::ifstream archive(path, std::ios::binary);
        ensure(static_cast<bool>(archive), "could not open proof bank");
        std::string header;
        std::unordered_set<std::size_t> ordinals;
        std::unordered_set<std::string> children;
        std::optional<Record> largest;
        std::size_t total_payload = 0;
        while (std::getline(archive, header)) {
            auto record = parse_record(archive, header);
            ensure(record.level == level && ordinals.insert(record.ordinal).second,
                   "archive file has wrong level or duplicate record ordinal");
            ensure(children.insert(record.child_key).second, "duplicate exact certified successor");
            total_payload += record.payload.size();
            if (!largest || largest->payload.size() < record.payload.size()) {
                largest = std::move(record);
            }
        }
        ensure(archive.eof() && !archive.bad(), "failed reading framed archive");
        std::cout << level << ',' << children.size() << ',' << total_payload << ',';
        if (largest) {
            save_cli_sample(*largest, samples);
            std::cout << largest->ordinal << ',' << largest->child.goal << ',' << largest->child.moves << ','
                      << largest->child.board.colors << ',' << largest->rules.refill_cap << ',' << largest->rules.cascade_budget;
        } else {
            std::cout << ",,,,,";
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        throw std::runtime_error(err_prefix + error.what());
    }
}

} // namespace

int main(int argc, char** argv) try {
    ensure(argc == 3, "usage: audit-proof-bank BANK_ENTRIES SAMPLE_OUTPUT");
    const fs::path archives = argv[1], samples = argv[2];
    ensure(fs::is_directory(archives), "proof bank archive directory missing");
    ensure(!fs::exists(samples), "CLI sample output directory must be new");
    fs::create_directories(samples);

    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(archives)) {
        ensure(entry.is_regular_file() && entry.path().extension() == ".m3bank", "unexpected bank directory entry");
        paths.push_back(entry.path());
    }
    ensure(!paths.empty(), "proof bank archive directory is empty");
    std::sort(paths.begin(), paths.end());
    std::cout << "id,entries,payload_bytes,sample_ordinal,sample_goal,sample_moves,sample_colors,sample_cap,sample_depth\n";
    for (const auto& path : paths) {
        inspect_board(path, samples);
    }
    ensure(static_cast<bool>(std::cout), "audit report write failed");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
