#pragma once

#include "resume_proof.hpp"
#include "resume_refill.hpp"

#include <tuple>
#include <variant>

namespace m3 {

enum class ResumeSchedule { round_robin, staged };

struct SearchBudget {
    uint64_t candidates, refill_nodes, states;
};

class ResumeSolver;
void write_checkpoint(std::ostream&, const ResumeSolver&);
ResumeSolver read_checkpoint(std::istream&, const State&, const Rules&, const Options&);

class ResumeSolver {
    friend struct ResumeCheckpoint;
    friend void write_checkpoint(std::ostream&, const ResumeSolver&);
    friend ResumeSolver read_checkpoint(std::istream&, const State&, const Rules&, const Options&);
    static constexpr size_t no_node = std::numeric_limits<size_t>::max();

    struct Move { Swap swap; Board board; Matches hits; };
    struct Outcome {
        Status status;
        size_t node = no_node;
        std::vector<std::vector<uint8_t>> batches{};
    };
    enum class Phase { enter, initialize, enumerate, candidate, await_child };
    struct StableFrame {
        State state;
        Phase phase = Phase::enter;
        std::vector<Move> moves{};
        size_t next{};
        bool unknown{};
        std::vector<Edge> edges{};
    };
    struct RefillFrame {
        Board board;
        Matches hits;
        int goal, moves, wave;
        bool unknown{}, stable_tier{}, prefix_reached{};
        Phase phase = Phase::enter;
        std::optional<Falling> falling{};
        std::optional<RefillCursor> cursor{};
        std::vector<std::vector<uint8_t>> stable_tried{};
        std::vector<uint8_t> batch{};
        std::optional<Board> filled{};
        Matches next_hits{};
        uint64_t tried{};
    };
    using Frame = std::variant<StableFrame, RefillFrame>;
    struct RootTask {
        Swap action;
        std::vector<Frame> stack{};
        std::optional<Outcome> outcome{};
        FirstSwapStats stats{};
    };
    struct Key {
        std::string cells;
        int goal, moves;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const {
            return std::hash<std::string>{}(key.cells) ^
                   (static_cast<size_t>(key.goal) * 0x9e3779b9u) ^
                   (static_cast<size_t>(key.moves) * 0x85ebca6bu);
        }
    };
    struct Known {
        Status status;
        size_t node = no_node;
        std::optional<Swap> losing_action{};
    };

public:
    ResumeSolver(Rules rules = {}, Options options = {}) : rules_(rules), options_(options) {}

    void start(const State& initial, const ProofBank* imported = nullptr) {
        initial_ = initial;
        roots_.clear();
        known_.clear();
        successors_.clear();
        certificate_ = {};
        imported_failures_.clear();
        metrics_ = {};
        next_root_ = stalled_ = 0;
        result_.reset();
        ++metrics_.calls;
        if (initial.goal == 0) {
            result_ = make_win(initial, {});
            return;
        }
        if (initial.moves == 0) {
            result_ = Outcome{Status::fail};
            return;
        }
        if (options_.max_states == 0) {
            ++metrics_.state_cutoffs;
            ++metrics_.complexity_cutoffs;
            result_ = Outcome{Status::unknown};
            return;
        }
        ++metrics_.states;
        for (auto& move : moves(initial.board)) {
            RootTask task{move.swap};
            task.stats.swap = move.swap;
            task.stack.emplace_back(RefillFrame{std::move(move.board), std::move(move.hits),
                                                initial.goal, initial.moves - 1, 0});
            roots_.push_back(std::move(task));
        }
        if (roots_.empty()) result_ = Outcome{Status::fail};
        if (imported) import_bank(*imported);
        if (!result_) settle();
    }

    // Stops at a quantum boundary, so each unfinished stack can be persisted.
    void run(size_t quanta = std::numeric_limits<size_t>::max()) {
        require_started();
        constexpr SearchBudget quantum{256, 4096, 64};
        while (!result_ && quanta > 0) {
            auto& task = roots_[next_root_];
            next_root_ = (next_root_ + 1) % roots_.size();
            if (task.outcome) continue;
            const auto before = usage();
            advance(task, quantum);
            --quanta;
            stalled_ = (task.outcome || before != usage()) ? 0 : stalled_ + 1;
            settle();
            if (!result_ && stalled_ >= pending_count()) result_ = Outcome{Status::unknown};
        }
    }

