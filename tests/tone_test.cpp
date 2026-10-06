// tone_test.cpp — 纸张方案色调映射的自动化断言（ADR-068）
//
// 配色是**观感**问题，但它的骨架是数学：色相、饱和度、WCAG 相对亮度。这里把骨架钉死，
// 只把"好不好看"留给人工验证（docs/04-人工验证.md）。断言分四类：
//
//   1. 原色零改动：原色方案必须逐通道等于基色 —— 它是用户认可的基准，任何"顺手调一下"
//      都是回归。
//   2. 黄金值：深色/暖色两套方案下，每个角色色的最终 RGB 逐项钉死。改了参数就会红，
//      逼你回来把这张表一起更新（这张表就是配色的定义）。
//   3. 对比度下限：正文/次要文字/强调色/层次分隔在各自底色上的 WCAG 对比度不得低于下限。
//      这是"看不清"的防线 —— 上一轮人工反馈的"暖色对比低"正是这类问题。
//   4. 结构不变量：①层次次序（谁比谁亮）在换色后不变；②强调族不再偏冷；
//      ③语义色（红/橙）仍是语义色。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
//
// 用法： tone_test.exe
//
// 注意：这张"基色表"必须与 src/app/platform.cpp 的 kPalLight/kPalDark 与
// apply_theme_colors 里的字面量保持一致；两者不一致时本测试即失效（当作提醒）。

#include "tone.h"

#include <cstdio>
#include <cstring>

using lr::app::Rgb;
using lr::app::Tone;

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

void to_hex(Rgb c, char* out, std::size_t n) {
    const int r = static_cast<int>(c.r * 255.0f + 0.5f);
    const int g = static_cast<int>(c.g * 255.0f + 0.5f);
    const int b = static_cast<int>(c.b * 255.0f + 0.5f);
    std::snprintf(out, n, "#%02X%02X%02X", r, g, b);
}

// 逐通道相等（容差 0.5/255：只吸收舍入，不放走任何真实改动）
bool same(Rgb a, Rgb b) {
    return std::fabs(a.r - b.r) < 0.0020f && std::fabs(a.g - b.g) < 0.0020f &&
           std::fabs(a.b - b.b) < 0.0020f;
}

struct Role {
    const char* name;
    Rgb base;
    Rgb want;   // 该方案下的期望值
};

// ---------------- 三套方案（与 platform.cpp 的常量同源） ----------------

const Tone kToneOriginal{ false, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, {} };
// 深色纸（页面是 #1F1D1B 暖黑）：中性族旋到 hue 29（与纸面同色相），但**饱和度压得很低** ——
// 深色主题的"暖"极易过量：文字面积小、对比强，一点色相在亮色上就非常显眼（sat 0.22 时
// 正文会被染成 #EFD7C1 那样的奶油色，整屏"变成暖深色"）。暖意只留在强调色上。
const Tone kToneDarkPage{ true, 29.0f, 0.08f, 1.00f, 1.00f, 0.25f, { 0.878f, 0.700f, 0.480f } };
// 暖色纸（页面是 #F6EEDC 米黄）：色相 40 与纸面一致；亮度整体压到 0.87，让米黄纸面成为
// 全屏最亮的一层（原来周围比纸还亮，页面反而显得发闷）；中间调压深 0.70 补偿色度对比。
const Tone kToneWarmPage{ true, 40.0f, 0.155f, 0.87f, 0.70f, 0.25f, { 0.663f, 0.435f, 0.145f } };

const Role kDarkRoles[] = {
    { "backdrop",    { 0.0902f, 0.0941f, 0.1098f }, { 0.0975f, 0.0940f, 0.0907f } },  // #191817
    { "chrome",      { 0.1255f, 0.1294f, 0.1490f }, { 0.1339f, 0.1294f, 0.1253f } },  // #222120
    { "panel",       { 0.1300f, 0.1300f, 0.1500f }, { 0.1353f, 0.1309f, 0.1267f } },  // #232120
    { "text",        { 0.8500f, 0.8600f, 0.8800f }, { 0.8863f, 0.8546f, 0.8249f } },  // #E2DAD2
    { "text_dim",    { 0.5000f, 0.5300f, 0.5700f }, { 0.5455f, 0.5235f, 0.5030f } },  // #8B8580
    { "border",      { 0.2400f, 0.2500f, 0.2800f }, { 0.2570f, 0.2490f, 0.2416f } },  // #42403E
    { "frame",       { 0.1900f, 0.2000f, 0.2300f }, { 0.2057f, 0.1993f, 0.1934f } },  // #343331
    { "button",      { 0.2100f, 0.2200f, 0.2600f }, { 0.2271f, 0.2201f, 0.2135f } },  // #3A3836
    { "accent",      { 0.4200f, 0.6500f, 0.9400f }, { 0.7701f, 0.6116f, 0.4156f } },  // #C49C6A
    { "chrome_text", { 0.8392f, 0.8510f, 0.8706f }, { 0.8774f, 0.8450f, 0.8147f } },  // #E0D7D0
    { "chrome_dim",  { 0.5569f, 0.5765f, 0.6039f }, { 0.5961f, 0.5706f, 0.5467f } },  // #98918B
    { "warn",        { 0.9500f, 0.7200f, 0.4200f }, { 0.9596f, 0.7157f, 0.4231f } },  // #F5B66C
    { "danger",      { 0.9500f, 0.4700f, 0.4500f }, { 0.9087f, 0.5004f, 0.4279f } },  // #E8806D
};

