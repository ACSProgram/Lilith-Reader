// settings_ui.cpp — Lilith Reader 应用层：设置窗口（自 ui.cpp 抽出，架构加固第五批）
//
// 设置窗口曾是 ui.cpp 里最大的一块自成体系绘制：四个分栏（界面 / 性能 / 按键 / 阅读数据）、
// 阅读数据卡片的破坏性按钮、按键改键捕获、开合与淡入淡出。把它拆成独立 TU，让 ui.cpp 回到
// "外壳 + 画布 + 侧栏 + 各弹窗"的本职，避免绘制与设置逻辑再度混成一坨。
//
// 依赖：只经 app_internal.h 的共享声明（含 ui.cpp 的 kThemeNames/kPageSchemeNames、col_dim/
// col_warn、set_theme_pref/apply_gap_pref）；本 TU 与 ui.cpp 互不 include。对外入口只有
// `draw_settings_window()`（由 ui.cpp 的 draw_shell 调用），其余函数仅本 TU 内部使用。

#include "app_internal.h"

#include "imgui_raii.h"   // ImGui 栈的 RAII 包装（Push/Pop · Begin/End 自动配对）

namespace lr::app {
//
// 浮动窗口的默认尺寸与位置：期望值按视口上限钳制后居中，小屏/低分辨率下也不超出屏幕。
ImVec2 clamp_to_viewport(const ImGuiViewport* vp, const ImVec2& want) {
    return ImVec2(std::min(want.x, vp->Size.x * 0.92f),
                  std::min(want.y, vp->Size.y * 0.90f));
}
ImVec2 centered_on_viewport(const ImGuiViewport* vp, const ImVec2& size) {
    return ImVec2(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                  vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
}

// ---------------- 设置 · 阅读数据分栏（ADR-062/063/065）----------------
//
// 一条记录 = 一份**阅读数据**（读到哪 / 书签 / 视图参数）。同一份内容出现在多个位置时只占
// 一条 —— 位置在卡片里列成"路径树"，这正是"一份数据、若干路径"的表达。此前一条记录只留
// 最后一个路径，复制出来的第二份会把第一份顶掉，看不出这是同一份文档的两处副本。
//
// 版式：卡片（圆角底 + 悬停描边）= 标题行（文档名 + 行尾删除按钮）/ 统计行（第 N / M 页 ·
// K 个书签 · J 个位置）/ 缩进的路径行（左侧一条竖导引线，把它们挂在标题下面）。
//
// 三个刻意的取舍：
//   · **不用 TreeNode**：那会带一个三角与整行选中态，而路径是只读信息、不该看起来可点；
//     一条安静的竖导引线更像"附属关系"，也更省横向空间。
//   · **路径单行中间省略，不折行**：折行会让卡片高度参差，且长路径开头往往是同样的前缀，
//     堆在一起看着像糊成一团；中间省略同时保住父目录与文件名，完整路径留给悬停提示。
//   · **"位置未知"的旧记录单独成区**：v1 记录（升级前）没存过路径，硬摆在列表里就是一堆
//     "未知文档"。它们挪到末尾、附一句解释，要么等再次打开那个文件时按路径键被认领，
//     要么一键清掉。
namespace {

// 路径 → 文件名（UTF-8 进出）；空路径返回空串。
std::string file_name_of_u8(const std::string& path_u8) {
    if (path_u8.empty()) return {};
    return lr::wide_to_utf8(lr::file_name_of(lr::utf8_to_wide(path_u8)));
}

// UTF-8 每个字符的起始字节下标（末尾补 size）。截断必须切在字符边界上，
// 否则半个汉字会被画成豆腐块。
std::vector<std::size_t> utf8_char_offsets(const std::string& s) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < s.size();) {
        v.push_back(i);
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    }
    v.push_back(s.size());
    return v;
}

// 放不下就加省略号（middle：掐中间，保留头 55% / 尾 45%；否则掐尾）。
// 二分"最多留几个字符"，每步两次 CalcTextSize，与串长无关地收敛到 log(n)。
std::string ellipsize(const std::string& s, float max_w, bool middle) {
    if (s.empty()) return s;
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w) return s;
    const char* kEll = "…";
    const float budget = max_w - ImGui::CalcTextSize(kEll).x;
    if (budget <= 0.0f) return kEll;

