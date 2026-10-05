// canvas_test.cpp — Lilith Reader 自研画布自动化测试（Phase 3 起常备）
//
// 做法：**直接链接项目自己的 lilithreader.canvas 模块**，对纯布局数学逐一断言。
// 画布不依赖 ImGui/D3D，故这里是"可程序判定的证据"：缩放/滚动/多列布局的
// 每条公式都用独立算出的期望值钉死，而不是从实际输出回填。
//
// 覆盖：fit-width 派生、固定缩放居中、内容尺寸与滚动钳制、以鼠标为锚的缩放
//       （定点不变性）、命中测试、可见范围、列切换锚定、非均匀页尺寸、缩放钳制、
//       空文档、fit 复位、阅读游标翻页、页间距随缩放（ADR-029）。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
//
// 用法： canvas_test.exe

#include <cmath>
#include <cstdio>
#include <vector>

import lilithreader.canvas;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const char* name) {
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
    }
}

bool near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

void check_near(float got, float want, const char* name) {
    if (near(got, want)) {
        ++g_pass;
        std::printf("  [PASS] %-46s (%.4f)\n", name, (double)got);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %-46s 期望 %.4f，实际 %.4f\n", name, (double)want, (double)got);
    }
}

// 常用装置：viewport 1000x800，页 600x800，margin 16（屏幕像素），gap 0.013×列宽（ADR-029）
constexpr float kVW = 1000.0f, kVH = 800.0f;
constexpr float kPW = 600.0f, kPH = 800.0f;

// 页间距与 fit-width 的**独立期望值**（不引用画布内部量，只按 ADR-029 的定义重算）：
//   屏幕间距 = 比例 × 列宽 × zoom；fit-width：zoom = (VW − 2·margin) / (cols·列宽 + (cols−1)·间距pt)
constexpr float kGapRatio = 0.013f;
float gap_at(float zoom, float col_w = kPW) { return kGapRatio * col_w * zoom; }
float fit_zoom(int cols, float vw = kVW, float pw = kPW, float margin = 16.0f) {
    return (vw - 2.0f * margin) /
           (static_cast<float>(cols) * pw + static_cast<float>(cols - 1) * kGapRatio * pw);
}

lr::Canvas make(int pages, int cols, bool fit, float zoom = 1.0f,
                lr::PageSizePt size = { kPW, kPH }) {
    lr::Canvas c;
    c.set_viewport(kVW, kVH);
    c.set_default_size(size);
    c.set_uniform(pages, size);
    lr::CanvasState st;
    st.columns = cols;
    st.fit_width = fit;
    st.zoom = zoom;
    c.set_state(st);
    c.clamp_scroll();
    return c;
}

// ---- 1. fit-width 派生 ----
void test_fit_width() {
    std::printf("\n[1] fit-width 派生\n");
    {
        lr::Canvas c = make(3, 1, true);
        check_near(c.effective_zoom(), (kVW - 2 * 16.0f) / (1 * kPW), "1 列 fit-width zoom");
        check_near(c.content_width_px(), kVW, "1 列内容宽 = 视口宽");
    }
    {
        lr::Canvas c = make(3, 2, true);
        check_near(c.effective_zoom(), (kVW - 2 * 16.0f - 12.0f) / (2 * kPW),
                   "2 列 fit-width zoom");
        check_near(c.content_width_px(), kVW, "2 列内容宽 = 视口宽");
    }
    {
        lr::Canvas c = make(3, 4, true);
        check_near(c.effective_zoom(), (kVW - 2 * 16.0f - 3 * 12.0f) / (4 * kPW),
                   "4 列 fit-width zoom");
        check_near(c.content_width_px(), kVW, "4 列内容宽 = 视口宽");
    }
}

// ---- 2. 内容尺寸 ----
void test_content_size() {
    std::printf("\n[2] 内容尺寸\n");
    {
        lr::Canvas c = make(1, 1, true);
        const float z = fit_zoom(1);
        check_near(c.content_height_px(), kPH * z + 32.0f, "1 列内容高 = 页高*z + 2*margin");
    }
    {
        lr::Canvas c = make(10, 2, true);  // 5 行
        const float z = fit_zoom(2);
        check_near(c.content_height_px(), 5 * kPH * z + 4 * gap_at(z) + 32.0f,
                   "2 列 10 页内容高（5 行）");
        check(c.rows() == 5, "2 列 10 页 = 5 行");
    }
}