const Role kWarmRoles[] = {
    { "backdrop",    { 0.8941f, 0.9059f, 0.9216f }, { 0.8879f, 0.8470f, 0.7652f } },  // #E2D8C3
    { "chrome",      { 0.9569f, 0.9608f, 0.9686f }, { 0.9360f, 0.9004f, 0.8291f } },  // #EFE6D3
    { "panel",       { 0.9600f, 0.9650f, 0.9720f }, { 0.9393f, 0.9041f, 0.8337f } },  // #F0E7D5
    { "text",        { 0.1300f, 0.1500f, 0.1800f }, { 0.1423f, 0.1365f, 0.1249f } },  // #242320
    { "text_dim",    { 0.5500f, 0.5800f, 0.6200f }, { 0.4791f, 0.4571f, 0.4130f } },  // #7A7569
    { "border",      { 0.8000f, 0.8200f, 0.8500f }, { 0.8074f, 0.7652f, 0.6807f } },  // #CEC3AE
    { "frame",       { 0.9000f, 0.9100f, 0.9250f }, { 0.8918f, 0.8513f, 0.7702f } },  // #E3D9C4
    { "button",      { 0.8900f, 0.9000f, 0.9150f }, { 0.8831f, 0.8417f, 0.7590f } },  // #E1D7C2
    { "accent",      { 0.2300f, 0.4900f, 0.8500f }, { 0.6367f, 0.4112f, 0.1244f } },  // #A26920
    { "chrome_text", { 0.2196f, 0.2353f, 0.2588f }, { 0.1867f, 0.1794f, 0.1648f } },  // #302E2A
    { "chrome_dim",  { 0.4784f, 0.5020f, 0.5333f }, { 0.4121f, 0.3945f, 0.3593f } },  // #69655C
    { "warn",        { 0.7200f, 0.3500f, 0.1000f }, { 0.6511f, 0.3451f, 0.0748f } },  // #A65813
    { "danger",      { 0.7600f, 0.2200f, 0.1900f }, { 0.6668f, 0.2595f, 0.1537f } },  // #AA4227
};

constexpr int kRoleCount = 13;
int index_of(const char* name) {
    static const char* kNames[kRoleCount] = {
        "backdrop", "chrome", "panel", "text", "text_dim", "border", "frame",
        "button", "accent", "chrome_text", "chrome_dim", "warn", "danger",
    };
    for (int i = 0; i < kRoleCount; ++i)
        if (std::strcmp(kNames[i], name) == 0) return i;
    return -1;
}

struct Scheme {
    const char* name;
    const Tone* tone;
    const Role* roles;
};

const Scheme kSchemes[] = {
    { "深色纸张", &kToneDarkPage, kDarkRoles },
    { "暖色", &kToneWarmPage, kWarmRoles },
};
constexpr int kSchemeCount = 2;
constexpr int kPairCount = 10;

// ---------------- 1. 原色零改动 ----------------

void test_original_identity() {
    std::printf("\n-- 原色：逐通道零改动 --\n");
    for (int i = 0; i < kRoleCount; ++i)
        check(same(lr::app::tone_apply(kDarkRoles[i].base, kToneOriginal), kDarkRoles[i].base),
              "原色不改动任何颜色（深色系角色）", kDarkRoles[i].name);
    for (int i = 0; i < kRoleCount; ++i)
        check(same(lr::app::tone_apply(kWarmRoles[i].base, kToneOriginal), kWarmRoles[i].base),
              "原色不改动任何颜色（浅色系角色）", kWarmRoles[i].name);
    const Rgb odd{ 0.02f, 0.97f, 0.41f };
    check(same(lr::app::tone_apply(odd, kToneOriginal), odd), "原色不改动任意色");
}

