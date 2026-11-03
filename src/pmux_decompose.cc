#include "kernel/yosys.h"
#include "kernel/sigtools.h"

#include <map>
#include <algorithm>
#include <vector>
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PmuxTag {
    int         group_id     = -1;
    std::string cls;
    std::string encoding;
    std::string origin;
    int         orig_sel_bits = 0;
    int         width        = 0;
    int         base_depth   = 0;
};

static int tree_levels_internal(int num_inputs)
{
    if (num_inputs <= 1) return 0;
    int h = 0, sz = num_inputs;
    while (sz > 1) { sz = (sz + 1) / 2; h++; }
    return h;
}

static int tree_levels_total(int num_inputs)
{
    if (num_inputs <= 1) return 1;
    return tree_levels_internal(num_inputs) + 1;
}

static int wide_total_levels(int n, int GROUP)
{
    if (n <= GROUP) return tree_levels_total(n);
    int num_groups   = (n + GROUP - 1) / GROUP;
    int group_levels = tree_levels_total(std::min(GROUP, n));
    int top_levels   = wide_total_levels(num_groups, GROUP);
    return group_levels + top_levels;
}

static void tag_new_cell(Module *module, IdString cell_name,
                        const PmuxTag &tag,
                        const std::string &role,
                        int pos, int depth)
{
    Cell *c = module->cell(cell_name);
    if (!c) return;

    int w = (role == "or") ? 1 : tag.width;

    c->attributes[ID(pmux_id)]       = RTLIL::Const(tag.group_id, 32);
    c->attributes[ID(pmux_class)]    = RTLIL::Const(tag.cls);
    c->attributes[ID(pmux_encoding)] = RTLIL::Const(tag.encoding);
    c->attributes[ID(pmux_role)]     = RTLIL::Const(role);
    c->attributes[ID(pmux_pos)]      = RTLIL::Const(pos, 32);
    c->attributes[ID(pmux_depth)]    = RTLIL::Const(depth, 32);
    c->attributes[ID(pmux_origin)]   = RTLIL::Const(tag.origin);
    c->attributes[ID(pmux_width)]    = RTLIL::Const(w, 32);
    c->attributes[ID(pmux_sel_bits)] = RTLIL::Const(tag.orig_sel_bits, 32);
}

struct PmuxDecomposePass : public Pass {
    PmuxDecomposePass() : Pass("pmux_decompose", "decompose $pmux cells based on Phase-1 classification") {}

    void help() override {
        log("\n");
        log("    pmux_decompose [-no-muxcover] [-quiet] [selection]\n");
        log("\n");
        log("Decompose $pmux / $safe_pmux cells annotated by Phase-1 classifier.\n");
        log("Order: $pmux first, then $mux chains (handles nested).\n");
        log("\n");
        log("Attributes written to every generated $mux / $reduce_or:\n");
        log("    pmux_id, pmux_class, pmux_encoding, pmux_role, pmux_pos,\n");
        log("    pmux_depth, pmux_origin, pmux_width, pmux_sel_bits\n");
        log("\n");
    }

    static std::string get_str_attr(const Cell *cell, IdString key, const std::string &def = "") {
        auto it = cell->attributes.find(key);
        return (it != cell->attributes.end()) ? it->second.decode_string() : def;
    }
    static int get_int_attr(const Cell *cell, IdString key, int def = -1) {
        auto it = cell->attributes.find(key);
        return (it != cell->attributes.end()) ? it->second.as_int() : def;
    }
    static bool spec_is_fully_undef(const SigSpec &s) {
        return s.is_fully_const() && s.is_fully_undef();
    }

    // ========================================================================
    // 优先级链展开
    //
    // 每个 sel/data/invert 三元组对应链的一级：
    //   invert[i] = false  →  Y = sel[i] ? data[i] : current
    //   invert[i] = true   →  Y = sel[i] ? current : data[i]
    //
    // invert=true 时不再生成 $not(sel)，而是把 $mux 的 A/B 位置交换——
    // 语义完全等价，且省掉一级反相器。
    // ========================================================================
    SigSpec decompose_priority_chain(Module *module,
                                     const std::vector<SigBit> &sel,
                                     const std::vector<SigSpec> &data,
                                     const std::vector<bool> &invert,
                                     SigSpec default_val,
                                     const PmuxTag &tag,
                                     IdString name_hint)
    {
        int n = (int)sel.size();
        SigSpec current = default_val;
        for (int i = 0; i < n; i++) {
            IdString mux_name = module->uniquify(
                stringf("%s_prio_%d", name_hint.c_str(), i));
            SigSpec y = module->addWire(NEW_ID, tag.width);

            bool inv = (i < (int)invert.size()) && invert[i];
            if (inv) {
                module->addMux(mux_name, data[i], current, sel[i], y);
            } else {
                module->addMux(mux_name, current, data[i], sel[i], y);
            }

            int depth = tag.base_depth + (n - 1 - i);
            tag_new_cell(module, mux_name, tag, "chain", i, depth);
            current = y;
        }
        return current;
    }

