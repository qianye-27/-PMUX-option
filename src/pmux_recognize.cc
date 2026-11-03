/*
 * pmux_recognize.cc — Yosys PMUX Recognition & Classification Plugin (v2)
 *
 * 编译：
 *   yosys-config --build pmux_recognize.so pmux_recognize.cc
 */

#include "kernel/yosys.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// ============================================================
//  数据结构
// ============================================================

enum class PmuxEncoding {
    BINARY, ONE_HOT, PRIORITY, DONT_CARE, UNKNOWN
};

enum class PmuxClass {
    EXCLUSIVE,
    PRIORITY,
    SPARSE,
    WIDE,
    NESTED,
    ENABLED,
    PARTIAL_EXCL,
    UNKNOWN
};

struct PmuxBranch {
    RTLIL::SigSpec condition;
    RTLIL::SigSpec value;
    int priority = 0;
    bool is_default = false;
    bool is_constant = false;
    bool is_undef = false;
};

struct PmuxInfo {
    int id = -1;
    RTLIL::IdString target;
    int width = 0;
    int branch_count = 0;
    int fanout = 0;

    std::vector<PmuxBranch> branches;
    std::set<RTLIL::SigBit> sel_bits;

    PmuxEncoding encoding = PmuxEncoding::UNKNOWN;
    PmuxClass classification = PmuxClass::UNKNOWN;

    bool is_complete = false;
    bool has_constant = false;
    bool from_pmux_cell = false;

    RTLIL::Cell* origin_cell = nullptr;
    std::vector<RTLIL::Cell*> chain_cells;
    std::vector<int> shared_with;
};

struct PmuxConfig {
    int wide_threshold = 8;
    float sparse_ratio = 0.5f;
    int max_chain_depth = 32;
    bool annotate = false;
};

struct PmuxResult {
    std::vector<PmuxInfo> items;
    std::map<RTLIL::IdString, std::vector<int>> target_map;

    int add(PmuxInfo info) {
        info.id = (int)items.size();
        items.push_back(std::move(info));
        target_map[items.back().target].push_back(info.id);
        return info.id;
    }
};

// ============================================================
//  辅助
// ============================================================

static const char* enc_str(PmuxEncoding e) {
    switch (e) {
        case PmuxEncoding::BINARY:    return "binary";
        case PmuxEncoding::ONE_HOT:   return "one_hot";
        case PmuxEncoding::PRIORITY:  return "priority";
        case PmuxEncoding::DONT_CARE: return "dont_care";
        default:                      return "unknown";
    }
}

static const char* cls_str(PmuxClass c) {
    switch (c) {
        case PmuxClass::EXCLUSIVE:    return "exclusive";
        case PmuxClass::PRIORITY:     return "priority";
        case PmuxClass::SPARSE:       return "sparse";
        case PmuxClass::WIDE:         return "wide";
        case PmuxClass::NESTED:       return "nested";
        case PmuxClass::ENABLED:      return "enabled";
        case PmuxClass::PARTIAL_EXCL: return "partial_exclusive";
        default:                      return "unknown";
    }
}

static bool const_is_fully_undef(const RTLIL::SigSpec& sig) {
    if (!sig.is_fully_const()) return false;
    return sig.is_fully_undef();
}

// ============================================================
//  分析器
// ============================================================

struct PmuxAnalyzer {
    RTLIL::Module* module;
    SigMap sigmap;
    PmuxConfig config;
    PmuxResult result;
    int auto_id_counter = 0;

    std::map<RTLIL::SigBit, RTLIL::Cell*> driver_map;
    std::map<RTLIL::SigBit, int> fanout_map;

    PmuxAnalyzer(RTLIL::Module* mod, const PmuxConfig& cfg)
        : module(mod), sigmap(mod), config(cfg)
    {
        build_driver_map();
        build_fanout_map();
    }

    void build_driver_map()
    {
        for (auto cell : module->cells()) {
            for (auto& conn : cell->connections()) {
                if (cell->output(conn.first)) {
                    for (auto bit : sigmap(conn.second)) {
                        if (bit.wire)
                            driver_map[bit] = cell;
                    }
                }
            }
        }
    }