// ---------------- 2. 黄金值 ----------------

void test_golden() {
    std::printf("\n-- 黄金值：各方案的角色最终色 --\n");
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        for (int i = 0; i < kRoleCount; ++i) {
            const Role& r = s.roles[i];
            const Rgb got = lr::app::tone_apply(r.base, *s.tone);
            char hg[8], hw[8], name[128];
            to_hex(got, hg, sizeof hg);
            to_hex(r.want, hw, sizeof hw);
            std::snprintf(name, sizeof name, "%s · %s = %s（期望 %s）", s.name, r.name, hg, hw);
            check(same(got, r.want), name);
        }
    }
}

// ---------------- 3. 对比度下限 ----------------

struct Pair {
    const char* fg;
    const char* bg;
    float floor;
};

// 下限取"三套方案当前值再留一点余量"，低于它就该有人回来看看是不是配色又坏了。
const Pair kPairs[] = {
    { "text",        "panel",       7.00f },   // 正文
    { "text_dim",    "panel",       2.70f },   // 次要说明文字
    { "frame",       "panel",       1.10f },   // 控件框与面板的分界
    { "border",      "panel",       1.32f },   // 边框
    { "button",      "panel",       1.10f },   // 按钮
    { "accent",      "panel",       3.00f },   // 强调色（勾选/滑块/下划线）
    { "accent",      "frame",       2.50f },   // 强调色压在自己的控件底上
    { "chrome_text", "chrome",      7.00f },   // 顶栏/状态栏主文字
    { "chrome_dim",  "chrome",      3.00f },   // 顶栏/状态栏次要文字
    { "panel",       "backdrop",    1.05f },   // 面板浮在画布上
};

void test_contrast() {
    std::printf("\n-- 对比度下限（WCAG） --\n");
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        for (int pi = 0; pi < kPairCount; ++pi) {
            const Pair& p = kPairs[pi];
            Rgb fg{}, bg{};
            for (int i = 0; i < kRoleCount; ++i) {
                if (std::strcmp(s.roles[i].name, p.fg) == 0) fg = s.roles[i].want;
                if (std::strcmp(s.roles[i].name, p.bg) == 0) bg = s.roles[i].want;
            }
            const float c = lr::app::tone_contrast(fg, bg);
            char name[128];
            std::snprintf(name, sizeof name, "%s · %s/%s = %.2f（下限 %.2f）", s.name, p.fg,
                          p.bg, static_cast<double>(c), static_cast<double>(p.floor));
            check(c >= p.floor, name);
        }
    }
    // 原色方案同样过一遍（基色即终色）
    const Role* neutral = kWarmRoles;   // 浅色系基色
    for (int pi = 0; pi < kPairCount; ++pi) {
        const Pair& p = kPairs[pi];
        Rgb fg{}, bg{};
        for (int i = 0; i < kRoleCount; ++i) {
            if (std::strcmp(neutral[i].name, p.fg) == 0) fg = neutral[i].base;
            if (std::strcmp(neutral[i].name, p.bg) == 0) bg = neutral[i].base;
        }
        const float c = lr::app::tone_contrast(fg, bg);
        char name[128];
        std::snprintf(name, sizeof name, "原色 · %s/%s = %.2f（下限 %.2f）", p.fg, p.bg,
                      static_cast<double>(c), static_cast<double>(p.floor));
        check(c >= p.floor, name);
    }
}

// ---------------- 4. 结构不变量 ----------------