    void run_staged() {
        require_started();
        constexpr std::array<uint64_t, 4> targets{256, 2048, 8192, 32768};
        for (const auto target : targets) {
            for (auto& task : roots_) {
                if (result_) return;
                if (task.outcome || task.stats.candidates >= target || remaining().candidates == 0) continue;
                auto slice = remaining();
                slice.candidates = std::min(slice.candidates, target - task.stats.candidates);
                advance(task, slice);
                settle();
            }
        }
        auto pending = pending_count();
        for (auto& task : roots_) {
            if (result_) return;
            if (task.outcome) continue;
            auto slice = remaining();
            if (slice.candidates == 0) break;
            slice.candidates = slice.candidates / pending + (slice.candidates % pending != 0);
            advance(task, slice);
            --pending;
            settle();
        }
        if (!result_) result_ = Outcome{Status::unknown};
    }

    bool done() const { return result_.has_value(); }

    SolveResult result(std::vector<FirstSwapStats>* first_swaps = nullptr) const {
        require_started();
        if (first_swaps) {
            first_swaps->clear();
            for (const auto& task : roots_) first_swaps->push_back(task.stats);
        }
        auto stats = metrics_;
        stats.memo_entries = options_.memo ? known_.size() : 0;
        if (result_ && result_->status == Status::win) {
            return {Status::win, compact(result_->node), stats};
        }
        return {result_ ? result_->status : Status::unknown, {}, stats};
    }

    SolveResult solve(const State& initial, ResumeSchedule schedule, std::vector<FirstSwapStats>* first_swaps = nullptr,
                      const ProofBank* imported = nullptr) {
        start(initial, imported);
        if (schedule == ResumeSchedule::staged) run_staged();
        else run();
        return result(first_swaps);
    }

    ProofBank proofs() const {
        require_started();
        ProofBank bank{*initial_, rules_, {}, {}};
        for (const auto& task : roots_) {
            if (!task.outcome) continue;
            if (task.outcome->status == Status::win) {
                bank.wins.push_back({task.action, task.outcome->batches, compact(task.outcome->node)});
            } else if (task.outcome->status == Status::fail) {
                if (const auto existing = std::ranges::find(imported_failures_, task.action, &RootFailureProof::action);
                    existing != imported_failures_.end()) {
                    bank.fails.push_back(*existing);
                    continue;
                }
                RootFailureProof proof{task.action, {}};
                for (const auto& [key, known] : known_) {
                    if (known.status == Status::fail) proof.states.push_back({from_key(key), known.losing_action});
                }
                std::ranges::sort(proof.states, [](const auto& a, const auto& b) {
                    const auto& x = a.state;
                    const auto& y = b.state;
                    return std::tie(x.moves, x.goal, x.board.cells) < std::tie(y.moves, y.goal, y.board.cells);
                });
                bank.fails.push_back(std::move(proof));
            }
        }
        return bank;
    }

private:
    void require_started() const {
        if (!initial_) throw std::logic_error("resume search has not been started");
    }

    static Key key_of(const State& state) {
        return {std::string(state.board.cells.begin(), state.board.cells.end()), state.goal, state.moves};
    }

    State from_key(const Key& key) const {
        auto board = initial_->board;
        board.cells.assign(key.cells.begin(), key.cells.end());
        return {std::move(board), key.goal, key.moves};
    }

    std::array<uint64_t, 3> usage() const {
        return {metrics_.candidates, metrics_.refill.nodes, metrics_.states};
    }

    SearchBudget remaining() const {
        return {options_.max_candidates - metrics_.candidates, options_.max_refill_nodes - metrics_.refill.nodes,
                options_.max_states - metrics_.states};
    }

    size_t pending_count() const {
        return static_cast<size_t>(std::ranges::count_if(roots_, [](const RootTask& task) { return !task.outcome; }));
    }

    Matches match_after_edit(const Board& board, const std::vector<int>& changed) const {
        return options_.incremental ? dirty_matches(board, changed) : full_matches(board);
    }