    SigSpec decompose_balanced_tree(Module *module,
                                    const std::vector<SigBit> &sel,
                                    const std::vector<SigSpec> &data,
                                    SigSpec default_val,
                                    const PmuxTag &tag,
                                    IdString name_hint,
                                    int &pos_counter)
    {
        int n = (int)data.size();
        if (n == 0) return default_val;

        if (n == 1) {
            IdString mux_name = module->uniquify(
                stringf("%s_btree_leaf", name_hint.c_str()));
            SigSpec y = module->addWire(NEW_ID, tag.width);
            module->addMux(mux_name, default_val, data[0], sel[0], y);
            tag_new_cell(module, mux_name, tag, "leaf",
                         pos_counter++, tag.base_depth);
            return y;
        }

        int h = tree_levels_internal(n);
        std::vector<SigSpec> level = data;

        int step = 1;
        int lvl  = 0;

        while (level.size() > 1) {
            std::vector<SigSpec> next_level;
            for (size_t i = 0; i < level.size(); i += 2) {
                IdString mux_name = module->uniquify(
                    stringf("%s_btree_L%d_%zu", name_hint.c_str(), lvl, i / 2));
                SigSpec y = module->addWire(NEW_ID, tag.width);

                std::string role = (lvl == 0) ? "leaf" : "tree";
                int node_depth = tag.base_depth + (h - lvl);

                if (i + 1 < level.size()) {
                    int hi_start = (int)(i + 1) * step;
                    int hi_end   = std::min(hi_start + step, n);

                    SigSpec or_input;
                    for (int b = hi_start; b < hi_end; b++)
                        or_input.append(sel[b]);

                    SigSpec hi_valid;
                    if (or_input.size() == 1) {
                        hi_valid = or_input;
                    } else {
                        IdString or_name = module->uniquify(
                            stringf("%s_btree_or_L%d_%zu",
                                    name_hint.c_str(), lvl, i / 2));
                        hi_valid = module->addWire(NEW_ID);
                        module->addReduceOr(or_name, or_input, hi_valid);
                        tag_new_cell(module, or_name, tag, "or",
                                     pos_counter++, node_depth);
                    }

                    module->addMux(mux_name, level[i], level[i + 1], hi_valid, y);
                } else {
                    int lo_start = (int)i * step;
                    int lo_end   = std::min(lo_start + step, n);

                    SigSpec or_input;
                    for (int b = lo_start; b < lo_end; b++)
                        or_input.append(sel[b]);

                    SigSpec node_valid;
                    if (or_input.size() == 1) {
                        node_valid = or_input;
                    } else {
                        IdString or_name = module->uniquify(
                            stringf("%s_btree_or_odd_L%d_%zu",
                                    name_hint.c_str(), lvl, i / 2));
                        node_valid = module->addWire(NEW_ID);
                        module->addReduceOr(or_name, or_input, node_valid);
                        tag_new_cell(module, or_name, tag, "or",
                                     pos_counter++, node_depth);
                    }

                    module->addMux(mux_name, default_val, level[i], node_valid, y);
                }

                tag_new_cell(module, mux_name, tag, role,
                             pos_counter++, node_depth);
                next_level.push_back(y);
            }
            level = next_level;
            step *= 2;
            lvl++;
        }

        SigSpec all_sel;
        for (auto b : sel) all_sel.append(b);

        SigSpec any_sel_valid;
        IdString final_or_name = module->uniquify(
            stringf("%s_btree_final_or", name_hint.c_str()));
        if (all_sel.size() == 1) {
            any_sel_valid = all_sel;
        } else {
            any_sel_valid = module->addWire(NEW_ID);
            module->addReduceOr(final_or_name, all_sel, any_sel_valid);
            tag_new_cell(module, final_or_name, tag, "or",
                         pos_counter++, tag.base_depth);
        }

        IdString final_mux_name = module->uniquify(
            stringf("%s_btree_final", name_hint.c_str()));
        SigSpec y_final = module->addWire(NEW_ID, tag.width);
        module->addMux(final_mux_name, default_val, level[0], any_sel_valid, y_final);

        tag_new_cell(module, final_mux_name, tag, "top",
                     pos_counter++, tag.base_depth);

        return y_final;
    }