    const std::vector<std::size_t> off = utf8_char_offsets(s);
    const std::size_t chars = off.size() - 1;
    const auto width_of = [&](std::size_t keep) {
        const std::size_t head = middle ? keep * 55 / 100 : keep;
        const std::size_t tail = middle ? keep - head : 0;
        return ImGui::CalcTextSize(s.substr(0, off[head]).c_str()).x +
               ImGui::CalcTextSize(s.substr(off[chars - tail]).c_str()).x;
    };
    std::size_t lo = 0, hi = chars;
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (width_of(mid) <= budget) lo = mid; else hi = mid - 1;
    }
    if (lo == 0) return kEll;
    const std::size_t head = middle ? lo * 55 / 100 : lo;
    const std::size_t tail = middle ? lo - head : 0;
    return s.substr(0, off[head]) + kEll + s.substr(off[chars - tail]);
}

// 危险动作的按钮配色（无底色 → 悬停浮出红）：删除图标与"清空 / 清理"文字按钮共用。
// 红色属**语义色**，tone_apply 只把它的色相往方案色相拉近一个小比例，保住"红=危险"。
ImVec4 danger_ink() {
    return tone_apply(g_app.dark_theme ? ImVec4(0.95f, 0.47f, 0.45f, 1.0f)
                                   : ImVec4(0.76f, 0.22f, 0.19f, 1.0f));
}
[[nodiscard]] ig::StyleColor4 danger_button_style(bool hovered) {
    const ImVec4 red = danger_ink();
    return ig::StyleColor4(ImGuiCol_Button, ImVec4(0, 0, 0, 0),
                           ImGuiCol_ButtonHovered, ImVec4(red.x, red.y, red.z, 0.14f),
                           ImGuiCol_ButtonActive, ImVec4(red.x, red.y, red.z, 0.26f),
                           ImGuiCol_Text, hovered ? red : col_dim());
}

// 行尾的删除按钮：默认无底色，悬停才浮出危险色。
// 刻意不用 ImGui 的默认按钮——那在浅色主题下就是一块灰方块（人工反馈"按钮很丑"）。
bool danger_icon_button(float x, float y, const char* tip) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 b0(x, y), b1(x + h, y + h);
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(b0, b1);
    bool clicked = false;
    {
        const ig::StyleColor4 danger = danger_button_style(hov);
        ImGui::SetCursorScreenPos(b0);
        clicked = ImGui::Button(g_app.icons_ok ? kIcTrash : "×", ImVec2(h, h));
    }
    if (tip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 破坏性动作的文字按钮（清空全部 / 清理旧记录）：同一套"无底色、悬停变红"的观感。
bool danger_text_button(const char* label) {
    const ImVec2 sz = ImGui::CalcTextSize(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = sz.x + st.FramePadding.x * 2.0f;
    const float h = sz.y + st.FramePadding.y * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + w, p0.y + h));
    const ig::StyleColor4 danger = danger_button_style(hov);
    return ImGui::Button(label);
}