// ---- 3. 页面矩形（fit-width） ----
void test_page_rect_fit() {
    std::printf("\n[3] 页面矩形（fit-width）\n");
    {
        lr::Canvas c = make(3, 1, true);
        const float z = c.effective_zoom();
        const lr::PageRect r0 = c.page_rect(0);
        check_near(r0.x, 16.0f, "单列页 0 x = 左留白");
        check_near(r0.y, 16.0f, "单列页 0 y = 上留白");
        check_near(r0.w, kPW * z, "单列页 0 宽 = 页宽*z");
        check_near(r0.h, kPH * z, "单列页 0 高 = 页高*z");

        const lr::PageRect r1 = c.page_rect(1);
        check_near(r1.y, 16.0f + kPH * z + gap_at(z), "单列页 1 y = 页 0 底 + gap");
    }
    {
        lr::Canvas c = make(4, 2, true);
        const float z = c.effective_zoom();
        check_near(c.page_rect(0).x, 16.0f, "双列页 0 在左列");
        check_near(c.page_rect(1).x, 16.0f + kPW * z + gap_at(z), "双列页 1 在右列");
        check_near(c.page_rect(1).y, 16.0f, "双列页 0/1 同一行");
        check_near(c.page_rect(2).x, 16.0f, "双列页 2 回到左列");
        check_near(c.page_rect(2).y, 16.0f + kPH * z + gap_at(z), "双列页 2 在第二行");
    }
}

// ---- 4. 固定缩放：居中 ----
void test_fixed_zoom_centering() {
    std::printf("\n[4] 固定缩放居中\n");
    lr::Canvas c = make(1, 1, false, 0.5f);
    const float cw = kPW * 0.5f + 32.0f;  // 332
    const float ch = kPH * 0.5f + 32.0f;  // 432
    check_near(c.origin_x(), (kVW - cw) * 0.5f, "内容窄于视口时水平居中");
    check_near(c.origin_y(), (kVH - ch) * 0.5f, "内容矮于视口时垂直居中");
    check_near(c.page_rect(0).x, (kVW - cw) * 0.5f + 16.0f, "居中后页 x");
    check_near(c.page_rect(0).y, (kVH - ch) * 0.5f + 16.0f, "居中后页 y");
    check_near(c.max_scroll_x(), 0.0f, "内容不溢出时不可横向滚动");
    check_near(c.max_scroll_y(), 0.0f, "内容不溢出时不可纵向滚动");
}

// ---- 5. 滚动钳制 ----
void test_scroll_clamp() {
    std::printf("\n[5] 滚动钳制\n");
    lr::Canvas c = make(1, 1, false, 1.0f, { 600.0f, 2000.0f });
    const float expect_max_y = 2000.0f + 32.0f - kVH;  // 1232
    check_near(c.max_scroll_y(), expect_max_y, "max_scroll_y");
    check_near(c.max_scroll_x(), 0.0f, "max_scroll_x = 0（页窄于视口）");

    c.scroll_by(0.0f, 999999.0f);
    check_near(c.state().scroll_y, expect_max_y, "向下滚动被钳到上限");
    c.scroll_by(0.0f, -999999.0f);
    check_near(c.state().scroll_y, 0.0f, "向上滚动被钳到 0");
    c.scroll_by(-500.0f, 0.0f);
    check_near(c.state().scroll_x, 0.0f, "横向滚动被钳到 0");
}

// ---- 6. 以鼠标为锚的缩放：定点不变性 ----
void test_zoom_at_anchor() {
    std::printf("\n[6] 以鼠标为锚的缩放（定点不变性）\n");
    lr::Canvas c = make(3, 1, false, 1.0f, { 600.0f, 2000.0f });
    c.scroll_by(0.0f, 500.0f);

    const float ax = 400.0f, ay = 300.0f;
    const float doc_x = (ax - c.origin_x()) / c.effective_zoom();
    const float doc_y = (ay - c.origin_y()) / c.effective_zoom();

    c.set_zoom(2.0f, ax, ay);

    const float sx = c.origin_x() + doc_x * c.effective_zoom();
    const float sy = c.origin_y() + doc_y * c.effective_zoom();
    check_near(c.effective_zoom(), 2.0f, "缩放到 2.0 生效");
    check_near(sx, ax, "锚点 x 不变（缩放定点）");
    check_near(sy, ay, "锚点 y 不变（缩放定点）");
    check(c.state().fit_width == false, "缩放后切到固定缩放模式");
}