    std::vector<Move> moves(const Board& board) const {
        std::vector<Move> result;
        for (const auto swap : options_.incremental ? legal_swaps(board) : reference_swaps(board)) {
            auto swapped = board;
            std::swap(swapped.cells[swap.a], swapped.cells[swap.b]);
            auto hits = match_after_edit(swapped, {swap.a, swap.b});
            result.push_back({swap, std::move(swapped), std::move(hits)});
        }
        if (options_.ranked) {
            const auto rank = [&](const Move& move) {
                const auto count = static_cast<int>(move.hits.size());
                return count > rules_.refill_cap ? 0 : count;
            };
            std::stable_sort(result.begin(), result.end(), [&](const Move& a, const Move& b) {
                return rank(a) < rank(b);
            });
        }
        return result;
    }

    Outcome make_win(const State& state, std::vector<Edge> edges) {
        const auto index = certificate_.nodes.size();
        certificate_.nodes.push_back({state, std::move(edges)});
        return {Status::win, index};
    }

    Certificate compact(size_t root) const {
        Certificate result;
        std::vector<size_t> mapping(certificate_.nodes.size(), no_node);
        auto visit = [&](auto&& self, size_t index) -> size_t {
            if (mapping[index] != no_node) return mapping[index];
            auto node = certificate_.nodes[index];
            for (auto& edge : node.edges) edge.child = self(self, edge.child);
            mapping[index] = result.nodes.size();
            result.nodes.push_back(std::move(node));
            return mapping[index];
        };
        result.root = visit(visit, root);
        return result;
    }

    void settle() {
        if (result_) return;
        bool unresolved = false;
        std::vector<Edge> edges;
        for (const auto& root : roots_) {
            if (!root.outcome) return;
            const auto& proof = *root.outcome;
            unresolved |= proof.status == Status::unknown;
        }
        if (unresolved) {
            result_ = Outcome{Status::unknown};
            return;
        }
        for (const auto& root : roots_) edges.push_back({root.action, root.outcome->batches, root.outcome->node});
        result_ = make_win(*initial_, std::move(edges));
    }

    void import_bank(const ProofBank& bank) {
        if (bank.initial != *initial_ || bank.rules.refill_cap != rules_.refill_cap ||
            bank.rules.cascade_budget != rules_.cascade_budget) {
            throw std::invalid_argument("proof bank board or rules differ from current search");
        }
        auto lookup = [&](Swap action) -> RootTask& {
            auto found = std::ranges::find(roots_, action, &RootTask::action);
            if (found == roots_.end() || found->outcome) throw std::invalid_argument("duplicate or illegal root proof");
            return *found;
        };
        for (const auto& proof : bank.wins) {
            auto& task = lookup(proof.action);
            if (!verify_root_witness(*initial_, rules_, proof)) throw std::invalid_argument("invalid root witness");
            const auto offset = certificate_.nodes.size();
            for (auto node : proof.continuation.nodes) {
                for (auto& edge : node.edges) edge.child += offset;
                certificate_.nodes.push_back(std::move(node));
            }
            task.outcome = Outcome{Status::win, offset + proof.continuation.root, proof.refills};
            task.stats.status = Status::win;
            task.stack.clear();
        }
        for (const auto& proof : bank.fails) {
            auto& task = lookup(proof.action);
            if (!verify_root_failure(*initial_, rules_, proof)) throw std::invalid_argument("invalid root failure proof");
            task.outcome = Outcome{Status::fail};
            task.stats.status = Status::fail;
            task.stack.clear();
            imported_failures_.push_back(proof);
            result_ = Outcome{Status::fail};
        }
    }

    void account(RootTask& task, const Metrics& before) {
        auto& stat = task.stats;
        if (!stat.status) {
            stat.candidate_exhausted_on_entry = before.candidates == options_.max_candidates;
            stat.refill_exhausted_on_entry = before.refill.nodes == options_.max_refill_nodes;
            stat.state_exhausted_on_entry = before.states == options_.max_states;
        }
        stat.status = task.outcome ? task.outcome->status : Status::unknown;
        stat.candidates += metrics_.candidates - before.candidates;
        stat.refill_nodes += metrics_.refill.nodes - before.refill.nodes;
        stat.states += metrics_.states - before.states;
        stat.candidate_cutoffs += metrics_.candidate_cutoffs - before.candidate_cutoffs;
        stat.refill_cutoffs += metrics_.refill.node_cutoffs - before.refill.node_cutoffs;
        stat.state_cutoffs += metrics_.state_cutoffs - before.state_cutoffs;
        stat.cascade_cutoffs += metrics_.cascade_cutoffs - before.cascade_cutoffs;
        stat.beam_cutoffs += metrics_.beam_cutoffs - before.beam_cutoffs;
    }

