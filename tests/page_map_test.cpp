// page_map_test.cpp — 页面点 ↔ 屏幕点坐标折算的自动化断言（Phase 8）
//
// 为什么这些断言值得写：文本选择、复制、超链接命中、搜索命中高亮**全部**建立在这组
// 折算之上，而它的错误是"看起来能用、实则错位"的那一类 —— 0° 下一切正常，用户一旋转
// 就整体偏一格，肉眼很难判断到底是渲染错位还是折算错位。折算本身完全由
// （页矩形 + 未旋转页尺寸 + 旋转角）决定，无状态、无外部依赖，因此可以钉死。
//
// 断言分四类：
//   1. 恒等与中心：0° 下逐点一致；中心点在任何旋转下都映到页矩形中心。
//   2. 四角对应：90°/180°/270° 下"未旋转页面的四个角"必须落到屏幕矩形的**确定**角上。
//      这是最容易写反的一处（顺时针还是逆时针），用四个角把它锁住。
//   3. 往返一致：正向再逆向必须回到原点（含页外点 —— 拖动选择依赖页外点也能换算）。
//   4. 退化输入：页尺寸/页矩形为 0 时必须返回 false，而不是产生 NaN 或除零。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
// 用法： page_map_test.exe

#include "page_map.h"

#include <cmath>
#include <cstdio>

using lr::app::PageGeom;
using lr::app::PageView;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name, const char* detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s   %s\n", name, detail);
    }
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// 四个合法旋转角。用显式数组而不是 `for (int r : {0,90,180,270})`：
// 后者要 <initializer_list>，本测试刻意只依赖 <cmath>/<cstdio>。
const int kRots[4] = { 0, 90, 180, 270 };

// 便捷：把未旋转 pt 映到屏幕，返回是否成功
bool fwd(const PageView& v, const PageGeom& g, int rot, float x, float y, float& sx, float& sy) {
    return lr::app::page_pt_to_screen(v, g, rot, x, y, sx, sy);
}
bool inv(const PageView& v, const PageGeom& g, int rot, float sx, float sy, float& x, float& y) {
    return lr::app::screen_to_page_pt(v, g, rot, sx, sy, x, y);
}

}  // namespace