// 一张文档卡：标题行 + 统计行 + 若干路径行。
void draw_reading_data_card(std::size_t i) {
    const std::uint64_t key = g_app.session.state.docs[i].first;
    const lr::DocRecord& r = g_app.session.state.docs[i].second;

    const float pad     = px(10.0f);
    const float row_gap = px(5.0f);
    const float line    = ImGui::GetTextLineHeight();
    const float row_h   = line + px(2.0f);
    const float indent  = px(16.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    constexpr std::size_t kMaxRows = 3;   // 最多铺 3 行路径，多出来的折成"还有 N 处…"
    const bool more = r.locations.size() > kMaxRows;
    const std::size_t shown = more ? kMaxRows : r.locations.size();
    const std::size_t rows = shown + (more ? 1u : 0u);
    const float card_h = pad * 2.0f + line * 2.0f + row_gap + px(6.0f) +
                         static_cast<float>(rows) * row_h;
    const ImVec2 p1(p0.x + w, p0.y + card_h);
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(p0, p1);

    // ID 用作用域对象持有：卡片体很长，任何一处提前退出/异常都不该把 ID 留在栈上。
    ig::Id card_id(static_cast<int>(i));
    ImGui::Dummy(ImVec2(w, card_h));   // 先占位撑开整张卡；后面的元素都按绝对坐标摆

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool dark = g_app.dark_theme;
    dl->AddRectFilled(p0, p1, dark ? IM_COL32(255, 255, 255, hov ? 16 : 8)
                                   : IM_COL32(0, 0, 0, hov ? 10 : 5), px(7.0f));
    dl->AddRect(p0, p1,
                hov ? g_app.pal.accent
                    : (dark ? IM_COL32(255, 255, 255, 26) : IM_COL32(0, 0, 0, 18)),
                px(7.0f), 0, px(1.0f));

    const std::string title = file_name_of_u8(r.locations.front());
    const float btn = ImGui::GetFrameHeight();

    // 标题行：文档名（左） + 删除按钮（右，垂直居中于标题行）
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
    ImGui::TextColored(col_text(), "%s",
                       ellipsize(title, w - pad * 2.0f - btn - px(8.0f), false).c_str());
    if (danger_icon_button(p1.x - pad - btn, p0.y + pad + (line - btn) * 0.5f,
                           "删除这份阅读数据")) {
        request_confirm(ConfirmKind::ClearOne, "删除这份阅读数据？",
                        "将删除它的阅读位置与书签，文档本身不受影响。",
                        { { "文档", title }, { "最近位置", r.locations.front() } },
                        "删除", "取消", key);
    }

    // 统计行
    char meta[192];
    std::size_t n = static_cast<std::size_t>(std::snprintf(
        meta, sizeof meta, "第 %d 页", r.page + 1));
    if (r.page_count > 0 && n < sizeof meta)
        n += static_cast<std::size_t>(std::snprintf(
            meta + n, sizeof meta - n, " / %d", r.page_count));
    if (n < sizeof meta)
        std::snprintf(meta + n, sizeof meta - n, " · %d 个书签 · %d 个位置",
                      static_cast<int>(r.bookmarks.size()),
                      static_cast<int>(r.locations.size()));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad + line + row_gap));
    ImGui::TextColored(col_dim(), "%s", meta);

    // 路径行：左侧一条竖导引线 + 每行一个短横，把若干位置挂在标题下面
    const float x_rule = p0.x + pad + px(6.0f);
    const float x_text = p0.x + pad + indent;
    const float y0 = p0.y + pad + line * 2.0f + row_gap + px(6.0f);
    const ImVec4 dim4 = col_dim();
    const ImU32 rule = ImGui::ColorConvertFloat4ToU32(
        ImVec4(dim4.x, dim4.y, dim4.z, dim4.w * 0.40f));
    if (rows >= 2)   // 只有一条位置时不画竖线，那一段零长度的线毫无意义
        dl->AddLine(ImVec2(x_rule, y0 + row_h * 0.5f),
                    ImVec2(x_rule, y0 + row_h * (static_cast<float>(rows) - 0.5f)),
                    rule, px(1.0f));
    for (std::size_t k = 0; k < shown; ++k) {
        const float y = y0 + row_h * static_cast<float>(k);
        dl->AddLine(ImVec2(x_rule, y + row_h * 0.5f),
                    ImVec2(x_rule + px(6.0f), y + row_h * 0.5f), rule, px(1.0f));
        ImGui::SetCursorScreenPos(ImVec2(x_text, y));
        const std::string cut = ellipsize(r.locations[k], p1.x - pad - x_text, true);
        ImGui::TextColored(col_dim(), "%s", cut.c_str());
        // 被省略的完整路径给悬停提示（用鼠标位置判定，不依赖文本项有没有 id）
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (cut != r.locations[k] && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseHoveringRect(p0, p1))
            ImGui::SetTooltip("%s", r.locations[k].c_str());
    }
    if (more) {
        ImGui::SetCursorScreenPos(ImVec2(x_text, y0 + row_h * static_cast<float>(shown)));
        ImGui::TextColored(col_dim(), "还有 %d 处…",
                           static_cast<int>(r.locations.size() - shown));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseHoveringRect(p0, p1)) {
            std::string rest;
            for (std::size_t k = shown; k < r.locations.size(); ++k) {
                rest += r.locations[k];
                rest += '\n';
            }
            if (!rest.empty()) rest.pop_back();
            ImGui::SetTooltip("%s", rest.c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + px(9.0f)));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));   // 收尾：让下一张卡从这里开始
    card_id.dismiss();
}