// ---- 7. 命中测试 ----
void test_hit_test() {
    std::printf("\n[7] 命中测试\n");
    lr::Canvas c = make(5, 1, false, 1.0f);
    check(c.page_at_content_y(0.0f) == 0, "y=0 → 页 0");
    check(c.page_at_content_y(100.0f) == 0, "y=100 → 页 0");
    check(c.page_at_content_y(830.0f) == 1, "y=830 → 页 1（跨过 gap）");
    check(c.page_at_content_y(1700.0f) == 2, "y=1700 → 页 2");
    check(c.page_at_content_y(999999.0f) == 4, "y 越界 → 末页");

    lr::Canvas c2 = make(5, 2, false, 1.0f);
    check(c2.page_at_content_y(830.0f) == 2, "双列 y=830 → 第二行首页 = 页 2");
}

// ---- 8. 可见范围 ----
void test_visible_range() {
    std::printf("\n[8] 可见范围\n");
    lr::Canvas c = make(10, 1, false, 1.0f);
    check(c.visible_first() == 0 && c.visible_last() == 0, "顶部：仅页 0 可见");

    c.scroll_by(0.0f, 820.0f);
    check(c.visible_first() == 0 && c.visible_last() == 1, "滚动 820：页 0..1 可见");

    c.scroll_to_page(5);
    check(c.visible_first() == 5, "跳页 5 后首可见页 = 5");
}

// ---- 9. 列切换锚定 ----
void test_columns_anchor() {
    std::printf("\n[9] 列切换锚定\n");
    lr::Canvas c = make(12, 1, true);
    c.scroll_to_page(5);
    const int anchor = c.visible_first();
    check(anchor == 5, "切列前首可见页 = 5");
    c.set_columns(2);
    check(c.state().columns == 2, "列数切到 2");
    check(c.visible_first() <= anchor && anchor <= c.visible_last(),
          "切列后锚点页仍在可见范围内");
    c.set_columns(9);
    check(c.state().columns == lr::kMaxColumns, "列数被钳到上限 4");
    c.set_columns(0);
    check(c.state().columns == lr::kMinColumns, "列数被钳到下限 1");
}

// ---- 10. 非均匀页尺寸：行高取该行最大 ----
void test_nonuniform() {
    std::printf("\n[10] 非均匀页尺寸\n");
    lr::Canvas c;
    c.set_viewport(kVW, kVH);
    c.set_page_sizes({ { 600.0f, 100.0f }, { 600.0f, 200.0f },
                       { 600.0f, 300.0f }, { 600.0f, 50.0f } });
    lr::CanvasState st;
    st.columns = 2;
    st.fit_width = false;
    st.zoom = 1.0f;
    c.set_state(st);
    c.clamp_scroll();

    check(c.rows() == 2, "4 页 2 列 = 2 行");
    check_near(c.row_height_px(0), 200.0f, "第 0 行高 = 行内最大页高");
    check_near(c.row_height_px(1), 300.0f, "第 1 行高 = 行内最大页高");
    check_near(c.page_rect(2).y, c.origin_y() + 16.0f + 200.0f + gap_at(1.0f),
               "第二行 y = 首行高 + gap + 上留白（含居中偏移）");
    check_near(c.content_height_px(), 200.0f + 300.0f + gap_at(1.0f) + 32.0f,
               "非均匀内容高 = 各行高 + gap + 2*margin");
}

// ---- 11. 缩放钳制与 fit 复位 ----
void test_zoom_clamp_and_fit() {
    std::printf("\n[11] 缩放钳制与 fit 复位\n");
    lr::Canvas c = make(3, 1, true);
    c.set_zoom(100.0f, 0.0f, 0.0f);
    check_near(c.effective_zoom(), lr::kMaxZoom, "缩放被钳到上限");
    c.set_zoom(0.001f, 0.0f, 0.0f);
    check_near(c.effective_zoom(), lr::kMinZoom, "缩放被钳到下限");
    c.fit_to_width();
    check(c.state().fit_width, "fit_to_width 恢复 fit 模式");
    check_near(c.content_width_px(), kVW, "fit 复位后内容宽 = 视口宽");
}