    void build_fanout_map()
    {
        for (auto cell : module->cells()) {
            for (auto& conn : cell->connections()) {
                if (cell->input(conn.first)) {
                    for (auto bit : sigmap(conn.second)) {
                        if (bit.wire)
                            fanout_map[bit]++;
                    }
                }
            }
        }
    }

    int get_fanout(const RTLIL::SigSpec& sig)
    {
        int max_fo = 0;
        for (auto bit : sigmap(sig)) {
            if (bit.wire) {
                auto it = fanout_map.find(bit);
                if (it != fanout_map.end())
                    max_fo = std::max(max_fo, it->second);
            }
        }
        return max_fo;
    }

    RTLIL::IdString make_auto_name(const char* prefix)
    {
        return RTLIL::IdString(stringf("%s_%d", prefix, auto_id_counter++));
    }

    // ---- 主入口 ----
    void run()
    {
        log("  PMUX recognition on module %s:\n", log_id(module));

        recognize_pmux_cells();
        recognize_mux_chains();
        analyze_encodings();
        classify_all();
        find_shared_selectors();

        log("  Identified %d PMUX structure(s).\n", GetSize(result.items));
    }

    // ---- 识别 $pmux 单元 ----
    void recognize_pmux_cells()
    {
        for (auto cell : module->cells()) {
            if (cell->type != ID($pmux))
                continue;

            PmuxInfo info;
            info.from_pmux_cell = true;
            info.origin_cell = cell;

            RTLIL::SigSpec sig_a = sigmap(cell->getPort(ID(A)));
            RTLIL::SigSpec sig_b = sigmap(cell->getPort(ID(B)));
            RTLIL::SigSpec sig_s = sigmap(cell->getPort(ID(S)));
            RTLIL::SigSpec sig_y = sigmap(cell->getPort(ID(Y)));

            info.width = GetSize(sig_a);

            if (sig_y.is_wire())
                info.target = sig_y.as_wire()->name;
            else
                info.target = make_auto_name("$pmux");

            int n = GetSize(sig_s);
            for (int i = 0; i < n; i++) {
                PmuxBranch br;
                br.condition = sig_s.extract(i, 1);
                br.value = sig_b.extract(i * info.width, info.width);
                br.priority = i;
                br.is_default = false;
                br.is_constant = br.value.is_fully_const();
                br.is_undef = const_is_fully_undef(br.value);

                if (br.is_undef)
                    continue;

                info.branches.push_back(br);
            }

            PmuxBranch def;
            def.value = sig_a;
            def.priority = n;
            def.is_default = true;
            def.is_constant = sig_a.is_fully_const();
            def.is_undef = const_is_fully_undef(sig_a);

            if (!def.is_undef)
                info.branches.push_back(def);

            info.branch_count = GetSize(info.branches);
            info.fanout = get_fanout(sig_y);
            info.is_complete = true;

            collect_sel_bits(info);
            result.add(std::move(info));
        }
    }

    // ---- 识别 $mux 级联链 ----
    void recognize_mux_chains()
    {
        std::vector<RTLIL::Cell*> mux_cells;
        for (auto cell : module->cells()) {
            if (cell->type == ID($mux) || cell->type == ID($_MUX_))
                mux_cells.push_back(cell);
        }

        std::set<RTLIL::Cell*> consumed;

        for (auto cell : mux_cells) {
            if (consumed.count(cell))
                continue;

            RTLIL::Cell* top = find_chain_top(cell, mux_cells);
            if (consumed.count(top))
                continue;

            auto chain = trace_chain_down(top, mux_cells);

            if (GetSize(chain) >= 2) {
                for (auto c : chain)
                    consumed.insert(c);
                build_pmux_from_chain(chain);
            }
        }
    }

    // ============================================================
    // find_chain_top：整体 SigSpec 匹配（不再逐 bit）
    // ============================================================
    RTLIL::Cell* find_chain_top(RTLIL::Cell* cell, const std::vector<RTLIL::Cell*>& all_mux)
    {
        RTLIL::Cell* current = cell;
        int depth = 0;

        while (depth < config.max_chain_depth) {
            RTLIL::SigSpec out = sigmap(current->getPort(ID(Y)));
            RTLIL::Cell* parent = nullptr;

            for (auto cand : all_mux) {
                if (cand == current) continue;
                RTLIL::SigSpec a = sigmap(cand->getPort(ID(A)));
                RTLIL::SigSpec b = sigmap(cand->getPort(ID(B)));

                // 整体匹配：Y 必须整体等于 A 或 B
                if (a == out || b == out) {
                    parent = cand;
                    break;
                }
            }

            if (!parent) break;
            current = parent;
            depth++;
        }

        return current;
    }

