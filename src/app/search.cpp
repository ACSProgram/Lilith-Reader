// search.cpp — Lilith Reader 应用层：全文搜索
//
// 从 session.cpp 拆出（ADR-087）。检索本身在渲染工作线程执行（render 的 SearchJob），
// 本 TU 只负责：输入防抖、取用增量命中、跳转命中、清空/取消。
// 搜索结果与输入状态仍在 AppContext（g_app.search_*，ADR-086）。

#include "app_internal.h"

namespace lr::app {

// ---------------- 全文搜索 ----------------

bool search_has_query() { return g_app.search_buf[0] != '\0'; }

void search_clear() {
    g_app.search_hits.clear();
    g_app.search_cur = -1;
    g_app.search_scanned = 0;
    g_app.search_total = 0;
    g_app.search_active = false;
    g_app.search_truncated = false;
    g_app.search_scroll_pending = false;
    g_app.search_committed.clear();
    g_app.search_pending = false;
    g_app.search_edit_at = -1.0;
    g_app.search_seen[0] = '\0';
    if (g_app.renderer != nullptr) g_app.renderer->cancel_search();
}

void search_start() {
    const std::string needle(g_app.search_buf);
    g_app.search_hits.clear();
    g_app.search_cur = -1;
    g_app.search_scanned = 0;
    g_app.search_truncated = false;
    g_app.search_scroll_pending = false;
    // 已经发起检索：防抖计时作废，否则 0.4s 后会被"自动检索"再发一次同样的查询。
    g_app.search_pending = false;
    g_app.search_edit_at = -1.0;
    g_app.search_committed = needle;
    if (g_app.renderer == nullptr) return;
    if (needle.empty()) {
        g_app.search_active = false;
        g_app.search_total = 0;
        g_app.renderer->cancel_search();
        // 空关键字**要说话**：否则用户按了回车什么都没发生，只会得出"搜索无效"的结论。
        show_toast("请输入要查找的关键字");
        return;
    }
    g_app.renderer->start_search(needle);
    const lr::SearchStatus st = g_app.renderer->search_status();
    g_app.search_total = st.total;
    g_app.search_active = st.active;
}

void search_cancel() {
    // 取消的产品语义是收场：输入框、在途任务、结果列表和画布高亮一起清掉。
    std::memset(g_app.search_buf, 0, sizeof g_app.search_buf);
    g_app.search_focus = false;
    search_clear();
}

void search_goto(int index) {
    if (index < 0 || index >= static_cast<int>(g_app.search_hits.size())) return;
    g_app.search_cur = index;
    g_app.search_scroll_pending = true;
}

void search_step_hit(int dir) {
    const int n = static_cast<int>(g_app.search_hits.size());
    if (n <= 0) return;
    int idx = (g_app.search_cur < 0) ? (dir >= 0 ? 0 : n - 1) : g_app.search_cur + dir;
    if (idx < 0) idx = n - 1;         // 环绕
    if (idx >= n) idx = 0;
    search_goto(idx);
}

void update_search() {
    if (g_app.renderer == nullptr) return;

    // ---- 1) 输入防抖：关键字一变就作废旧查询，停手 kSearchDebounceSec 后自动检索 ----
    // 必须放在"取新命中"之前：本帧若判定用户改了字，就不该再把旧查询的命中并进列表。
    if (std::strcmp(g_app.search_buf, g_app.search_seen) != 0) {
        std::snprintf(g_app.search_seen, sizeof g_app.search_seen, "%s", g_app.search_buf);
        g_app.search_edit_at = ImGui::GetTime();
        g_app.search_pending = true;
        // 立即停掉在途检索：关键字已经变了，旧查询的结果既没意义、又白占工作线程
        // （它与渲染抢同一个线程，不停会明显拖慢出图）。这就是"改词即取消"。
        if (g_app.search_active) {
            g_app.renderer->cancel_search();
            g_app.search_active = false;
        }
        // 旧结果立即清掉：结果列表必须与关键字一致，否则列表里是"别的词"的命中，
        // 点进去跳到的地方与输入框里的词对不上（用户会当成"搜索不准"）。
        g_app.search_hits.clear();
        g_app.search_cur = -1;
        g_app.search_scanned = 0;
        g_app.search_truncated = false;
        g_app.search_committed.clear();
    }
    if (g_app.search_pending && ImGui::GetTime() - g_app.search_edit_at >= kSearchDebounceSec) {
        if (g_app.search_buf[0] == '\0') {
            // 清空关键字 = 收场。**不弹**"请输入要查找的关键字"：用户是在删除，
            // 不是在搜空词，弹提示只会让人以为操作错了。
            g_app.search_pending = false;
            g_app.search_edit_at = -1.0;
            g_app.search_total = 0;
        } else {
            search_start();   // 内部会清空并重新发起，同时把 committed 落定
        }
    }

    // ---- 2) 增量取用新命中（渲染层边搜边发布）----
    const std::size_t before = g_app.search_hits.size();
    g_app.renderer->take_search_hits(g_app.search_hits);
    if (g_app.search_hits.size() > before && g_app.search_cur < 0)
        search_goto(0);   // 首批结果到达：自动选中并滚到第一条

    const lr::SearchStatus st = g_app.renderer->search_status();
    g_app.search_active = st.active;
    g_app.search_scanned = st.scanned;
    g_app.search_total = st.total;
    g_app.search_truncated = st.truncated;

    if (g_app.search_scroll_pending) {
        g_app.search_scroll_pending = false;
        if (g_app.search_cur >= 0 && g_app.search_cur < static_cast<int>(g_app.search_hits.size()))
            request_jump_scroll(g_app.search_hits[static_cast<std::size_t>(g_app.search_cur)].page, 0.0f);
    }
}

}  // namespace lr::app