    SigSpec decompose_wide_hierarchical(Module *module,
                                        const std::vector<SigBit> &sel,
                                        const std::vector<SigSpec> &data,
                                        SigSpec default_val,
                                        const PmuxTag &tag,
                                        IdString name_hint,
                                        int &pos_counter)
    {
        const int GROUP = 4;
        int n = (int)data.size();

        if (n == 0) return default_val;
        if (n == 1) {
            SigSpec y = module->addWire(NEW_ID, tag.width);
            IdString mux_name = module->uniquify(
                stringf("%s_wide_single", name_hint.c_str()));
            module->addMux(mux_name, default_val, data[0], sel[0], y);
            tag_new_cell(module, mux_name, tag, "leaf",
                         pos_counter++, tag.base_depth);
            return y;
        }

        int num_groups = (n + GROUP - 1) / GROUP;

        int top_levels;
        if (num_groups <= GROUP) {
            top_levels = tree_levels_total(num_groups);
        } else {
            top_levels = wide_total_levels(num_groups, GROUP);
        }

        PmuxTag sub_tag = tag;
        sub_tag.base_depth = tag.base_depth + top_levels;

        std::vector<SigSpec> level1_outs;
        std::vector<SigBit>  group_sel_bits;

        for (int g = 0; g < n; g += GROUP) {
            int cnt = std::min(GROUP, n - g);
            SigSpec grp_or_input;
            for (int j = 0; j < cnt; j++)
                grp_or_input.append(sel[g + j]);

            SigSpec grp_valid;
            IdString or_name = module->uniquify(
                stringf("%s_wide_grp_or_%d", name_hint.c_str(), g / GROUP));
            if (grp_or_input.size() == 1) {
                grp_valid = grp_or_input;
            } else {
                grp_valid = module->addWire(NEW_ID);
                module->addReduceOr(or_name, grp_or_input, grp_valid);
                int or_depth = sub_tag.base_depth - 1;
                tag_new_cell(module, or_name, tag, "or",
                             pos_counter++, or_depth);
            }
            group_sel_bits.push_back(grp_valid[0]);

            std::vector<SigBit>  grp_sel(sel.begin() + g, sel.begin() + g + cnt);
            std::vector<SigSpec> grp_data(data.begin() + g, data.begin() + g + cnt);

            IdString grp_hint = module->uniquify(
                stringf("%s_wide_L1_g%d", name_hint.c_str(), g / GROUP));

            SigSpec grp_result = decompose_balanced_tree(
                module, grp_sel, grp_data, default_val, sub_tag, grp_hint,
                pos_counter);

            level1_outs.push_back(grp_result);
        }

        IdString top_hint = module->uniquify(
            stringf("%s_wide_top", name_hint.c_str()));
        if ((int)level1_outs.size() <= GROUP) {
            return decompose_balanced_tree(module, group_sel_bits, level1_outs,
                                           default_val, tag, top_hint,
                                           pos_counter);
        } else {
            return decompose_wide_hierarchical(module, group_sel_bits, level1_outs,
                                               default_val, tag, top_hint,
                                               pos_counter);
        }
    }

    // ========================================================================
    // 主执行入口
    // ========================================================================
    void execute(std::vector<std::string> args, Design *design) override
    {
        log_header(design, "Executing PMUX_DECOMPOSE pass (Phase 2).\n");

        bool no_muxcover = false;
        bool quiet = false;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-no-muxcover") { no_muxcover = true; continue; }
            if (args[argidx] == "-quiet")       { quiet = true;       continue; }
            break;
        }
        extra_args(args, argidx, design);

        int decomposed_count  = 0;
        int decomposed_chains = 0;
        int skipped_sparse    = 0;
        int skipped_unknown   = 0;