    // ============================================================
    // trace_chain_down：整体 SigSpec 匹配（不再逐 bit 找 driver）
    // ============================================================
    std::vector<RTLIL::Cell*> trace_chain_down(RTLIL::Cell* top,
                                              const std::vector<RTLIL::Cell*>& all_mux)
    {
        std::vector<RTLIL::Cell*> chain;
        chain.push_back(top);

        RTLIL::Cell* current = top;
        int depth = 0;

        while (depth < config.max_chain_depth) {
            RTLIL::SigSpec a = sigmap(current->getPort(ID(A)));
            RTLIL::SigSpec b = sigmap(current->getPort(ID(B)));

            RTLIL::Cell* next = nullptr;

            // 遍历所有 $mux 找 Y 整体匹配的
            for (auto cand : all_mux) {
                if (cand == current) continue;
                RTLIL::SigSpec cy = sigmap(cand->getPort(ID(Y)));
                if (a == cy || b == cy) {
                    next = cand;
                    break;
                }
            }

            if (!next) break;

            bool already = false;
            for (auto c : chain) {
                if (c == next) { already = true; break; }
            }
            if (already) break;

            chain.push_back(next);
            current = next;
            depth++;
        }

        return chain;
    }

    void build_pmux_from_chain(const std::vector<RTLIL::Cell*>& chain)
    {
        if (GetSize(chain) < 2) return;

        PmuxInfo info;
        info.from_pmux_cell = false;
        info.chain_cells = chain;

        RTLIL::SigSpec first_a = sigmap(chain[0]->getPort(ID(A)));
        info.width = GetSize(first_a);

        RTLIL::SigSpec top_y = sigmap(chain[0]->getPort(ID(Y)));
        if (top_y.is_wire())
            info.target = top_y.as_wire()->name;
        else
            info.target = make_auto_name("$chain");

        int priority = 0;
        for (int i = 0; i < GetSize(chain); i++) {
            RTLIL::Cell* c = chain[i];
            RTLIL::SigSpec sel = sigmap(c->getPort(ID(S)));
            RTLIL::SigSpec b = sigmap(c->getPort(ID(B)));

            PmuxBranch br;
            br.condition = sel;
            br.value = b;
            br.priority = priority;
            br.is_default = false;
            br.is_constant = b.is_fully_const();
            br.is_undef = const_is_fully_undef(b);

            if (br.is_undef)
                continue;

            priority++;
            info.branches.push_back(br);
        }

        RTLIL::SigSpec last_a = sigmap(chain.back()->getPort(ID(A)));
        PmuxBranch def;
        def.value = last_a;
        def.priority = priority;
        def.is_default = true;
        def.is_constant = last_a.is_fully_const();
        def.is_undef = const_is_fully_undef(last_a);

        if (!def.is_undef)
            info.branches.push_back(def);

        info.branch_count = GetSize(info.branches);
        info.fanout = get_fanout(top_y);
        info.is_complete = true;

        collect_sel_bits(info);

        int predicted_id = GetSize(result.items);

        for (int i = 0; i < GetSize(chain); i++) {
            RTLIL::Cell* c = chain[i];
            c->attributes[ID(pmux_id)]     = RTLIL::Const(predicted_id, 32);
            c->attributes[ID(pmux_pos)]    = RTLIL::Const(i, 32);
            c->attributes[ID(pmux_member)] = RTLIL::Const(1, 32);
        }
        if (!chain.empty())
            chain[0]->attributes[ID(pmux_head)] = RTLIL::Const(1, 32);

        result.add(std::move(info));
    }

    void collect_sel_bits(PmuxInfo& info)
    {
        info.sel_bits.clear();
        for (auto& br : info.branches) {
            if (br.is_default) continue;
            for (auto bit : sigmap(br.condition)) {
                if (bit.wire)
                    info.sel_bits.insert(bit);
            }
        }
    }

