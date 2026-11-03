#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

/*
 * pmux_recode: 把「select 全部来自同一总线的比较」的 $pmux 改写成按总线位索引的
 * $mux 树，从而消掉整组比较器、共享解码。
 *
 * 支持的 select 形态：
 *   $eq(bus, pat)      pat 可以是全常量，也可以带变量位（如 {3'000, w}）
 *   $logic_not(bus)    == eq(bus, 0)
 *   $reduce_and(bus)   == eq(bus, 全 1)
 *
 * 语义（$pmux 映射成 mux 链时，下标大的分支优先）：
 *   对总线的每个取值 v，从默认值 A 开始按下标升序折叠：
 *     - pattern 全常量且等于 v   → 该分支数据（条件恒真，不加 mux）
 *     - pattern 带变量位且与 v 相容 → mux(cond_i, data_i, cur)，cond_i 是原比较器输出
 *   这样既保留优先级语义，也正确处理「常量带变量位」的分支
 *   （test1: eq(state, {3'000, w})，v=0/1 时由 w 决定）。
 *
 * 级联保护：若某候选的数据端来自另一个候选的输出（逐级依赖的 mux 链），跳过该候选，
 * 避免把链变成深树（实测 pmux_case04_feedback 会多 55 个 LUT）。
 */

struct PmuxRecodePass : public Pass {
    PmuxRecodePass() : Pass("pmux_recode", "rewrite const-decode $pmux into a $mux tree") {}

    void help() override {
        log("  -min-coverage <P>  only rewrite when 2^W covers at least P%% of branches (default 50)\n");
        log("\n");
        log("    pmux_recode [options] [selection]\n");
        log("\n");
        log("Rewrites $pmux cells whose select bits all come from comparisons of the same\n");
        log("bus against a (possibly partially constant) pattern, into a $mux tree indexed\n");
        log("by the bus bits. Priority follows the original $pmux branch order.\n");
        log("\n");
        log("  -max-width <N>     only rewrite buses with at most N bits (default 6)\n");
        log("  -max-branches <N>  only rewrite $pmux with at most N branches (default 16)\n");
        log("  -max-chain <N>     only rewrite $pmux whose candidate dependency depth is < N (default 1)\n");
        log("\n");
    }

    struct Branch {
        RTLIL::Const pat;   // Sx 表示该位是变量
        SigSpec data;
        SigSpec cond;       // 原比较器输出（1 bit）
    };

    static void collect_drivers(Module *module, SigMap &sigmap, dict<SigBit, Cell*> &drv) {
        for (auto cell : module->cells())
            for (auto &conn : cell->connections())
                if (cell->output(conn.first))
                    for (auto bit : sigmap(conn.second))
                        if (bit.wire && !drv.count(bit))
                            drv[bit] = cell;
    }

    static RTLIL::Const to_pattern(const SigSpec &sig) {
        RTLIL::Const pat(RTLIL::State::Sx, GetSize(sig));
        for (int i = 0; i < GetSize(sig); i++)
            if (!sig[i].wire)
                pat.bits()[i] = sig[i].data;
        return pat;
    }

    static bool pattern_is_const(RTLIL::Const &pat) {
        for (auto s : pat.bits())
            if (s != RTLIL::State::S0 && s != RTLIL::State::S1)
                return false;
        return true;
    }

    static bool pattern_matches(RTLIL::Const &pat, int v) {
        for (int i = 0; i < pat.size(); i++) {
            auto s = pat[i];
            if (s == RTLIL::State::S0 && ((v >> i) & 1)) return false;
            if (s == RTLIL::State::S1 && !((v >> i) & 1)) return false;
        }
        return true;
    }