    void advance(RootTask& task, SearchBudget slice) {
        const auto before = metrics_;
        const auto available = remaining();
        const SearchBudget limits{before.candidates + std::min(available.candidates, slice.candidates),
                                  before.refill.nodes + std::min(available.refill_nodes, slice.refill_nodes),
                                  before.states + std::min(available.states, slice.states)};
        while (!task.outcome) {
            const bool progressed = std::holds_alternative<StableFrame>(task.stack.back()) ?
                step_stable(task, limits) : step_refill(task, limits);
            if (!progressed) break;
        }
        account(task, before);
    }

    void next_cursor(RefillFrame& frame, bool stable) const {
        frame.stable_tier = stable;
        frame.cursor.emplace(frame.falling->board, frame.falling->holes,
                             balanced_quota(frame.board.colors, static_cast<int>(frame.hits.size())),
                             stable ? RefillMode::csp : RefillMode::quota, stable, options_.ranked);
    }

    bool step_stable(RootTask& task, const SearchBudget& limits) {
        auto& frame = std::get<StableFrame>(task.stack.back());
        if (frame.phase == Phase::enter) {
            ++metrics_.calls;
            frame.phase = Phase::initialize;
        }
        if (frame.phase == Phase::initialize) {
            if (frame.state.goal == 0) {
                deliver(task, make_win(frame.state, {}));
                return true;
            }
            if (frame.state.moves == 0) {
                deliver(task, {Status::fail});
                return true;
            }
            if (options_.memo) {
                ++metrics_.memo_lookups;
                if (const auto found = known_.find(key_of(frame.state)); found != known_.end()) {
                    ++metrics_.memo_hits;
                    deliver(task, {found->second.status, found->second.node});
                    return true;
                }
            }
            if (metrics_.states >= limits.states) {
                ++metrics_.state_cutoffs;
                ++metrics_.complexity_cutoffs;
                return false;
            }
            ++metrics_.states;
            const auto depth = static_cast<uint64_t>(initial_->moves - frame.state.moves);
            metrics_.depth_sum += depth;
            metrics_.max_depth = std::max(metrics_.max_depth, depth);
            frame.moves = moves(frame.state.board);
            if (frame.moves.empty()) {
                known_.emplace(key_of(frame.state), Known{Status::fail});
                deliver(task, {Status::fail});
                return true;
            }
            frame.phase = Phase::enumerate;
        }
        if (frame.next == frame.moves.size()) {
            if (frame.unknown) {
                deliver(task, {Status::unknown});
            } else {
                auto win = make_win(frame.state, std::move(frame.edges));
                known_.emplace(key_of(frame.state), Known{Status::win, win.node});
                deliver(task, std::move(win));
            }
            return true;
        }
        frame.phase = Phase::await_child;
        const auto& move = frame.moves[frame.next];
        task.stack.emplace_back(RefillFrame{move.board, move.hits, frame.state.goal, frame.state.moves - 1, 0});
        return true;
    }