    // ============================================================
    // 检查 $pmux 的 S 位是否结构互斥
    // ============================================================
    bool pmux_s_bits_are_exclusive(const PmuxInfo& info)
    {
        int n_non_default = 0;
        for (auto& br : info.branches) {
            if (!br.is_default) n_non_default++;
        }
        if (n_non_default <= 1) return true;

        RTLIL::SigSpec common_sel;
        bool have_common = false;
        std::set<long long> consts;

        for (auto& br : info.branches) {
            if (br.is_default) continue;

            if (br.condition.size() != 1)
                return false;

            RTLIL::SigBit sbit = sigmap(br.condition)[0];
            auto it = driver_map.find(sbit);
            if (it == driver_map.end())
                return false;

            RTLIL::Cell* drv = it->second;

            RTLIL::SigSpec sel_sig;
            long long const_val = 0;

            if (drv->type == ID($eq)) {
                RTLIL::SigSpec a = sigmap(drv->getPort(ID(A)));
                RTLIL::SigSpec b = sigmap(drv->getPort(ID(B)));
                RTLIL::SigSpec const_sig;
                if (a.is_fully_const())      { const_sig = a; sel_sig = b; }
                else if (b.is_fully_const()) { const_sig = b; sel_sig = a; }
                else return false;
                const_val = (long long)const_sig.as_const().as_int();
            } else if (drv->type == ID($logic_not)) {
                RTLIL::SigSpec a = sigmap(drv->getPort(ID(A)));
                if (a.is_fully_const()) return false;
                sel_sig   = a;
                const_val = 0;
            } else {
                return false;
            }

            if (!have_common) {
                common_sel  = sel_sig;
                have_common = true;
            } else if (sel_sig != common_sel) {
                return false;
            }

            if (consts.count(const_val))
                return false;
            consts.insert(const_val);
        }

        return have_common;
    }

    // ---- 编码分析 ----
    void analyze_encodings()
    {
        for (auto& info : result.items) {

            if (info.from_pmux_cell) {
                info.encoding = pmux_s_bits_are_exclusive(info)
                              ? PmuxEncoding::ONE_HOT
                              : PmuxEncoding::PRIORITY;
                continue;
            }

            info.encoding = PmuxEncoding::PRIORITY;
        }
    }

    // ---- 分类 ----
    void classify_all()
    {
        for (auto& info : result.items)
            classify_one(info);
    }

    void classify_one(PmuxInfo& info)
    {
        if (check_nested(info)) {
            info.classification = PmuxClass::NESTED;
        } else if (check_sparse(info)) {
            info.classification = PmuxClass::SPARSE;
        } else if (info.branch_count > config.wide_threshold) {
            info.classification = PmuxClass::WIDE;
        } else if ((info.encoding == PmuxEncoding::ONE_HOT ||
                    info.encoding == PmuxEncoding::BINARY) && info.is_complete) {
            info.classification = PmuxClass::EXCLUSIVE;
        } else if (check_enabled(info)) {
            info.classification = PmuxClass::ENABLED;
        } else {
            info.classification = PmuxClass::PRIORITY;
        }
    }

    bool check_sparse(const PmuxInfo& info)
    {
        if (info.branch_count == 0) return false;

        int const_count = 0;
        for (auto& br : info.branches) {
            if (br.is_constant) const_count++;
        }

        std::map<std::string, int> val_count;
        for (auto& br : info.branches) {
            std::string key;
            if (br.value.is_fully_const()) {
                key = "C:" + br.value.as_const().as_string();
            } else if (br.value.is_wire()) {
                key = "W:" + br.value.as_wire()->name.str();
            } else {
                key = "S:" + log_signal(br.value);
            }
            val_count[key]++;
        }

        int max_dup = 0;
        for (auto& kv : val_count)
            max_dup = std::max(max_dup, kv.second);

        float ratio = (float)(const_count + max_dup - 1) / info.branch_count;
        return ratio > config.sparse_ratio;
    }

    bool check_nested(const PmuxInfo& info)
    {
        for (auto& br : info.branches) {
            if (br.value.is_wire()) {
                auto name = br.value.as_wire()->name;
                if (result.target_map.count(name))
                    return true;
            }
        }
        return false;
    }