    static bool decode_sel(Cell *cell, SigMap &sigmap, SigSpec &bus,
                           RTLIL::Const &pat, SigSpec &cond) {
        if (cell->type == ID($logic_not)) {
            bus = sigmap(cell->getPort(ID(A)));
            pat = RTLIL::Const(0, GetSize(bus));
        } else if (cell->type == ID($reduce_and)) {
            bus = sigmap(cell->getPort(ID(A)));
            pat = RTLIL::Const(RTLIL::State::S1, GetSize(bus));
        } else if (cell->type == ID($eq)) {
            SigSpec A = sigmap(cell->getPort(ID(A)));
            SigSpec B = sigmap(cell->getPort(ID(B)));
            bool a_wire = true, b_wire = true;
            for (auto b : A) if (!b.wire) { a_wire = false; break; }
            for (auto b : B) if (!b.wire) { b_wire = false; break; }
            if (a_wire && !b_wire)      { bus = A; pat = to_pattern(B); }
            else if (b_wire && !a_wire) { bus = B; pat = to_pattern(A); }
            else                        return false;
        } else {
            return false;
        }
        for (auto b : bus)
            if (!b.wire) return false;
        if (GetSize(pat) < GetSize(bus))
            pat.bits().resize(GetSize(bus), RTLIL::State::S0);
        if (GetSize(pat) > GetSize(bus))
            return false;
        cond = cell->getPort(ID(Y));
        return true;
    }
    void execute(std::vector<std::string> args, Design *design) override {
        int max_width = 6, max_branches = 16, max_chain = 2, min_cov = 50, mode = 3;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-max-width" && argidx + 1 < args.size()) {
                max_width = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-max-branches" && argidx + 1 < args.size()) {
                max_branches = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-max-chain" && argidx + 1 < args.size()) {
                max_chain = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-mode" && argidx + 1 < args.size()) {
                std::string m = args[++argidx];
                mode = (m == "outer") ? 1 : (m == "all") ? 2 : 0;
                continue;
            }
            if (args[argidx] == "-min-coverage" && argidx + 1 < args.size()) {
                min_cov = atoi(args[++argidx].c_str());
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        int total = 0, skipped_cascade = 0;
        for (auto module : design->selected_modules()) {
            SigMap sigmap(module);
            dict<SigBit, Cell*> drv;
            collect_drivers(module, sigmap, drv);

            struct Cand {
                Cell *cell;
                SigSpec bus;
                std::vector<Branch> branches;
            };
            std::vector<Cand> cands;

            for (auto cell : module->cells()) {
                if (cell->type != ID($pmux) && cell->type != ID($safe_pmux)) continue;
                SigSpec S = sigmap(cell->getPort(ID(S)));
                SigSpec B = cell->getPort(ID(B));
                SigSpec A = cell->getPort(ID(A));
                int width = GetSize(A);
                if (width == 0 || GetSize(S) < 2 || GetSize(S) > max_branches) continue;
                if (GetSize(B) != GetSize(S) * width) continue;

                Cand c;
                c.cell = cell;
                bool ok = true;
                for (int i = 0; i < GetSize(S) && ok; i++) {
                    if (!S[i].wire) { ok = false; break; }
                    auto it = drv.find(S[i]);
                    if (it == drv.end()) { ok = false; break; }
                    SigSpec bus, cond;
                    RTLIL::Const pat;
                    if (!decode_sel(it->second, sigmap, bus, pat, cond)) { ok = false; break; }
                    if (GetSize(bus) > max_width) { ok = false; break; }
                    if (i == 0)
                        c.bus = bus;
                    else if (bus != c.bus) { ok = false; break; }
                    Branch br;
                    br.pat = pat;
                    br.cond = sigmap(cond);
                    br.data = B.extract(i * width, width);
                    c.branches.push_back(br);
                }
                // 覆盖率门限：树要 2^W-1 个 mux，稀疏 select 上不如保留比较器
                if ((1 << GetSize(c.bus)) * 100 < min_cov * GetSize(c.branches)) continue;
                if (!ok || c.bus.empty() || GetSize(c.branches) != GetSize(S)) continue;
                cands.push_back(c);
            }
            if (cands.size() < 2) continue;

            dict<SigBit, bool> cand_out;
            for (auto &c : cands)
                for (auto bit : sigmap(c.cell->getPort(ID(Y))))
                    if (bit.wire) cand_out[bit] = true;

            // 依赖关系：cand 的数据端来自另一个 cand 的输出
            dict<Cell*, std::vector<Cell*>> deps;
            dict<Cell*, bool> is_cand;
            for (auto &c : cands) is_cand[c.cell] = true;
            for (auto &c : cands) {
                dict<Cell*, bool> seen;
                for (auto bit : sigmap(c.cell->getPort(ID(B)))) {
                    if (!bit.wire) continue;
                    if (!cand_out.count(bit)) continue;
                    auto d = drv.find(bit);
                    if (d == drv.end() || d->second == c.cell) continue;
                    if (!is_cand.count(d->second) || seen.count(d->second)) continue;
                    seen[d->second] = true;
                    deps[c.cell].push_back(d->second);
                }
            }

            // 链深：0 = 不依赖任何候选。逐级改写会让 mux 链变成深树，
            // 实测 pmux_case04_feedback 全量改写会多 55 个 LUT，所以限制深度。
            dict<Cell*, int> depth;
            std::function<int(Cell*)> calc = [&](Cell *c) -> int {
                if (depth.count(c)) return depth.at(c);
                depth[c] = 0;
                int d = 0;
                auto it = deps.find(c);
                if (it != deps.end())
                    for (auto p : it->second)
                        d = std::max(d, calc(p) + 1);
                depth[c] = d;
                return d;
            };

            // 反向边：有多少候选依赖它
            dict<Cell*, int> users;
            for (auto &kv : deps)
                for (auto p : kv.second)
                    users[p]++;

            for (auto &c : cands) {
                bool take;
                int dep = calc(c.cell);
                if (mode == 6)      take = (dep >= 2);                  // 只改更深层
                else if (mode == 5) take = (dep % 2 == 1);              // 只改奇数层
                else if (mode == 4) take = (dep % 2 == 0);              // 只改偶数层（不叠树）
                else if (mode == 3) take = (dep < max_chain);           // 按链深放行
                else if (mode == 2) take = true;                        // 全部改写
                else if (mode == 1) take = (users.count(c.cell) == 0);  // 只改写链尾
                else                take = (dep == 0);                  // 只改写链首
                if (!take) { skipped_cascade++; continue; }
                SigSpec A = c.cell->getPort(ID(A));
                int width = GetSize(A);
                int nvals = 1 << GetSize(c.bus);   // 每个候选按自己的总线宽度展开

                dict<int, SigSpec> leaf;
                for (int v = 0; v < nvals; v++) {
                    SigSpec cur = A;
                    for (auto &br : c.branches) {
                        if (!pattern_matches(br.pat, v)) continue;
                        if (pattern_is_const(br.pat)) {
                            cur = br.data;
                        } else {
                            SigSpec y = module->addWire(NEW_ID, width);
                            module->addMux(NEW_ID, cur, br.data, br.cond, y);
                            cur = y;
                        }
                    }
                    leaf[v] = cur;
                }

                std::function<SigSpec(int, int)> rec = [&](int bit, int value) -> SigSpec {
                    if (bit == GetSize(c.bus))
                        return leaf.at(value);
                    SigSpec lo = rec(bit + 1, value);
                    SigSpec hi = rec(bit + 1, value | (1 << bit));
                    SigSpec y = module->addWire(NEW_ID, width);
                    module->addMux(NEW_ID, lo, hi, SigSpec(c.bus[bit]), y);
                    return y;
                };
                SigSpec tree = rec(0, 0);
                module->connect(c.cell->getPort(ID(Y)), tree);
                module->remove(c.cell);
                total++;
            }
        }

        log("  pmux_recode: rewrote %d const-decode $pmux, skipped %d cascaded one(s).\n",
            total, skipped_cascade);
    }
} PmuxRecodePass;

PRIVATE_NAMESPACE_END