// 位置未知的旧记录（升级前的 v1 记录没存过路径，永远显示不出文档名）单独成区：
// 说清它们是什么、什么时候会被认领，并给一个只清这一类的入口。
void draw_unknown_records(const std::vector<std::size_t>& idx) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(col_text(), "位置未知的旧记录 · %d 条", static_cast<int>(idx.size()));
    ImGui::TextColored(col_dim(), "%s", "升级前的记录没有保存过文件路径，认不出是哪份文档；");
    ImGui::TextColored(col_dim(), "%s", "再次打开那个文件时，它会自动认领这里的一条。");
    ImGui::Spacing();

    const float line = ImGui::GetTextLineHeight();
    const float btn = ImGui::GetFrameHeight();
    const float h = line > btn ? line : btn;
    const float w = ImGui::GetContentRegionAvail().x;
    for (const std::size_t i : idx) {
        const std::uint64_t key = g_app.session.state.docs[i].first;
        const lr::DocRecord& r = g_app.session.state.docs[i].second;
        ig::Id row_id(static_cast<int>(i));

        char text[128];
        if (r.page_count > 0)
            std::snprintf(text, sizeof text, "第 %d / %d 页 · %d 个书签",
                          r.page + 1, r.page_count, static_cast<int>(r.bookmarks.size()));
        else
            std::snprintf(text, sizeof text, "第 %d 页 · %d 个书签",
                          r.page + 1, static_cast<int>(r.bookmarks.size()));

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p0.x + px(6.0f), p0.y + (h - line) * 0.5f));
        ImGui::TextColored(col_dim(), "%s", text);
        if (danger_icon_button(p0.x + w - btn, p0.y, "删除这条旧记录")) {
            request_confirm(ConfirmKind::ClearOne, "删除这条阅读数据？",
                            "将删除它的阅读位置与书签，文档本身不受影响。",
                            { { "记录", text } }, "删除", "取消", key);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + px(2.0f)));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        row_id.dismiss();
    }

    ImGui::Spacing();
    char label[64];
    std::snprintf(label, sizeof label, "清理这 %d 条", static_cast<int>(idx.size()));
    if (danger_text_button(label)) {
        char body[224];
        std::snprintf(body, sizeof body,
                      "将删除这 %d 条认不出文档的记录（阅读位置与书签）。\n"
                      "文档本身不受影响，其余记录也不受影响。",
                      static_cast<int>(idx.size()));
        request_confirm(ConfirmKind::PruneUnknown, "清理位置未知的旧记录？", body, {},
                        "清理", "取消", 0);
    }
}

}  // namespace

// ---- 设置项表格（左列标签 + 右列控件）----
//
// 原先是一对 settings_rows_begin()/settings_rows_end()：前者 PushStyleVar + BeginTable、
// 后者 EndTable + PopStyleVar。**跨函数的配对**在这里尤其危险 —— 中间隔着整段分栏内容，
// 任何一处提前 return 都会把 CellPadding 留在栈上（此后所有表格的行距都变窄，且看不出是谁漏的）。
// 改成一个作用域对象：构造即开表、析构即收表，配对由语言保证。
//
// CellPadding 收窄：控件自身已有 FramePadding，行再留 6px 会显得空、且撑高窗口。
// 直接组合 ig::StyleVar + ig::Table：成员按声明序构造、逆序析构，因此"先 PopStyleVar 后
// EndTable"的顺序天然正确 —— 不需要在这里再手写一次配对逻辑。
class SettingsRows {
public:
    explicit SettingsRows(const char* id)
        : pad_(ImGuiStyleVar_CellPadding, ImVec2(px(9.0f), px(3.0f))),
          table_(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX) {
        if (table_) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, px(136.0f));
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        }
    }
    SettingsRows(const SettingsRows&) = delete;
    SettingsRows& operator=(const SettingsRows&) = delete;
    SettingsRows(SettingsRows&&) = delete;
    SettingsRows& operator=(SettingsRows&&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(table_); }

private:
    ig::StyleVar pad_;
    ig::Table    table_;
};

void settings_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(px(190.0f));
}
void settings_note(const char* text) {
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_app.pal.chrome_dim), "%s", text);
}