    bool check_enabled(const PmuxInfo& info)
    {
        if (GetSize(info.branches) < 2) return false;

        std::set<RTLIL::SigBit> common;
        bool first = true;

        for (auto& br : info.branches) {
            if (br.is_default) continue;
            std::set<RTLIL::SigBit> bits;
            for (auto bit : sigmap(br.condition)) {
                if (bit.wire) bits.insert(bit);
            }
            if (first) {
                common = bits;
                first = false;
            } else {
                std::set<RTLIL::SigBit> inter;
                for (auto& b : common) {
                    if (bits.count(b)) inter.insert(b);
                }
                common = inter;
            }
        }

        return !common.empty();
    }

    void find_shared_selectors()
    {
        int n = GetSize(result.items);
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                bool shared = false;
                for (auto& bit : result.items[i].sel_bits) {
                    if (result.items[j].sel_bits.count(bit)) {
                        shared = true;
                        break;
                    }
                }
                if (shared) {
                    result.items[i].shared_with.push_back(j);
                    result.items[j].shared_with.push_back(i);
                }
            }
        }
    }
};

// ============================================================
//  报告生成
// ============================================================

static std::string generate_report(RTLIL::Module* module, const PmuxResult& result)
{
    std::ostringstream oss;

    oss << "\n";
    oss << "================================================================\n";
    oss << "  PMUX Recognition Report — Module: " << log_id(module) << "\n";
    oss << "================================================================\n\n";

    std::map<PmuxClass, int> cls_cnt;
    std::map<PmuxEncoding, int> enc_cnt;
    for (auto& p : result.items) {
        cls_cnt[p.classification]++;
        enc_cnt[p.encoding]++;
    }

    oss << "  Total: " << GetSize(result.items) << " PMUX structure(s)\n\n";

    oss << "  Classification breakdown:\n";
    for (auto& kv : cls_cnt)
        oss << "    " << cls_str(kv.first) << ": " << kv.second << "\n";

    oss << "\n  Encoding breakdown:\n";
    for (auto& kv : enc_cnt)
        oss << "    " << enc_str(kv.first) << ": " << kv.second << "\n";

    oss << "\n----------------------------------------------------------------\n";
    oss << "  Details:\n\n";

    for (auto& p : result.items) {
        oss << "  [PMUX #" << p.id << "]  target=" << log_id(p.target) << "\n";
        oss << "    width=" << p.width
            << "  branches=" << p.branch_count
            << "  fanout=" << p.fanout << "\n";
        oss << "    encoding=" << enc_str(p.encoding)
            << "  class=" << cls_str(p.classification)
            << "  complete=" << (p.is_complete ? "yes" : "no")
            << "  source=" << (p.from_pmux_cell ? "$pmux" : "mux_chain") << "\n";

        if (!p.shared_with.empty()) {
            oss << "    shared_sel_with=[";
            for (int i = 0; i < GetSize(p.shared_with); i++) {
                if (i) oss << ", ";
                oss << "#" << p.shared_with[i];
            }
            oss << "]\n";
        }

        oss << "    branches:\n";
        for (auto& br : p.branches) {
            if (br.is_default) {
                oss << "      [default]";
            } else {
                oss << "      [P" << br.priority << "] cond=" << log_signal(br.condition);
            }
            oss << "  val=" << log_signal(br.value);
            if (br.is_constant) oss << " (const)";
            oss << "\n";
        }
        oss << "\n";
    }

    oss << "================================================================\n";
    return oss.str();
}

// ============================================================
//  JSON 输出
// ============================================================

