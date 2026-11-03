#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PmuxOptimizePass : public Pass {
    PmuxOptimizePass() : Pass("pmux_optimize", "Phase 3: post-decomposition optimization") {}

    void help() override {
        log("\n");
        log("    pmux_optimize [options] [selection]\n");
        log("\n");
        log("Phase 3 optimizations on Phase-2 decomposed PMUX structures.\n");
        log("All are ON by default; use -no-* flags to disable.\n");
        log("\n");
        log("  -no-cse            disable duplicate $reduce_or elimination\n");
        log("  -no-redundant-mux  disable redundant $mux removal\n");
        log("  -no-eq-simplify    disable $eq identity simplification\n");
        log("  -max-cells <N>     skip modules with more than N cells (default 50000)\n");
        log("\n");
        log("Both [3.1] and [3.2] apply to every $mux / $reduce_or in the module,\n");
        log("not only to cells annotated by pmux_recognize.\n");
        log("\n");
        log("$eq simplifications (all unconditionally safe):\n");
        log("    $eq(a, a)      -> 1\n");
        log("    $eq(a, 0)      -> ~(|a)\n");
        log("    $eq(a, all-1)  -> &a\n");
        log("\n");
    }

    // ============================================================
    // 3.1 冗余 MUX 消除：$mux(S, A, A) -> A
    // 对所有 $mux 生效（不再只限于带 pmux_id 标注的 cell）。
    // 两阶段：先收集，后统一 connect + remove
    // ============================================================
    int remove_redundant_mux(Module *module, SigMap &sigmap) {
        std::vector<std::pair<Cell*, SigSpec>> repls;

        for (auto cell : module->cells()) {
            if (cell->type != ID($mux)) continue;

            SigSpec A = sigmap(cell->getPort(ID(A)));
            SigSpec B = sigmap(cell->getPort(ID(B)));

            if (A == B)
                repls.push_back({cell, A});
        }

        for (auto &[c, val] : repls)
            module->connect(c->getPort(ID(Y)), val);
        for (auto &[c, _] : repls)
            module->remove(c);

        return (int)repls.size();
    }

    // ============================================================
    // 3.2 CSE：消除重复的 $reduce_or
    //
    // 用归一化后的 SigSpec 生成字符串 key，跨所有 $reduce_or 做 CSE
    // （不再只限于同一 pmux_id 分组，也不要求带 pmux_id 属性）。
    // 两阶段：先 collect 所有连接对，再统一 connect；最后统一 remove。
    // ============================================================
    static std::string sig_key(const SigSpec &s) {
        std::string k;
        k.reserve(s.size() * 10);
        for (auto bit : s) {
            if (bit.wire)
                k += stringf("w%d.%d,", bit.wire->name.index_, bit.offset);
            else
                k += stringf("c%d,", int(bit.data));
        }
        return k;
    }

    int eliminate_duplicate_reduce_or(Module *module, SigMap &sigmap) {
        std::map<std::string, Cell*> canon;
        std::vector<std::pair<Cell*, Cell*>> dups;   // {dup, canon}

        for (auto cell : module->cells()) {
            if (cell->type != ID($reduce_or)) continue;

            SigSpec A = sigmap(cell->getPort(ID(A)));
            A.sort_and_unify();
            std::string k = sig_key(A);

            auto it = canon.find(k);
            if (it == canon.end())
                canon.emplace(std::move(k), cell);
            else
                dups.push_back({cell, it->second});
        }
        if (dups.empty()) return 0;

        // ===== 阶段 1：提前取出所有 Y 端 SigSpec，再全部 connect =====
        std::vector<std::pair<SigSpec, SigSpec>> conns;
        conns.reserve(dups.size());
        for (auto &[dup, c] : dups)
            conns.push_back({dup->getPort(ID(Y)), c->getPort(ID(Y))});

        for (auto &[y_dup, y_canon] : conns)
            module->connect(y_dup, y_canon);

        // ===== 阶段 2：统一 remove =====
        for (auto &[dup, c] : dups)
            module->remove(dup);

        return (int)dups.size();
    }

    // ============================================================
    // 3.3 $eq 恒等式化简
    //
    // 关键：遍历 module->cells() 时**不能** addReduceOr/addNot（会修改 container）。
    // 采用两阶段：先收集所有可化简项（不修改 module），再执行替换。
    // ============================================================
    enum EqKind { EQ_REPLACE_1, EQ_REPLACE_NOT_OR, EQ_REPLACE_AND };

    struct EqWork {
        Cell*   cell;
        SigSpec a_sig;
        EqKind  kind;
    };

    int simplify_eq_identities(Module *module, SigMap &sigmap) {
        std::vector<EqWork> work;

        // ===== 阶段 1：只读遍历，收集待处理项 =====
        for (auto cell : module->cells()) {
            if (cell->type != ID($eq)) continue;

            SigSpec A = sigmap(cell->getPort(ID(A)));
            SigSpec B = sigmap(cell->getPort(ID(B)));
            SigSpec Y = sigmap(cell->getPort(ID(Y)));

            if (Y.size() != 1 || !Y[0].wire) continue;

            // 恒等式 1：$eq(a, a) -> 1
            if (A == B) {
                work.push_back({cell, SigSpec(), EQ_REPLACE_1});
                continue;
            }

            // 找一个常量操作数
            RTLIL::Const cval;
            SigSpec a_sig;
            bool has_const = false;
            if (B.is_fully_const())      { cval = B.as_const(); a_sig = A; has_const = true; }
            else if (A.is_fully_const()) { cval = A.as_const(); a_sig = B; has_const = true; }
            if (!has_const || !a_sig.is_wire()) continue;

            bool all_zero = true, all_one = true, has_undef = false;
            for (int i = 0; i < cval.size(); i++) {
                auto v = cval[i];
                if (v == State::S1) all_zero = false;
                else if (v == State::S0) all_one = false;
                else { has_undef = true; break; }
            }
            if (has_undef) continue;

            // $eq 的两个操作数允许位宽不同：窄操作数会被扩展后再比较。
            // 全 0 常量无论零扩展还是符号扩展都仍是 0，所以 eq(a,0) -> ~|a 恒成立；
            // 全 1 只有在“扩展后仍全 1”时才等价于 &a：本位宽相同（零扩展不补位），
            // 或两侧都是有符号（符号扩展会把 1 补成全 1）。
            bool a_signed = cell->getParam(ID::A_SIGNED).as_bool();
            bool b_signed = cell->getParam(ID::B_SIGNED).as_bool();
            bool ones_extend_to_all_ones =
                (cval.size() == a_sig.size()) || (a_signed && b_signed);

            if (all_zero)
                work.push_back({cell, a_sig, EQ_REPLACE_NOT_OR});
            else if (all_one && ones_extend_to_all_ones)
                work.push_back({cell, a_sig, EQ_REPLACE_AND});
        }

        if (work.empty()) return 0;

        // ===== 阶段 2：执行替换 =====
        int cnt = 0;
        std::vector<Cell*> to_remove;

        for (auto &w : work) {
            SigSpec result;

            if (w.kind == EQ_REPLACE_1) {
                result = SigSpec(State::S1, 1);

            } else if (w.kind == EQ_REPLACE_NOT_OR) {
                IdString or_name = module->uniquify(
                    stringf("$auto$pmux_optimize.cc:eq0_or$%d", cnt));
                SigSpec or_out = module->addWire(NEW_ID, 1);
                module->addReduceOr(or_name, w.a_sig, or_out);

                IdString not_name = module->uniquify(
                    stringf("$auto$pmux_optimize.cc:eq0_not$%d", cnt));
                SigSpec not_out = module->addWire(NEW_ID, 1);
                module->addNot(not_name, or_out, not_out);

                result = not_out;

            } else { // EQ_REPLACE_AND
                IdString and_name = module->uniquify(
                    stringf("$auto$pmux_optimize.cc:eqall_and$%d", cnt));
                SigSpec and_out = module->addWire(NEW_ID, 1);
                module->addReduceAnd(and_name, w.a_sig, and_out);

                result = and_out;
            }

            module->connect(w.cell->getPort(ID(Y)), result);
            to_remove.push_back(w.cell);
            cnt++;
        }

        for (auto c : to_remove) module->remove(c);
        return cnt;
    }

    // ============================================================
    // 主入口
    //
    // 每个阶段独立 SigMap：前一个阶段可能删/加 cell，旧 sigmap 不再可靠。
    // ============================================================
    void execute(std::vector<std::string> args, Design *design) override {
        log_header(design, "Executing PMUX_OPTIMIZE pass (Phase 3).\n");

        bool do_cse = true;
        bool do_redundant = true;
        bool do_eq_simplify = true;
        int max_cells = 50000;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-no-cse")           { do_cse = false;         continue; }
            if (args[argidx] == "-no-redundant-mux") { do_redundant = false;   continue; }
            if (args[argidx] == "-no-eq-simplify")   { do_eq_simplify = false; continue; }
            if (args[argidx] == "-max-cells" && argidx + 1 < args.size()) {
                max_cells = atoi(args[++argidx].c_str());
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        bool did_something = false;

        for (auto module : design->selected_modules()) {
            int total = 0;

            // 面积门限：超过阈值的大模块直接跳过，避免 O(n) 扫描拖慢综合
            if (max_cells > 0 && GetSize(module->cells()) > max_cells) {
                log("  [pmux_optimize] skip %s: %d cells > -max-cells %d\n",
                    log_id(module), GetSize(module->cells()), max_cells);
                continue;
            }

            if (do_redundant) {
                SigMap sigmap(module);   // 独立 SigMap
                int n = remove_redundant_mux(module, sigmap);
                if (n > 0) {
                    log("  [3.1] Removed %d redundant $mux\n", n);
                    total += n;
                    did_something = true;
                }
            }

            if (do_cse) {
                SigMap sigmap(module);   // 独立 SigMap（前一个阶段可能已删 cell）
                int n = eliminate_duplicate_reduce_or(module, sigmap);
                if (n > 0) {
                    log("  [3.2] Eliminated %d duplicate $reduce_or\n", n);
                    total += n;
                    did_something = true;
                }
            }

            if (do_eq_simplify) {
                SigMap sigmap(module);   // 独立 SigMap
                int n = simplify_eq_identities(module, sigmap);
                if (n > 0) {
                    log("  [3.3] Simplified %d $eq via identities\n", n);
                    total += n;
                    did_something = true;
                }
            }

            if (total > 0)
                log("  Total: %d cells affected\n", total);
        }

        if (did_something)
            log("\n  Phase 3 custom optimizations done. Call 'opt' explicitly if needed.\n");
        else
            log("\n  No optimizations applied.\n");
    }

} PmuxOptimizePass;

PRIVATE_NAMESPACE_END