// ---- 12. 空文档 ----
void test_empty() {
    std::printf("\n[12] 空文档\n");
    lr::Canvas c = make(0, 1, true);
    check(c.page_count() == 0, "页数为 0");
    check(c.visible_last() < c.visible_first(), "可见范围为空");
    const lr::PageRect r = c.page_rect(0);
    check(near(r.w, 0.0f) && near(r.h, 0.0f), "越界页矩形为空");
    check_near(c.content_height_px(), 0.0f, "空文档内容高为 0");
    check(c.current_page() == -1, "空文档游标为 -1");
}

// ---- 13. 翻页游标：矮页（视口高于一行）不卡住 ----
//
// 这是第三轮调试修掉的核心回归：视口比"一行"还高时（横向页 / 缩小 / 多列），
// 末尾若干行的 row_top 会超过 max_scroll_y，**无法**对齐到视口顶部。旧实现以
// visible_first() 反推游标，于是滚到底后游标仍停在前一行，连续按 → 原地打转。
void test_row_navigation_tall_viewport() {
    std::printf("\n[13] 翻页游标（矮页：视口高于一行）\n");
    // 视口 1000x800；页 600x200 → 一行 200px，行间距 0.013*600=7.8px（zoom=1），
    // 视口容得下约 3.8 行。
    // 20 行内容高 = 20*200 + 19*7.8 + 32 = 4180.2 → max_scroll_y = 3380.2；
    // 末行(row19) 顶部 = 16 + 19*207.8 = 3964.2 > 3380.2 → 够不到视口顶部。
    lr::Canvas c = make(20, 1, false, 1.0f, { 600.0f, 200.0f });
    check(c.rows() == 20, "20 页单列 = 20 行");
    check(c.current_page() == 0, "初始游标在第 0 页");
    check_near(c.max_scroll_y(),
               20 * 200.0f + 19 * gap_at(1.0f) + 32.0f - kVH,
               "max_scroll_y = 内容高 − 视口高");

    int guard = 0;
    while (c.current_page() < 19 && guard < 100) { c.scroll_rows(+1); ++guard; }
    check(c.current_page() == 19, "连续下翻能抵达末页（不卡住）");
    check(guard == 19, "恰好 19 次下翻走到末页（不多不少）");
    check_near(c.state().scroll_y, c.max_scroll_y(), "抵达末页时视口已到底");

    const float y_end = c.state().scroll_y;
    c.scroll_rows(+1);
    check_near(c.state().scroll_y, y_end, "到底后再下翻无操作");
    check(c.current_page() == 19, "到底后游标仍停在末页");

    guard = 0;
    while (c.current_page() > 0 && guard < 100) { c.scroll_rows(-1); ++guard; }
    check(c.current_page() == 0, "连续上翻能回到首页（不卡住）");
    // 注意：scroll_to_page(0) 的语义是"把首页顶部对齐到视口顶部"，
    // 因此 scroll_y = 上留白 16，而不是 0（上留白要靠手动上滚才露出）。
    check_near(c.state().scroll_y, 16.0f, "回到首页时首页顶部对齐视口顶部");
    c.scroll_rows(-1);
    check_near(c.state().scroll_y, 16.0f, "到首页后再上翻无操作");
    c.scroll_by(0.0f, -999999.0f);
    check_near(c.state().scroll_y, 0.0f, "手动上滚可到内容最顶端（露出上留白）");
}

// ---- 14. 翻页游标：多列按行推进 ----
void test_row_navigation_columns() {
    std::printf("\n[14] 翻页游标（多列）\n");
    lr::Canvas c = make(8, 2, false, 1.0f, { 600.0f, 200.0f });
    check(c.rows() == 4, "2 列 8 页 = 4 行");
    check(c.current_page() == 0, "初始游标 = 页 0");

    c.scroll_rows(+1);
    check(c.current_page() == 2, "下翻一行 → 页 2（下一行首页）");
    c.scroll_rows(+1);
    check(c.current_page() == 4, "再下翻一行 → 页 4");
    c.scroll_rows(+1);
    check(c.current_page() == 6, "下翻到末行首页 = 页 6");
    c.scroll_rows(+1);
    check(c.current_page() == 6, "末行后再下翻无操作（游标钳在末行）");
    c.scroll_rows(-1);
    check(c.current_page() == 4, "上翻一行 → 页 4");
    c.scroll_rows(-1);
    c.scroll_rows(-1);
    check(c.current_page() == 0, "回到首行 → 页 0");
}