int main() {
    // 一个"非正方、非零原点"的页矩形与页尺寸：任何把偏移/宽高弄混的实现都会在这里露馅。
    const PageView view{ 100.0f, 50.0f, 300.0f, 400.0f };   // 屏幕矩形（已含画布原点）
    const PageGeom geom{ 600.0f, 800.0f };                  // 未旋转页面尺寸（pt）

    std::printf("== page_map：坐标折算断言 ==\n");

    // ---- 1. 0° 恒等与中心 ----
    {
        float sx = 0, sy = 0;
        check(fwd(view, geom, 0, 0.0f, 0.0f, sx, sy) && near(sx, 100.0f) && near(sy, 50.0f),
              "rot 0：页左上 → 屏幕矩形左上");
        check(fwd(view, geom, 0, 600.0f, 800.0f, sx, sy) && near(sx, 400.0f) && near(sy, 450.0f),
              "rot 0：页右下 → 屏幕矩形右下");
        check(fwd(view, geom, 0, 300.0f, 400.0f, sx, sy) && near(sx, 250.0f) && near(sy, 250.0f),
              "rot 0：页中心 → 屏幕矩形中心");
        // 各旋转下中心都必须落在矩形中心（旋转不动点）
        for (int ri = 0; ri < 4; ++ri) {
            const int rot = kRots[ri];
            const bool ok = fwd(view, geom, rot, 300.0f, 400.0f, sx, sy) &&
                            near(sx, 250.0f) && near(sy, 250.0f);
            char name[64];
            std::snprintf(name, sizeof name, "rot %d：页中心不动", rot);
            check(ok, name);
        }
    }

    // ---- 2. 四角对应（这是最容易写反的一处）----
    //
    // 顺时针旋转 90° 时：原左上角转到**右上**，原左下角转到左上，依此类推。
    // 若把方向写反（逆时针），这四条会同时失败。
    {
        struct Corner { const char* name; float x, y, ex, ey; };
        // rot 90（顺时针）：(u,v) = (1-nv, nu)
        const Corner r90[] = {
            { "rot 90：页左上 → 屏右上", 0.0f, 0.0f, 400.0f, 50.0f },
            { "rot 90：页左下 → 屏左上", 0.0f, 800.0f, 100.0f, 50.0f },
            { "rot 90：页右下 → 屏左下", 600.0f, 800.0f, 100.0f, 450.0f },
            { "rot 90：页右上 → 屏右下", 600.0f, 0.0f, 400.0f, 450.0f },
        };
        for (const Corner& c : r90) {
            float sx = 0, sy = 0;
            check(fwd(view, geom, 90, c.x, c.y, sx, sy) && near(sx, c.ex) && near(sy, c.ey),
                  c.name);
        }
        // rot 180：对角互换
        const Corner r180[] = {
            { "rot 180：页左上 → 屏右下", 0.0f, 0.0f, 400.0f, 450.0f },
            { "rot 180：页右下 → 屏左上", 600.0f, 800.0f, 100.0f, 50.0f },
        };
        for (const Corner& c : r180) {
            float sx = 0, sy = 0;
            check(fwd(view, geom, 180, c.x, c.y, sx, sy) && near(sx, c.ex) && near(sy, c.ey),
                  c.name);
        }
        // rot 270（顺时针 270° = 逆时针 90°）：(u,v) = (nv, 1-nu)
        const Corner r270[] = {
            { "rot 270：页左上 → 屏左下", 0.0f, 0.0f, 100.0f, 450.0f },
            { "rot 270：页右上 → 屏左上", 600.0f, 0.0f, 100.0f, 50.0f },
            { "rot 270：页左下 → 屏右下", 0.0f, 800.0f, 400.0f, 450.0f },
            { "rot 270：页右下 → 屏右上", 600.0f, 800.0f, 400.0f, 50.0f },
        };
        for (const Corner& c : r270) {
            float sx = 0, sy = 0;
            check(fwd(view, geom, 270, c.x, c.y, sx, sy) && near(sx, c.ex) && near(sy, c.ey),
                  c.name);
        }
    }

    // ---- 3. 往返一致（含页外点）----
    {
        const float xs[7] = { -120.0f, 0.0f, 137.5f, 300.0f, 599.0f, 600.0f, 780.0f };
        const float ys[7] = { -90.0f, 0.0f, 12.25f, 400.0f, 799.0f, 800.0f, 950.0f };
        bool all_ok = true;
        char detail[128] = {};
        for (int ri = 0; ri < 4; ++ri) {
            const int rot = kRots[ri];
            for (int xi = 0; xi < 7; ++xi) {
                for (int yi = 0; yi < 7; ++yi) {
                    const float x = xs[xi], y = ys[yi];
                    float sx = 0, sy = 0, bx = 0, by = 0;
                    if (!fwd(view, geom, rot, x, y, sx, sy)) { all_ok = false; continue; }
                    if (!inv(view, geom, rot, sx, sy, bx, by)) { all_ok = false; continue; }
                    if (!near(bx, x) || !near(by, y)) {
                        all_ok = false;
                        std::snprintf(detail, sizeof detail,
                                      "rot %d  pt(%.2f, %.2f) → 屏幕(%.2f, %.2f) → pt(%.2f, %.2f)",
                                      rot, (double)x, (double)y, (double)sx, (double)sy,
                                      (double)bx, (double)by);
                    }
                }
            }
        }
        check(all_ok, "正向→逆向 往返一致（含页外点）", detail);
    }

    // ---- 4. 退化输入 ----
    {
        float sx = 0, sy = 0;
        const PageGeom zero_w{ 0.0f, 800.0f };
        const PageGeom zero_h{ 600.0f, 0.0f };
        const PageView zero_vw{ 100.0f, 50.0f, 0.0f, 400.0f };
        const PageView zero_vh{ 100.0f, 50.0f, 300.0f, 0.0f };
        check(!fwd(view, zero_w, 0, 10.0f, 10.0f, sx, sy), "页宽为 0 → 返回 false");
        check(!fwd(view, zero_h, 0, 10.0f, 10.0f, sx, sy), "页高为 0 → 返回 false");
        check(!fwd(zero_vw, geom, 0, 10.0f, 10.0f, sx, sy), "屏幕矩形宽为 0 → 返回 false");
        check(!fwd(zero_vh, geom, 0, 10.0f, 10.0f, sx, sy), "屏幕矩形高为 0 → 返回 false");
        float px = 0, py = 0;
        check(!inv(zero_vw, geom, 0, 10.0f, 10.0f, px, py), "逆向：屏幕矩形宽为 0 → false");
    }

    // ---- 5. 旋转角归一化（负数 / 超 360 / 非 90 倍数）----
    {
        float a = 0, b = 0, c = 0, d = 0;
        const bool ok1 = fwd(view, geom, 90, 0.0f, 0.0f, a, b) &&
                         fwd(view, geom, 450, 0.0f, 0.0f, c, d) && near(a, c) && near(b, d);
        check(ok1, "rot 450 ≡ rot 90（超 360 归一）");
        const bool ok2 = fwd(view, geom, 270, 0.0f, 0.0f, a, b) &&
                         fwd(view, geom, -90, 0.0f, 0.0f, c, d) && near(a, c) && near(b, d);
        check(ok2, "rot -90 ≡ rot 270（负角归一）");
        const bool ok3 = fwd(view, geom, 0, 0.0f, 0.0f, a, b) &&
                         fwd(view, geom, 37, 0.0f, 0.0f, c, d) && near(a, c) && near(b, d);
        check(ok3, "非 90 倍数（37°）按 0° 处理");
    }

    std::printf("合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