    bool step_refill(RootTask& task, const SearchBudget& limits) {
        auto& frame = std::get<RefillFrame>(task.stack.back());
        if (frame.phase == Phase::enter) {
            if (frame.hits.empty()) {
                auto stable = State{std::move(frame.board), frame.goal, frame.moves};
                if (options_.successor_dedup) {
                    ++metrics_.successor_visits;
                    const auto [entry, inserted] = successors_.try_emplace(key_of(stable), std::nullopt);
                    metrics_.successor_classes += inserted;
                    if (entry->second) {
                        ++metrics_.successor_reuses;
                        deliver(task, {entry->second->status, entry->second->node});
                        return true;
                    }
                }
                task.stack.back().emplace<StableFrame>(StableFrame{std::move(stable)});
                return true;
            }
            if (static_cast<int>(frame.hits.size()) > rules_.refill_cap) {
                ++metrics_.over_cap;
                deliver(task, {Status::fail});
                return true;
            }
            if (frame.wave >= rules_.cascade_budget) {
                ++metrics_.cascade_cutoffs;
                deliver(task, {Status::unknown});
                return true;
            }
            ++metrics_.refill_states;
            metrics_.max_clear_waves = std::max(metrics_.max_clear_waves, static_cast<uint64_t>(frame.wave + 1));
            metrics_.max_tested_batch = std::max(metrics_.max_tested_batch, static_cast<uint64_t>(frame.hits.size()));
            frame.falling = clear_and_drop(frame.board, frame.hits);
            next_cursor(frame, options_.csp);
            frame.phase = Phase::enumerate;
        }
        if (frame.phase == Phase::enumerate) {
            switch (frame.cursor->next(metrics_.refill, limits.refill_nodes)) {
                case CursorResult::paused: return false;
                case CursorResult::exhausted:
                    if (frame.stable_tier) next_cursor(frame, false);
                    else deliver(task, {frame.unknown ? Status::unknown : Status::fail});
                    return true;
                case CursorResult::candidate: break;
            }
            const auto& values = frame.cursor->values();
            if (frame.stable_tier && options_.stable_prefix && frame.stable_tried.size() >= options_.stable_prefix) {
                frame.prefix_reached = true;
                next_cursor(frame, false);
                return true;
            }
            if (!frame.stable_tier && frame.prefix_reached &&
                std::ranges::find(frame.stable_tried, values) != frame.stable_tried.end()) return true;
            auto filled = frame.falling->board;
            for (size_t i = 0; i < values.size(); ++i) filled.cells[frame.falling->holes[i]] = values[i];
            auto matches = frame.stable_tier ? Matches{} : match_after_edit(filled, frame.falling->changed);
            if (!frame.stable_tier && options_.csp && !frame.prefix_reached && matches.empty()) return true;
            if (frame.stable_tier && options_.stable_prefix) frame.stable_tried.push_back(values);
            frame.batch = values;
            frame.filled = std::move(filled);
            frame.next_hits = std::move(matches);
            frame.phase = Phase::candidate;
        }
        if (options_.beam_width && frame.tried >= options_.beam_width) {
            ++metrics_.beam_cutoffs;
            deliver(task, {Status::unknown});
            return true;
        }
        if (metrics_.candidates >= limits.candidates) {
            ++metrics_.candidate_cutoffs;
            ++metrics_.complexity_cutoffs;
            return false;
        }
        ++metrics_.candidates;
        ++frame.tried;
        frame.phase = Phase::await_child;
        auto child = RefillFrame{std::move(*frame.filled), std::move(frame.next_hits),
                                 std::max(0, frame.goal - static_cast<int>(frame.hits.size())),
                                 frame.moves, frame.wave + 1};
        frame.filled.reset();
        task.stack.emplace_back(std::move(child));
        return true;
    }

    void deliver(RootTask& task, Outcome outcome) {
        while (true) {
            if (options_.successor_dedup && outcome.status != Status::unknown) {
                if (const auto* stable = std::get_if<StableFrame>(&task.stack.back())) {
                    successors_.at(key_of(stable->state)) = Known{outcome.status, outcome.node};
                }
            }
            task.stack.pop_back();
            if (task.stack.empty()) {
                if (outcome.status == Status::fail) result_ = Outcome{Status::fail};
                task.outcome = std::move(outcome);
                return;
            }
            if (auto* parent = std::get_if<StableFrame>(&task.stack.back())) {
                const auto swap = parent->moves[parent->next].swap;
                if (outcome.status == Status::fail) {
                    known_.emplace(key_of(parent->state), Known{Status::fail, no_node, swap});
                    continue;
                }
                if (outcome.status == Status::win) {
                    parent->edges.push_back({swap, std::move(outcome.batches), outcome.node});
                } else {
                    parent->unknown = true;
                }
                ++parent->next;
                parent->phase = Phase::enumerate;
                return;
            }
            auto& parent = std::get<RefillFrame>(task.stack.back());
            if (outcome.status == Status::win) {
                outcome.batches.insert(outcome.batches.begin(), std::move(parent.batch));
                continue;
            }
            parent.unknown |= outcome.status == Status::unknown;
            parent.batch.clear();
            parent.phase = Phase::enumerate;
            return;
        }
    }

    Rules rules_;
    Options options_;
    std::optional<State> initial_;
    std::vector<RootTask> roots_;
    Metrics metrics_;
    Certificate certificate_;
    std::unordered_map<Key, Known, KeyHash> known_;
    std::unordered_map<Key, std::optional<Known>, KeyHash> successors_;
    std::vector<RootFailureProof> imported_failures_;
    std::optional<Outcome> result_;
    size_t next_root_ = 0, stalled_ = 0;
};

} // namespace m3