        for (auto module : design->selected_modules()) {
            SigMap sigmap(module);

            // ============================================================
            // ===== 1. 先处理 $pmux / $safe_pmux =====
            // ============================================================
            std::vector<Cell *> pmux_cells;
            for (auto cell : module->cells()) {
                if (cell->type != ID($pmux) && cell->type != ID($safe_pmux)) continue;

                if (cell->type == ID($safe_pmux) && !cell->has_attribute(ID(pmux_class))) {
                    cell->set_string_attribute(ID(pmux_class), "priority");
                    cell->set_string_attribute(ID(pmux_encoding), "priority");
                    cell->attributes[ID(pmux_id)] = RTLIL::Const(-1, 32);
                }

                if (!cell->has_attribute(ID(pmux_class))) continue;
                pmux_cells.push_back(cell);
            }

            for (auto cell : pmux_cells) {
                std::string cls = get_str_attr(cell, ID(pmux_class));
                std::string enc = get_str_attr(cell, ID(pmux_encoding), "unknown");
                int pmux_id     = get_int_attr(cell, ID(pmux_id), -1);

                if (enc == "binary") {
                    log_warning("PMUX #%d has binary encoding; skipping.\n", pmux_id);
                    skipped_unknown++;
                    continue;
                }

                SigSpec A = cell->getPort(ID(A));
                SigSpec B = cell->getPort(ID(B));
                SigSpec S = cell->getPort(ID(S));
                int width   = cell->getParam(ID(WIDTH)).as_int();
                int s_width = cell->getParam(ID(S_WIDTH)).as_int();

                if (cell->type == ID($safe_pmux)) {
                    A = SigSpec(RTLIL::Const(RTLIL::State::Sx, width));
                }

                std::vector<SigBit>  sel_bits;
                std::vector<SigSpec> data_vec;
                for (int i = 0; i < s_width; i++) {
                    sel_bits.push_back(S[i]);
                    data_vec.push_back(B.extract(i * width, width));
                }

                if (!quiet) {
                    log("  PMUX #%d [%s]: class=%s, encoding=%s, branches=%d, width=%d\n",
                        pmux_id, log_id(cell->name), cls.c_str(), enc.c_str(), s_width, width);
                }

                PmuxTag tag;
                tag.group_id      = pmux_id;
                tag.cls           = cls;
                tag.encoding      = enc;
                tag.origin        = "pmux";
                tag.orig_sel_bits = s_width;
                tag.width         = width;
                tag.base_depth    = 0;

                SigSpec result;
                bool handled = true;

                if (cls == "exclusive") {
                    int pos_counter = 0;
                    result = decompose_balanced_tree(module, sel_bits, data_vec,
                                                     A, tag, cell->name, pos_counter);
                } else if (cls == "priority" || cls == "nested") {
                    // $pmux 的 S 端口天然无极性反转，invert 全 false。
                    std::vector<bool> no_invert(sel_bits.size(), false);
                    result = decompose_priority_chain(module, sel_bits, data_vec,
                                                      no_invert, A, tag, cell->name);
                } else if (cls == "wide") {
                    int pos_counter = 0;
                    result = decompose_wide_hierarchical(module, sel_bits, data_vec,
                                                         A, tag, cell->name, pos_counter);
                } else if (cls == "sparse") {
                    if (!quiet) log("    Skipping sparse (deferred to Phase 3)\n");
                    skipped_sparse++;
                    handled = false;
                } else {
                    log_warning("    Unknown pmux_class '%s', skipping.\n", cls.c_str());
                    skipped_unknown++;
                    handled = false;
                }

                if (!handled) continue;

                module->connect(cell->getPort(ID(Y)), result);
                module->remove(cell);
                decomposed_count++;

                if (!quiet) log("    Decomposed → %s\n", log_signal(result));
            }

            // ============================================================
            // ===== 2. 再处理 $mux 链 =====
            // ============================================================
            std::map<int, std::vector<Cell*>> chains;
            for (auto cell : module->cells()) {
                if (cell->type != ID($mux) && cell->type != ID($_MUX_)) continue;
                if (!cell->has_attribute(ID(pmux_member))) continue;
                int id = get_int_attr(cell, ID(pmux_id), -1);
                if (id < 0) continue;
                chains[id].push_back(cell);
            }

            for (auto &kv : chains) {
                int id = kv.first;
                auto &cells = kv.second;
                if (cells.empty()) continue;

                Cell *head = nullptr;
                for (auto c : cells) {
                    if (get_int_attr(c, ID(pmux_head), 0) == 1) {
                        head = c;
                        break;
                    }
                }
                if (!head) {
                    log_warning("Chain #%d: no head found, skipping.\n", id);
                    continue;
                }

                std::sort(cells.begin(), cells.end(),
                    [](Cell *a, Cell *b) {
                        return get_int_attr(a, ID(pmux_pos), 0) <
                               get_int_attr(b, ID(pmux_pos), 0);
                    });

                std::vector<SigBit>  sel_bits;
                std::vector<SigSpec> data_vec;
                std::vector<bool>    invert_vec;
                SigSpec              default_val;
                int width = GetSize(head->getPort(ID(Y)));

                bool topology_ok = true;
                for (int i = 0; i < GetSize(cells); i++) {
                    Cell *c = cells[i];
                    SigSpec S = sigmap(c->getPort(ID(S)));
                    SigSpec A = sigmap(c->getPort(ID(A)));
                    SigSpec B = sigmap(c->getPort(ID(B)));

                    if (S.size() != 1) {
                        log_warning("Chain #%d: cell %s has non-1-bit select, "
                                    "skipping.\n", id, log_id(c->name));
                        topology_ok = false;
                        break;
                    }

                    if (i + 1 < GetSize(cells)) {
                        Cell *next = cells[i + 1];
                        SigSpec next_y = sigmap(next->getPort(ID(Y)));

                        SigSpec branch_data;
                        bool invert = false;

                        if (A == next_y) {
                            // Y = S ? B : next_y  →  选 B，极性不变
                            branch_data = B;
                            invert = false;
                        } else if (B == next_y) {
                            // Y = S ? next_y : A  →  选 A，极性反转
                            branch_data = A;
                            invert = true;
                        } else {
                            log_warning("Chain #%d: topology broken at pos %d "
                                        "(cell %s), skipping.\n",
                                        id, i, log_id(c->name));
                            topology_ok = false;
                            break;
                        }

                        if (spec_is_fully_undef(branch_data))
                            continue;

                        sel_bits.push_back(S[0]);
                        data_vec.push_back(branch_data);
                        invert_vec.push_back(invert);
                    } else {
                        // 尾端：Y = S ? B : A，A 是 default，本级数据在 B
                        if (!spec_is_fully_undef(B)) {
                            sel_bits.push_back(S[0]);
                            data_vec.push_back(B);
                            invert_vec.push_back(false);
                        }
                        default_val = A;
                    }
                }

                if (!topology_ok) continue;

                std::string cls = get_str_attr(head, ID(pmux_class), "priority");
                std::string enc = get_str_attr(head, ID(pmux_encoding), "priority");

                PmuxTag tag;
                tag.group_id      = id;
                tag.cls           = cls;
                tag.encoding      = enc;
                tag.origin        = "mux_chain";
                tag.orig_sel_bits = GetSize(cells);
                tag.width         = width;
                tag.base_depth    = 0;

                SigSpec result;
                bool handled = true;

                if (cls == "exclusive") {
                    // 平衡树是 one-hot 语义，与极性无关；invert_vec 不参与。
                    int pos_counter = 0;
                    result = decompose_balanced_tree(module, sel_bits, data_vec,
                                                     default_val, tag, head->name,
                                                     pos_counter);
                } else if (cls == "priority" || cls == "nested") {
                    // head 在 cells[0]，要落到遍历的末尾（最高优先级）——
                    // 三个并行数组一起反转，保持 i 轴一致。
                    std::reverse(sel_bits.begin(),   sel_bits.end());
                    std::reverse(data_vec.begin(),   data_vec.end());
                    std::reverse(invert_vec.begin(), invert_vec.end());

                    result = decompose_priority_chain(module, sel_bits, data_vec,
                                                      invert_vec, default_val,
                                                      tag, head->name);
                } else {
                    log_warning("Chain #%d: unsupported class '%s', skipping.\n",
                                id, cls.c_str());
                    handled = false;
                }

                if (!handled) continue;

                SigSpec head_y = head->getPort(ID(Y));
                for (auto c : cells) module->remove(c);
                module->connect(head_y, result);

                decomposed_count++;
                decomposed_chains++;

                if (!quiet) {
                    log("  Chain #%d: %d cells decomposed (class=%s, enc=%s, %d branches)\n",
                        id, GetSize(cells), cls.c_str(), enc.c_str(),
                        GetSize(sel_bits));
                }
            }
        }

        log("\n  Summary: %d decomposed (%d from chains), %d sparse skipped, "
            "%d unknown skipped\n",
            decomposed_count, decomposed_chains,
            skipped_sparse, skipped_unknown);

        if (decomposed_count > 0 && !no_muxcover) {
            log("\n  Running technology mapping (muxcover)...\n");
            try {
                Pass::call(design, "muxcover -mux4 -mux8 -mux16");
            } catch (log_cmd_error_exception &) {
                log_warning("muxcover not available or failed, skipping tech mapping.\n");
            }
        } else if (decomposed_count > 0 && no_muxcover) {
            log("\n  Skipping muxcover (-no-muxcover specified).\n");
        } else {
            log("\n  No PMUX decomposed, skipping muxcover.\n");
        }
    }

} PmuxDecomposePass;

PRIVATE_NAMESPACE_END