// ---- 15. 游标与手动滚动/跳页同步 ----
void test_nav_row_sync() {
    std::printf("\n[15] 游标与手动滚动同步\n");
    lr::Canvas c = make(20, 1, false, 1.0f, { 600.0f, 200.0f });

    c.scroll_by(0.0f, 999999.0f);  // 滚轮滚到底
    check_near(c.state().scroll_y, c.max_scroll_y(), "滚轮到底被钳到上限");
    check(c.current_page() == 19, "到底后游标 = 末页（而不是倒数第二页）");

    c.scroll_by(0.0f, -999999.0f);  // 滚到顶
    check(c.current_page() == 0, "到顶后游标 = 首页");

    c.scroll_to_page(7);
    check(c.current_page() == 7, "显式跳页后游标 = 目标页");

    // 固定缩放后行高改变，游标按新滚动位置回同步
    c.set_zoom(0.5f, 0.0f, 0.0f);
    check(c.current_page() >= 0 && c.current_page() < 20, "缩放后游标仍在合法范围");
}

// ---- 16. 页间距随缩放变化（ADR-029）----
//
// 背景：第四轮反馈"页面缩小时感觉上的间距显得太大"。根因是间距原本是**屏幕像素常量**
// （换算进文档坐标时除以 zoom），页面变小而间距不变。改为"列宽的比例"后，
// 屏幕间距 = 比例 × 列宽 × zoom，**严格正比于 zoom** —— 这里把这条性质钉死。
void test_gap_scales_with_zoom() {
    std::printf("\n[16] 页间距随缩放（ADR-029）\n");

    // 固定缩放：zoom 翻倍 → 间距翻倍；绝对值 = 比例 × 页宽 × zoom
    const float z1 = 1.0f, z2 = 2.0f;
    lr::Canvas a = make(2, 1, false, z1);
    lr::Canvas b = make(2, 1, false, z2);
    const lr::PageRect a0 = a.page_rect(0), a1 = a.page_rect(1);
    const lr::PageRect b0 = b.page_rect(0), b1 = b.page_rect(1);
    const float gap1 = a1.y - (a0.y + a0.h);
    const float gap2 = b1.y - (b0.y + b0.h);

    check(gap1 > 0.0f, "两页之间存在间距");
    check_near(gap1, lr::CanvasState{}.gap_ratio * kPW * z1, "间距 = 比例 × 页宽 × zoom（zoom=1）");
    check_near(gap2 / gap1, z2 / z1, "间距与 zoom 严格成正比（zoom 翻倍 → 间距翻倍）");

    // fit-width：视口宽度减半 → zoom 约减半 → 间距约减半（正是用户反馈的场景）
    auto fit_gap = [](float vw) {
        lr::Canvas c;
        c.set_viewport(vw, kVH);
        c.set_default_size({ kPW, kPH });
        c.set_uniform(2, { kPW, kPH });
        lr::CanvasState st;
        st.fit_width = true;
        c.set_state(st);
        c.clamp_scroll();
        const lr::PageRect p0 = c.page_rect(0), p1 = c.page_rect(1);
        return p1.y - (p0.y + p0.h);
    };
    const float gwide = fit_gap(1000.0f);
    const float gnarrow = fit_gap(500.0f);
    check(gnarrow < gwide * 0.55f && gnarrow > gwide * 0.42f,
          "fit-width 下视口减半 → 间距约减半（不再是屏幕常量）");

    // 间距随列数参与 fit-width 方程：内容宽度仍恰等于视口宽（不变量不被破坏）
    lr::Canvas c2 = make(4, 2, true);
    check_near(c2.content_width_px(), kVW, "含间距后 fit-width 仍严格铺满视口宽");
}

