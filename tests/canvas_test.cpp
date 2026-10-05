// canvas_test.cpp — Lilith Reader 自研画布自动化测试（Phase 3 起常备）
//
// 做法：**直接链接项目自己的 lilithreader.canvas 模块**，对纯布局数学逐一断言。
// 画布不依赖 ImGui/D3D，故这里是"可程序判定的证据"：缩放/滚动/多列布局的
// 每条公式都用独立算出的期望值钉死，而不是从实际输出回填。
//
// 覆盖：fit-width 派生、固定缩放居中、内容尺寸与滚动钳制、以鼠标为锚的缩放
//       （定点不变性）、命中测试、可见范围、列切换锚定、非均匀页尺寸、缩放钳制、
//       空文档、fit 复位。
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

// 常用装置：viewport 1000x800，页 600x800，margin 16，gap 12
constexpr float kVW = 1000.0f, kVH = 800.0f;
constexpr float kPW = 600.0f, kPH = 800.0f;

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
        lr::Canvas c = make(1, 1, true);  // z = 968/600
        const float z = (kVW - 32.0f) / kPW;
        check_near(c.content_height_px(), kPH * z + 32.0f, "1 列内容高 = 页高*z + 2*margin");
    }
    {
        lr::Canvas c = make(10, 2, true);  // 5 行
        const float z = (kVW - 32.0f - 12.0f) / (2 * kPW);
        check_near(c.content_height_px(), 5 * kPH * z + 4 * 12.0f + 32.0f,
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
        check_near(r1.y, 16.0f + kPH * z + 12.0f, "单列页 1 y = 页 0 底 + gap");
    }
    {
        lr::Canvas c = make(4, 2, true);
        const float z = c.effective_zoom();
        check_near(c.page_rect(0).x, 16.0f, "双列页 0 在左列");
        check_near(c.page_rect(1).x, 16.0f + kPW * z + 12.0f, "双列页 1 在右列");
        check_near(c.page_rect(1).y, 16.0f, "双列页 0/1 同一行");
        check_near(c.page_rect(2).x, 16.0f, "双列页 2 回到左列");
        check_near(c.page_rect(2).y, 16.0f + kPH * z + 12.0f, "双列页 2 在第二行");
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
    check_near(c.page_rect(2).y, c.origin_y() + 16.0f + 200.0f + 12.0f,
               "第二行 y = 首行高 + gap + 上留白（含居中偏移）");
    check_near(c.content_height_px(), 200.0f + 300.0f + 12.0f + 32.0f,
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

    std::printf("\n合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