static void write_json(const PmuxResult& result, RTLIL::Module* module, FILE* f)
{
    fprintf(f, "{\n");
    fprintf(f, "  \"module\": \"%s\",\n", log_id(module));
    fprintf(f, "  \"count\": %d,\n", GetSize(result.items));
    fprintf(f, "  \"items\": [\n");

    for (int i = 0; i < GetSize(result.items); i++) {
        auto& p = result.items[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"id\": %d,\n", p.id);
        fprintf(f, "      \"target\": \"%s\",\n", log_id(p.target));
        fprintf(f, "      \"width\": %d,\n", p.width);
        fprintf(f, "      \"branches\": %d,\n", p.branch_count);
        fprintf(f, "      \"encoding\": \"%s\",\n", enc_str(p.encoding));
        fprintf(f, "      \"class\": \"%s\",\n", cls_str(p.classification));
        fprintf(f, "      \"complete\": %s,\n", p.is_complete ? "true" : "false");
        fprintf(f, "      \"fanout\": %d,\n", p.fanout);
        fprintf(f, "      \"branch_list\": [\n");

        for (int j = 0; j < GetSize(p.branches); j++) {
            auto& br = p.branches[j];
            fprintf(f, "        {\"priority\": %d, \"default\": %s, \"const\": %s}%s\n",
                    br.priority,
                    br.is_default ? "true" : "false",
                    br.is_constant ? "true" : "false",
                    (j < GetSize(p.branches) - 1) ? "," : "");
        }

        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", (i < GetSize(result.items) - 1) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
}

// ============================================================
//  自测试
// ============================================================

static const char* self_test_verilog = R"(
module test_pmux (
    input  wire [2:0] sel,
    input  wire [7:0] a, b, c, d, e, f, g,
    input  wire       en,
    output reg  [7:0] out1,
    output reg  [7:0] out2,
    output wire [7:0] out3,
    output reg  [7:0] out4,
    output reg  [7:0] out5
);

    always @(*) begin
        if (sel[0])      out1 = a;
        else if (sel[1]) out1 = b;
        else if (sel[2]) out1 = c;
        else             out1 = d;
    end

    always @(*) begin
        case (sel)
            3'b001:  out2 = a;
            3'b010:  out2 = b;
            3'b100:  out2 = c;
            default: out2 = 8'h00;
        endcase
    end

    assign out3 = sel[0] ? a :
                  sel[1] ? b :
                  sel[2] ? c : d;

    always @(*) begin
        if (en) begin
            if (sel[0])      out4 = e;
            else if (sel[1]) out4 = f;
            else             out4 = g;
        end else begin
            out4 = 8'hFF;
        end
    end

    always @(*) begin
        case (sel)
            3'b001:  out5 = a;
            3'b010:  out5 = 8'h11;
            3'b011:  out5 = 8'h22;
            3'b100:  out5 = 8'h33;
            3'b101:  out5 = 8'h44;
            3'b110:  out5 = 8'h55;
            3'b111:  out5 = 8'h66;
            default: out5 = 8'h00;
        endcase
    end

endmodule
)";

static void run_self_test()
{
    log_header(nullptr, "PMUX_RECOGNIZE self-test.\n");

    std::string tmpfile = "/tmp/_pmux_selftest.v";
    {
        FILE* f = fopen(tmpfile.c_str(), "w");
        if (!f) {
            log_error("Cannot write temp file for self-test.\n");
            return;
        }
        fprintf(f, "%s", self_test_verilog);
        fclose(f);
    }

    RTLIL::Design* design = new RTLIL::Design;
    {
        std::ifstream ff(tmpfile);
        if (!ff.is_open()) {
            log_error("Cannot open temp file.\n");
            delete design;
            return;
        }
        Frontend::frontend_call(design, &ff, tmpfile, "verilog");
    }

    Pass::call(design, "hierarchy -top test_pmux");
    Pass::call(design, "proc");
    Pass::call(design, "opt_clean");
    Pass::call(design, "opt_muxtree");
    Pass::call(design, "opt_clean");

    PmuxConfig config;
    for (auto module : design->modules()) {
        if (module->get_blackbox_attribute())
            continue;

        PmuxAnalyzer analyzer(module, config);
        analyzer.run();

        std::string report = generate_report(module, analyzer.result);
        log("%s", report.c_str());
    }

    delete design;
    log("Self-test complete.\n");
}

// ============================================================
//  Pass 定义
// ============================================================

struct PmuxRecognizePass : public Pass {

    PmuxRecognizePass() : Pass("pmux_recognize",
        "recognize and classify priority MUX structures") {}