void draw_settings_reading_data_tab() {
    // 顶部一行：智能匹配三态。它决定"文件换位置后要不要沿用同一份阅读数据"，
    // 与这一栏是同一件事 —— 放这儿，免得「阅读」栏里再出现一句"见阅读数据分栏"。
    if (const SettingsRows rows = SettingsRows("##set_rd_head")) {
        settings_row("智能匹配");
        const char* smart[] = { "关（只认路径）", "询问", "自动沿用" };
        if (ImGui::Combo("##smart", &g_app.prefs.smart_match, smart, IM_ARRAYSIZE(smart)))
            save_prefs();
        settings_note("换位置后沿用同一份阅读数据");
    }
    ImGui::Spacing();

    // 底部操作条**不参与滚动**：列表用负高度，剩下的一行留给「清空全部」+ 计数
    const float footer_h = ImGui::GetFrameHeightWithSpacing();

    std::vector<std::size_t> known, unknown;
    for (std::size_t i = 0; i < g_app.session.state.docs.size(); ++i)
        (g_app.session.state.docs[i].second.locations.empty() ? unknown : known).push_back(i);

    {
        const ig::Child rd_list("##rd_list", ImVec2(0, -footer_h),
                                ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
        if (g_app.session.state.docs.empty()) {
            ImGui::TextColored(col_dim(), "%s", "还没有记录任何阅读数据。");
            ImGui::TextColored(col_dim(), "%s", "读到哪、书签和视图参数会在关闭文档时自动记下。");
        }
        for (const std::size_t i : known) draw_reading_data_card(i);
        if (!unknown.empty()) draw_unknown_records(unknown);
    }   // 列表子窗口在这里收口（footer 一行必须留在它外面）

    char count[64];
    std::snprintf(count, sizeof count, "共 %d 份文档", static_cast<int>(known.size()));
    {
        const ig::Disabled d(g_app.session.state.docs.empty());
        if (danger_text_button("清空全部")) {
            char body[176];
            std::snprintf(body, sizeof body,
                          "将删除全部 %d 条阅读数据（阅读位置与书签）。\n文档本身不受影响。",
                          static_cast<int>(g_app.session.state.docs.size()));
            request_confirm(ConfirmKind::ClearAll, "清空全部阅读数据？", body, {},
                            "清空", "取消", 0);
        }
    }
    const float cw = ImGui::CalcTextSize(count).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - cw - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextColored(col_dim(), "%s", count);
}

// ---- 设置窗口（分栏：界面 / 性能 / 按键 / 阅读数据） ----

void reset_prefs_to_default() {
    g_app.prefs = UiPrefs{};
    g_app.user_scale = g_app.prefs.ui_scale;
    g_app.apply_scale_pending = true;
    g_app.renderer->set_resource_tier(static_cast<lr::ResourceTier>(g_app.prefs.resource_tier));
    apply_gap_pref();
    save_prefs();
}


void draw_settings_interface_tab() {
    if (const SettingsRows rows = SettingsRows("##set_ui")) {
        settings_row("界面缩放");
        int pct = static_cast<int>(std::lround(g_app.prefs.ui_scale * 100.0f));
        if (ImGui::SliderInt("##uiscale", &pct, 80, 150, "%d%%")) {
            g_app.prefs.ui_scale = pct / 100.0f;
            g_app.user_scale = g_app.prefs.ui_scale;
            g_app.apply_scale_pending = true;   // 样式改动延到下一帧首（不在帧中途换样式）
            save_prefs();
        }
        settings_note("80% ~ 150%");

        settings_row("界面主题");
        int theme = g_app.prefs.theme;
        if (ImGui::Combo("##theme", &theme, kThemeNames, IM_ARRAYSIZE(kThemeNames)))
            set_theme_pref(theme);

        settings_row("纸张方案");
        const bool reading = g_app.session.doc.kind == UiDoc::Kind::Reading;
        int cm = g_app.session.scheme;
        {
            const ig::Disabled d(!reading);
            if (ImGui::Combo("##scheme", &cm, kPageSchemeNames, IM_ARRAYSIZE(kPageSchemeNames)))
                set_scheme(cm);
        }
        settings_note(reading ? "随当前文档记忆" : "打开文档后可用");

        settings_row("联动规则");
        ImGui::TextWrapped("仅“跟随系统”时，深色纸张会额外启用深色界面；手动选浅色或深色则固定明暗。纸张方案随当前文档记忆，并协调界面色调。");

        settings_row("顶栏自动隐藏");
        if (ImGui::Checkbox("##autohide", &g_app.prefs.auto_hide_toolbar)) {
            save_prefs();
            g_app.toolbar_visible = true;
        }
        settings_note("阅读时收起，鼠标移到窗口顶部即重现");

        settings_row("界面动效");
        if (ImGui::Checkbox("##motion", &g_app.prefs.motion)) save_prefs();
        settings_note("页面淡入 / 滚动缩放平滑 / 顶栏与侧栏滑动 / 窗口淡入淡出");

        // 「页面间距」原先单列「阅读」分栏，现并入本栏（分栏合并）：它同样是"看着舒服"
        // 的观感项，与界面缩放/动效同属一类，单独占一栏显得空。
        settings_row("页面间距");
        float gp = g_app.prefs.gap_percent;
        if (ImGui::SliderFloat("##gap", &gp, 0.0f, 6.0f, "%.1f%%")) {
            g_app.prefs.gap_percent = gp;
            apply_gap_pref();
            save_prefs();
        }
        settings_note("页与页之间的留白比例（列宽的百分比）");
    }
}

void draw_settings_performance_tab() {
    if (const SettingsRows rows = SettingsRows("##set_perf")) {
        settings_row("资源档位");
        const char* tiers[] = { "低", "中", "高" };
        if (ImGui::Combo("##resource_tier", &g_app.prefs.resource_tier, tiers, IM_ARRAYSIZE(tiers))) {
            g_app.renderer->set_resource_tier(static_cast<lr::ResourceTier>(g_app.prefs.resource_tier));
            save_prefs();
        }
        const lr::ResourceProfile profile = g_app.renderer->resource_profile();
        char note[96];
        std::snprintf(note, sizeof note, "缓存上限 %zu MB · tile %d px",
                      profile.cache_bytes / (1024ull * 1024ull), profile.tile_size_px);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_app.pal.chrome_dim), "%s", note);
        ImGui::SetItemTooltip("缓存按需占用，不会启动时一次性分配；只保留当前页和预加载页，超出上限按最近使用顺序逐出。\n当前渲染队列使用单个 MuPDF worker。\n低 / 中 / 高：256 / 512 / 768 MB。\n大页面 tile 单边：1536 / 2048 / 2560 px。");
    }
}