// 4a. 层次次序：换色不该把"谁比谁亮"搞乱 —— 否则就是"某些地方看不清"的来源。
//     断言的是**次序关系**而不是绝对次序：深色底与浅色底的次序本来就相反，写死一套
//     换个方案就废。这里要求每一对中性角色的亮度大小关系，在 tone 前后符号一致。
void test_layer_order() {
    std::printf("\n-- 结构：换色不改变明暗次序 --\n");
    static const char* kNeutral[] = { "backdrop", "chrome", "panel", "text", "text_dim",
                                      "border", "frame", "button" };
    constexpr int kN = 8;
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        int bad = 0;
        char first[160] = {};
        for (int i = 0; i < kN; ++i) {
            for (int j = i + 1; j < kN; ++j) {
                const int a = index_of(kNeutral[i]), b = index_of(kNeutral[j]);
                const float d0 = lr::app::tone_luminance(s.roles[a].base) -
                                 lr::app::tone_luminance(s.roles[b].base);
                if (std::fabs(d0) < 1e-4f) continue;   // 基色本来就同亮度：次序无意义
                const float d1 = lr::app::tone_luminance(s.roles[a].want) -
                                 lr::app::tone_luminance(s.roles[b].want);
                if ((d0 > 0.0f) != (d1 > 0.0f)) {
                    if (bad == 0)
                        std::snprintf(first, sizeof first, "%s 与 %s 的明暗关系被翻转",
                                      kNeutral[i], kNeutral[j]);
                    ++bad;
                }
            }
        }
        char name[160];
        if (bad == 0) std::snprintf(name, sizeof name, "%s 中性族明暗次序与原色一致", s.name);
        else          std::snprintf(name, sizeof name, "%s 中性族明暗次序（%d 对翻转）", s.name, bad);
        check(bad == 0, name, first);
    }
}

// 4b. 强调族不再偏冷：色相 180~305 的颜色过完 tone 必须离开冷区。
void test_accent_not_cold() {
    std::printf("\n-- 结构：强调色不再偏冷 --\n");
    const float colds[] = { 190.0f, 210.0f, 240.0f, 270.0f, 300.0f };
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        for (int ci = 0; ci < 5; ++ci) {
            const float h = colds[ci];
            const Rgb c = lr::app::hsv_to_rgb(h, 0.75f, 0.85f);
            const Rgb got = lr::app::tone_apply(c, *s.tone);
            char name[128];
            std::snprintf(name, sizeof name, "%s 冷色 %.0f° 被换成暖强调色", s.name,
                          static_cast<double>(h));
            check(got.r > got.b, name);
        }
    }
}

// 4c. 语义色仍是语义色：红/橙只做小幅色相融入，不得被换成强调色。
void test_semantic_kept() {
    std::printf("\n-- 结构：语义色保持可辨识 --\n");
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        const Rgb red = lr::app::tone_apply({ 0.76f, 0.22f, 0.19f }, *s.tone);
        const Rgb orange = lr::app::tone_apply({ 0.72f, 0.35f, 0.10f }, *s.tone);
        check(red.r > red.g && red.g >= red.b, "红仍是红（R>G≥B）");
        check(orange.r > orange.g && orange.g > orange.b, "橙仍是橙（R>G>B）");
        // 错误色与警告色不能被压成同一个颜色
        check(std::fabs(red.r - orange.r) + std::fabs(red.g - orange.g) > 0.10f,
              "错误色与警告色仍可区分");
        const Rgb pair2[2] = { red, orange };
        for (int i = 0; i < 2; ++i)
            check(pair2[i].r > pair2[i].b, "语义色不带冷色调（R>B）");
    }
}

// 4d. 亮度保持：中性族（非中间调）过完 tone 后，相对亮度恰为原值 × level
//     —— 这是"层次关系不变"的数学保证，也是上面 test_layer_order 的前提。
void test_luminance_preserved() {
    std::printf("\n-- 结构：中性族保亮度（× level） --\n");
    const Rgb neutrals[] = {
        { 0.9600f, 0.9650f, 0.9720f }, { 0.9569f, 0.9608f, 0.9686f },
        { 0.8941f, 0.9059f, 0.9216f }, { 0.8000f, 0.8200f, 0.8500f },
    };
    for (int si = 0; si < kSchemeCount; ++si) {
        const Scheme& s = kSchemes[si];
        for (int i = 0; i < 4; ++i) {
            const Rgb c = neutrals[i];
            const float want = lr::app::tone_luminance(c) * s.tone->level;
            const float got = lr::app::tone_luminance(lr::app::tone_apply(c, *s.tone));
            const bool ok = std::fabs(got - want) <= want * 0.02f + 0.0005f;
            char name[160];
            std::snprintf(name, sizeof name, "%s 亮度 %.4f → %.4f（期望 %.4f）", s.name,
                          static_cast<double>(lr::app::tone_luminance(c)),
                          static_cast<double>(got), static_cast<double>(want));
            check(ok, name);
        }
    }
}

}  // namespace

int main() {
    std::printf("Lilith Reader 纸张方案色调测试（ADR-068）\n");
    test_original_identity();
    test_golden();
    test_contrast();
    test_layer_order();
    test_accent_not_cold();
    test_semantic_kept();
    test_luminance_preserved();

    std::printf("\n合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