    void help() override
    {
        log("\n");
        log("    pmux_recognize [options] [selection]\n");
        log("\n");
        log("Recognize and classify Priority MUX (PMUX) structures.\n");
        log("\n");
        log("    -report <file>\n");
        log("        Write text report to file (overwrite).\n");
        log("\n");
        log("    -json <file>\n");
        log("        Write JSON output to file (overwrite).\n");
        log("\n");
        log("    -annotate\n");
        log("        Set attributes on identified cells.\n");
        log("\n");
        log("    -wide-threshold <N>\n");
        log("        Branch count threshold for 'wide' class (default: 8).\n");
        log("\n");
        log("    -sparse-ratio <F>\n");
        log("        Ratio threshold for 'sparse' class (default: 0.5).\n");
        log("\n");
        log("    -max-depth <N>\n");
        log("        Max MUX chain depth to trace (default: 32).\n");
        log("\n");
        log("    -self-test\n");
        log("        Run built-in self-test with sample Verilog.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design* design) override
    {
        PmuxConfig config;
        std::string report_file, json_file;
        bool self_test = false;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-report" && argidx + 1 < args.size()) {
                report_file = args[++argidx];
            } else if (args[argidx] == "-json" && argidx + 1 < args.size()) {
                json_file = args[++argidx];
            } else if (args[argidx] == "-annotate") {
                config.annotate = true;
            } else if (args[argidx] == "-wide-threshold" && argidx + 1 < args.size()) {
                config.wide_threshold = atoi(args[++argidx].c_str());
            } else if (args[argidx] == "-sparse-ratio" && argidx + 1 < args.size()) {
                config.sparse_ratio = (float)atof(args[++argidx].c_str());
            } else if (args[argidx] == "-max-depth" && argidx + 1 < args.size()) {
                config.max_chain_depth = atoi(args[++argidx].c_str());
            } else if (args[argidx] == "-self-test") {
                self_test = true;
            } else {
                break;
            }
        }

        if (self_test) {
            run_self_test();
            return;
        }

        extra_args(args, argidx, design);

        FILE* report_fp = nullptr;
        FILE* json_fp = nullptr;

        if (!report_file.empty()) {
            report_fp = fopen(report_file.c_str(), "w");
            if (!report_fp)
                log_error("Cannot open report file: %s\n", report_file.c_str());
        }

        if (!json_file.empty()) {
            json_fp = fopen(json_file.c_str(), "w");
            if (!json_fp)
                log_error("Cannot open JSON file: %s\n", json_file.c_str());
        }

        for (auto module : design->selected_modules()) {
            if (module->get_blackbox_attribute())
                continue;

            PmuxAnalyzer analyzer(module, config);
            analyzer.run();

            std::string report = generate_report(module, analyzer.result);
            log("%s", report.c_str());

            if (report_fp) fprintf(report_fp, "%s", report.c_str());
            if (json_fp)   write_json(analyzer.result, module, json_fp);

            if (config.annotate) {
                for (auto& p : analyzer.result.items) {
                    if (p.origin_cell) {
                        p.origin_cell->attributes[ID(pmux_class)] =
                            RTLIL::Const(cls_str(p.classification));
                        p.origin_cell->attributes[ID(pmux_encoding)] =
                            RTLIL::Const(enc_str(p.encoding));
                        p.origin_cell->attributes[ID(pmux_id)] =
                            RTLIL::Const(p.id, 32);
                    }

                    for (int i = 0; i < GetSize(p.chain_cells); i++) {
                        RTLIL::Cell* c = p.chain_cells[i];
                        c->attributes[ID(pmux_member)]   = RTLIL::Const(1, 32);
                        c->attributes[ID(pmux_id)]       = RTLIL::Const(p.id, 32);
                        c->attributes[ID(pmux_pos)]      = RTLIL::Const(i, 32);
                        c->attributes[ID(pmux_class)]    = RTLIL::Const(cls_str(p.classification));
                        c->attributes[ID(pmux_encoding)] = RTLIL::Const(enc_str(p.encoding));
                    }
                    if (!p.chain_cells.empty()) {
                        p.chain_cells[0]->attributes[ID(pmux_head)] =
                            RTLIL::Const(1, 32);
                    }
                }
            }
        }

        if (report_fp) { fclose(report_fp); log("Report written to: %s\n", report_file.c_str()); }
        if (json_fp)   { fclose(json_fp);   log("JSON written to: %s\n", json_file.c_str()); }
    }

} PmuxRecognizePass;

PRIVATE_NAMESPACE_END