// ---- 17. 双页对开（书籍模式，Phase 5）----
//
// 书籍模式 = 封面（第 0 页）单独成页 + 其余两页对开：(1,2)、(3,4)…。
// 与"columns=2 均匀网格"的唯一区别是**奇偶偏移**；布局/翻页/缩放全部复用网格逻辑。
// 这里把映射与"封面在整行内居中"钉死，并守卫 spread=false 时映射不被污染。
void test_spread_book_mode() {
    std::printf("\n[17] 双页对开（书籍模式，Phase 5）\n");
    auto make_spread = [](int pages, bool spread) {
        lr::Canvas c;
        c.set_viewport(kVW, kVH);
        c.set_default_size({ kPW, kPH });
        c.set_uniform(pages, { kPW, kPH });
        lr::CanvasState st;
        st.columns = 1;         // 对开时 columns 被忽略（等效 2 列）
        st.fit_width = false;
        st.zoom = 1.0f;
        st.spread = spread;
        c.set_state(st);
        c.clamp_scroll();
        return c;
    };

    lr::Canvas c = make_spread(5, true);
    check(c.rows() == 3, "5 页对开 = 3 行（封面 1 行 + 对开 2 行）");
    check(c.row_of(0) == 0, "页 0 独占第 0 行");
    check(c.row_of(1) == 1 && c.row_of(2) == 1, "页 1/2 在第 1 行（对开）");
    check(c.row_of(3) == 2 && c.row_of(4) == 2, "页 3/4 在第 2 行");
    check(c.first_page_in_row(1) == 1, "第 1 行首页 = 页 1");
    check(c.row_page_end(0) == 0, "第 0 行末页 = 页 0（封面单独）");
    check(c.row_page_end(1) == 2, "第 1 行末页 = 页 2");
    check(c.row_page_end(2) == 4, "第 2 行末页 = 页 4");

    // 封面在整行宽度内居中；对开两页分列左右
    const float gap = gap_at(1.0f);
    const float ox = c.origin_x();
    check_near(c.page_rect(0).x, ox + 16.0f + (2 * kPW + gap - kPW) * 0.5f,
               "封面在整行内居中");
    check_near(c.page_rect(1).x, ox + 16.0f, "对开左页贴左列");
    check_near(c.page_rect(2).x, ox + 16.0f + kPW + gap, "对开右页在右列");
    check_near(c.page_rect(1).y, c.page_rect(2).y, "对开两页同一行");
    check(c.page_rect(0).y < c.page_rect(1).y, "封面行在对开行之上");

    // 翻页按"行"推进，行首页即该行首页
    check(c.current_page() == 0, "初始游标 = 页 0");
    c.scroll_rows(+1);
    check(c.current_page() == 1, "下翻 → 页 1（第 1 行首页）");
    c.scroll_rows(+1);
    check(c.current_page() == 3, "再下翻 → 页 3（第 2 行首页）");
    c.scroll_rows(+1);
    check(c.current_page() == 3, "末行后再下翻无操作");
    c.scroll_rows(-1);
    check(c.current_page() == 1, "上翻 → 页 1");
    c.scroll_rows(-1);
    check(c.current_page() == 0, "回到封面");

    // 回归守卫：spread=false 时映射不变（均匀网格，未被对开映射污染）
    lr::Canvas g = make_spread(5, false);
    check(g.rows() == 5, "非对开：单列 5 页 = 5 行");
    check(g.row_of(1) == 1, "非对开：页 1 在第 1 行");
    check(g.row_page_end(0) == 0, "非对开：第 0 行末页 = 页 0");
    check_near(g.page_rect(0).x, g.origin_x() + 16.0f,
               "非对开：无行内居中偏移（x = 留白 + 居中原点）");

    // 切换对开时锚定当前页（锚点页须仍在可见范围内）
    lr::Canvas a = make(9, 1, true);
    a.scroll_to_page(6);
    check(a.current_page() == 6, "切换前游标 = 页 6");
    a.set_spread(true);
    check(a.state().spread, "spread 已开启");
    check(a.visible_first() <= 6 && 6 <= a.visible_last(), "切换对开后锚点页仍可见");
    a.set_spread(false);
    check(!a.state().spread, "spread 已关闭");
}

}  // namespace

int main() {
    std::printf("Lilith Reader 画布测试（Phase 3）\n");
    test_fit_width();
    test_content_size();
    test_page_rect_fit();
    test_fixed_zoom_centering();
    test_scroll_clamp();
    test_zoom_at_anchor();
    test_hit_test();
    test_visible_range();
    test_columns_anchor();
    test_nonuniform();
    test_zoom_clamp_and_fit();
    test_empty();
    test_row_navigation_tall_viewport();
    test_row_navigation_columns();
    test_nav_row_sync();
    test_gap_scales_with_zoom();
    test_spread_book_mode();

    std::printf("\n合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