// ---- 按键分栏（ADR-054）----
//
// 每条命令一行：命令名 + 两个按键框（主键 / 备键）。左键点框进入捕获（"按下按键…"），
// 按下的第一个键即写入该槽；右键清除（捕获中则取消）。同一键被多条命令使用会以警示色
// 标出并给出冲突对象——**只提示不阻止**：有人确实想让一个键在不同状态下做不同事。
//
// 修饰键型命令（kCmds[i].mod_only，"滚轮缩放"）**不是一条被按下的命令**，而是"按住不放的
// 键"，故不用捕获框，改用下拉框（无 / Ctrl / Alt / Shift / Ctrl+Alt）。
//
// 分栏末尾还有一节**鼠标与固定键**：这些操作改不了（不是命令），但用户必须能查到 ——
// 早期只列可自定义的命令，结果"Ctrl+滚轮能缩放""右键有菜单"这类事全靠猜（人工反馈）。

// 滚轮缩放修饰键的候选（值与 ini 里的存储串一一对应，见 chord_to_string）。
const char* const kModLabels[] = { "无", "Ctrl", "Alt", "Shift", "Ctrl+Alt" };
const ImGuiKeyChord kModValues[] = { 0, ImGuiMod_Ctrl, ImGuiMod_Alt, ImGuiMod_Shift,
                                     ImGuiMod_Ctrl | ImGuiMod_Alt };
constexpr int kModCount = static_cast<int>(IM_ARRAYSIZE(kModValues));

