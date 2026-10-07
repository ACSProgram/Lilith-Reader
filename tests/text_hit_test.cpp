// text_hit_test.cpp — 文本命中测试的「选行」判据（多列版面修正）
//
// 为什么值得写：这是"单列一切正常、多列右列几乎选不中"的那一类 bug。命中测试的选行
// 必须两维（纵向距行带 → 横向距行框），旧实现"取第一个 y 命中就收"，而 MuPDF 的 stext
// 块按阅读顺序排（左列整列在前），于是右列任意一点都会先命中左列那一行 —— 实测两列论文
// 右列几乎无法选择。这里用构造数据把判据钉死，防止再退回单维。
//
// 断言分四类：
//   1. 单列：每个 y 至多命中一行，行为与旧实现一致（回归守卫）。
//   2. 多列：同一 y 区间左右两行重叠，必须按 x 破平取到**正确那一列**（核心用例）。
//   3. 带外：纵向超出容差 → in_band = 0（严格模式据此如实报告"此处无文字"）。
//   4. 文字高度：竖排（宽 < 高）取宽度当行高，避免荒唐容差。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
// 用法： text_hit_test.exe

#include "text_hit.h"

#include <cstdio>
#include <vector>

using lr::app::pick_text_line;
using lr::app::hit_line_text_height;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name) {
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
    }
}

// 与 lr::TextLine 布局一致的最小结构：pick_text_line 是模板，无需依赖 document 模块。
struct L {
    float x0, y0, x1, y1;
};

}  // namespace

int main() {
    std::printf("== text_hit：选行判据断言 ==\n");

    // ---- 1. 单列：三行上下排布（行距 < 半字高，相邻容差带有重叠） ----
    {
        const std::vector<L> lines = {
            { 50.0f, 100.0f, 250.0f, 112.0f },
            { 50.0f, 116.0f, 250.0f, 128.0f },
            { 50.0f, 132.0f, 250.0f, 144.0f },
        };
        int band = 0;
        check(pick_text_line(lines, 150.0f, 106.0f, &band) == 0 && band == 1, "单列：第一行内 → 行 0");
        check(pick_text_line(lines, 150.0f, 122.0f, &band) == 1 && band == 1, "单列：第二行内 → 行 1");
        check(pick_text_line(lines, 150.0f, 138.0f, &band) == 2 && band == 1, "单列：第三行内 → 行 2");
        // 行间空白落在相邻两行容差带的重叠处 → 带内，取更靠前的一行
        check(pick_text_line(lines, 150.0f, 114.0f, &band) == 0 && band == 1, "单列：行间重叠带 → 带内最近行");
    }

    // ---- 2. 多列：左右两列同一 y 区间（核心回归） ----
    // 左列整列在前（与 MuPDF 阅读顺序一致），右列在后；两行 y 带完全重叠。
    {
        const std::vector<L> lines = {
            { 50.0f, 100.0f, 250.0f, 112.0f },   // 0 左列
            { 300.0f, 100.0f, 550.0f, 112.0f },  // 1 右列
        };
        int band = 0;
        check(pick_text_line(lines, 150.0f, 106.0f, &band) == 0, "多列：悬停左列 → 命中左列行");
        check(pick_text_line(lines, 400.0f, 106.0f, &band) == 1, "多列：悬停右列 → 命中右列行（旧实现误判为左列）");
        check(pick_text_line(lines, 300.0f, 106.0f, &band) == 1, "多列：右列左边界 → 命中右列行");
        check(pick_text_line(lines, 250.0f, 106.0f, &band) == 0, "多列：左列右边界 → 命中左列行");
    }

    // ---- 3. 带外：纵向超出容差 → in_band = 0 ----
    {
        const std::vector<L> lines = {
            { 50.0f, 100.0f, 250.0f, 112.0f },
        };
        int band = -1;
        check(pick_text_line(lines, 150.0f, 200.0f, &band) == 0 && band == 0, "带外：远端仍返回最近行，但 in_band=0");
        check(pick_text_line(lines, 150.0f, 104.0f, &band) == 0 && band == 1, "带内：正中 → in_band=1");
        // 空行表：无行可命中
        const std::vector<L> empty;
        check(pick_text_line(empty, 0.0f, 0.0f, &band) == -1 && band == 0, "空行表 → 返回 -1");
    }

    // ---- 4. 拖拽（clamp）语义：横向远离行框仍返回该行 ----
    // 拖到行尾之外要吸附到该行最近字符，故 pick_text_line 不因横向过远而返回 -1。
    {
        const std::vector<L> lines = {
            { 50.0f, 100.0f, 250.0f, 112.0f },
        };
        int band = 0;
        check(pick_text_line(lines, 600.0f, 106.0f, &band) == 0, "clamp：横向远超出行框仍返回该行");
    }

    // ---- 5. 文字高度：竖排取宽度 ----
    {
        const L vertical{ 100.0f, 100.0f, 108.0f, 500.0f };   // 宽 8 < 高 400
        check(hit_line_text_height(vertical) == 8.0f, "竖排行：行高取宽度（8）而非长度");
        const L degenerate{ 100.0f, 100.0f, 100.0f, 100.0f };
        check(hit_line_text_height(degenerate) == 1.0f, "退化行（零尺寸）：行高下限为 1");
    }

    std::printf("合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