void draw_bind_button(int cmd, int slot) {
    const bool capturing = (g_app.capture_cmd == cmd && g_app.capture_slot == slot);
    const ImGuiKeyChord c = g_app.binds[cmd][slot];
    const std::string label = capturing ? std::string("按下按键…")
                                        : (c == ImGuiKey_None ? std::string("未设置")
                                                              : chord_label(c));
    const ig::Id slot_id(slot);
    // 捕获态的高亮底色：只在捕获期间压栈（非捕获态用空对象表达"不压"）。
    const ImVec4 capture_bg = tone_apply(g_app.dark_theme ? ImVec4(0.42f, 0.65f, 0.94f, 0.35f)
                                                      : ImVec4(0.23f, 0.49f, 0.85f, 0.28f));
    const bool clicked = [&] {
        if (capturing) {
            const ig::StyleColor highlight(ImGuiCol_Button, capture_bg);
            return ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0));
        }
        return ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0));
    }();
    if (clicked) {
        g_app.capture_cmd = cmd;
        g_app.capture_slot = slot;
        g_app.capture_frame = ImGui::GetFrameCount();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (capturing) {
            g_app.capture_cmd = kCmdCount;          // 右键取消捕获
        } else if (c != ImGuiKey_None) {
            g_app.binds[cmd][slot] = ImGuiKey_None; // 右键清除
            save_binds();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("左键点击后按下新键；右键清除 / 取消捕获");
}

int conflict_of(int cmd) {
    const int c0 = find_bind_conflict(cmd, 0);
    return c0 >= 0 ? c0 : find_bind_conflict(cmd, 1);
}

void draw_settings_keys_tab() {
    const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(g_app.pal.chrome_dim);
    if (ImGui::Button("恢复默认按键")) { reset_binds_to_default(); save_binds(); }
    ImGui::SameLine();
    if (g_app.capture_cmd < kCmdCount)
        ImGui::TextColored(col_warn(), "正在捕获按键…（点别处或右键取消）");
    else
        ImGui::TextColored(dim, "点按键框后按下新键；右键清除。下拉框直接选。");
    ImGui::Separator();

    const ig::Child keys_list("##keys_list", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
    int i = 0;
    while (i < kCmdCount) {
        const char* group = kCmds[i].group;
        ImGui::SeparatorText(group);
        char tid[32];
        std::snprintf(tid, sizeof tid, "##keys_g%d", i);
        if (const ig::Table group_table = ig::Table(tid, 3, tf)) {
            ImGui::TableSetupColumn("cmd", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("k0", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
            ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
            for (; i < kCmdCount && std::strcmp(kCmds[i].group, group) == 0; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ig::Id row_id(i);
                ImGui::AlignTextToFramePadding();
                const int conflict = conflict_of(i);
                if (conflict >= 0) ImGui::TextColored(col_warn(), "%s", kCmds[i].name);
                else               ImGui::TextUnformatted(kCmds[i].name);
                if (conflict >= 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("与「%s」使用了同一个键", kCmds[conflict].name);
                if (kCmds[i].mod_only) {
                    // 修饰键型：下拉框选"按住哪个键"，第二列留给说明文字（没有备键）
                    const ImGuiKeyChord cur = g_app.binds[i][0] & ImGuiMod_Mask_;
                    int sel = 0;
                    for (int o = 0; o < kModCount; ++o)
                        if ((kModValues[o] & ImGuiMod_Mask_) == cur) { sel = o; break; }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(px(150.0f));
                    if (ImGui::Combo("##mod", &sel, kModLabels, kModCount)) {
                        g_app.binds[i][0] = kModValues[sel];
                        save_binds();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        ImGui::SetTooltip("按住此键滚动滚轮 = 缩放；不按则滚动页面");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(dim, "%s", "按住 + 滚轮");
                } else {
                    for (int s = 0; s < kBindSlots; ++s) {
                        ImGui::TableSetColumnIndex(1 + s);
                        draw_bind_button(i, s);
                    }
                }
                row_id.dismiss();
            }
        }
    }

    // ---- 鼠标与固定键（**不可自定义**的操作清单）----
    // 用户查不到就会以为"没有这个功能"：滚轮缩放、右键菜单、滚条拖拽都属于这一类。
    ImGui::SeparatorText("鼠标与固定键");
    {
        const ImGuiTableFlags rf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
        if (const ig::Table ref_table = ig::Table("##keys_ref", 2, rf)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(190.0f));
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            auto ref = [&](const char* key, const char* desc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(dim, "%s", desc);
            };
            const std::string mod = chord_label(g_app.binds[static_cast<int>(Cmd::ZoomWheelMod)][0]);
            const std::string wheel_zoom =
                mod.empty() ? std::string("滚轮缩放：未设置修饰键（当前不可用）")
                            : ("按住 " + mod + " + 滚轮");
            ref("滚轮", "上下滚动");
            ref(wheel_zoom.c_str(), "以鼠标位置为中心缩放（修饰键可改，见上）");
            ref("左键拖拽", "在文字上 = 选择文本；在非文字处 = 平移页面");
            ref("中键拖拽", "平移页面（任何位置都可用）");
            ref("左键单击", "点链接即打开；点文字处取消选区");
            ref("右键", "打开画布菜单（Shift+F10 / 菜单键同效）");
            ref("拖拽右侧滚动条 / 点轨道", "定位到该处");
            ref("单击「渲染失败」占位", "重试渲染该页");
            ref("鼠标移到窗口顶部", "顶栏自动隐藏后重新显示");
            ref("Esc", "关闭当前对话框（设置 / 跳页 / 密码 / 确认）");
        }
    }
}

// 设置窗口本体：**底栏固定、正文装分栏**（内容多时只滚动正文）。
//
// 打开/关闭 = **缩放 + 淡入淡出**（开合对称，ADR-056），进度用**定时补间 + 三次缓出**
// （ToggleAnim，160ms，ADR-061）—— 比一阶滞后利落：时长确定、末端干脆，无长尾。
// 以窗口中心为不动点，尺寸 96% ↔ 100%，同时整体透明度 0 ↔ 1。**关闭不做"整窗幽灵化淡出"** —— 原实现只降 alpha，
// 结果是窗口原地保持原尺寸、内容逐渐透明，既不像常见窗口动画、又留一个半透明大窗占着画面。
// 现在边缩边淡，读起来是"收回去"。设置是普通 ImGui 窗口（非 popup），ImGui 自身不做动画。
//
// 几何：动画进行中每帧强制驱动（Cond_Always）；静止后不再覆盖，位置交给 ImGui 记忆，
// 这样用户仍可拖动窗口 —— 拖动后的位置由 g_app.settings_rest_pos 每帧实测记录，下次开合
// 即以该位置为中心缩放，不会"跳回屏幕中央"。
void draw_settings_window() {
    const float dt = ImGui::GetIO().DeltaTime;
    if (!g_app.settings_anim.step(dt, g_app.show_settings, g_app.prefs.motion))
        return;   // 完全关闭：不再创建窗口
    const float a = g_app.settings_anim.value;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 want = clamp_to_viewport(vp, ImVec2(px(600.0f), px(700.0f)));
    if (!g_app.settings_rest_valid) {
        g_app.settings_rest_pos = centered_on_viewport(vp, want);
        g_app.settings_rest_valid = true;
    }

    const bool animating = g_app.settings_anim.active;
    const float s = kWindowScaleFrom + (1.0f - kWindowScaleFrom) * a;
    const ImVec2 sz(want.x * s, want.y * s);
    const ImVec2 pos(g_app.settings_rest_pos.x + (want.x - sz.x) * 0.5f,
                     g_app.settings_rest_pos.y + (want.y - sz.y) * 0.5f);

    // 尺寸始终由我们驱动（NoResize，用户改不了）；位置仅在动画中强制，
    // 静止后用 Cond_Appearing —— 只在窗口出现那一帧落位，之后交给 ImGui（可拖动）。
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, animating ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(a);
    const ig::StyleVar win_alpha(ImGuiStyleVar_Alpha, a);

    bool keep = true;   // 每帧从 true 起算：真正的关闭由 g_app.show_settings 驱动，便于淡出
    const ig::Window settings_win("设置##settings", &keep,
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoSavedSettings);
    if (settings_win) {
        if (!animating) {
            // 记住用户拖动后的位置。只在**真的移动过**（与基准差 > 1px）时更新，
            // 否则每次收尾那一帧的亚像素误差会被记成新基准、逐次累积漂移。
            const ImVec2 actual = ImGui::GetWindowPos();
            if (std::fabs(actual.x - g_app.settings_rest_pos.x) > 1.0f ||
                std::fabs(actual.y - g_app.settings_rest_pos.y) > 1.0f)
                g_app.settings_rest_pos = actual;
        }
        const int want_tab = g_app.settings_open_tab;   // F1 等入口指定的分栏，用完即复位
        g_app.settings_open_tab = -1;

        const float footer_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        // 正文子窗口必须在这里收口，底部操作条要留在它**外面**（否则会跟着内容一起滚动）。
        {
            const ig::Child body("##settings_body", ImVec2(0, -footer_h), false,
                                 ImGuiWindowFlags_NoNav);
            if (const ig::TabBar tabs = ig::TabBar("##settings_tabs")) {
                if (const ig::TabItem t = ig::TabItem(
                        "界面", nullptr, want_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    draw_settings_interface_tab();
                }
                if (const ig::TabItem t = ig::TabItem(
                        "性能", nullptr, want_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    draw_settings_performance_tab();
                }
                if (const ig::TabItem t = ig::TabItem(
                        "按键", nullptr, want_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    draw_settings_keys_tab();
                }
                if (const ig::TabItem t = ig::TabItem(
                        "阅读数据", nullptr, want_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    draw_settings_reading_data_tab();
                }
            }
        }

        // 底部操作条**固定在窗口底部**（不随内容滚动）：否则内容稍多时按钮会被挤出滚动区。
        ImGui::Separator();
        if (ImGui::Button("恢复默认设置")) reset_prefs_to_default();
        const float close_w = ImGui::CalcTextSize("关闭").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - close_w - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("关闭")) g_app.show_settings = false;
    }
    if (!keep) g_app.show_settings = false;
}
}  // namespace lr::app
