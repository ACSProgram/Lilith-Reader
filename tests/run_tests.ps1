# run_tests.ps1 — 编译并运行 Lilith Reader 的自动化测试（Phase 2 起常备）
#
# 做什么：
#   1) 用 Python 生成样本集 tests/samples/（含加密 PDF、损坏集、改名集）
#   2) 用 cl.exe 直接编译项目自己的模块（utils.ixx / document.ixx / document.cpp）
#      与 tests/doc_test.cpp，链接成 tests/_build/doc_test.exe
#   3) 运行断言测试，用它的退出码作为本脚本的退出码
#   可选 -Probe：额外编译并运行 MuPDF 诊断探针（打印 MuPDF 自己对每个样本的判定）
#
# 为什么不走 MSBuild：测试要"绕过应用外壳、直接断言文档模块"，自己写编译命令行更直接，
# 也避免动主工程的 vcxproj。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Probe
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -VcpkgRoot D:\vcpkg-roots\LilithReader

[CmdletBinding()]
param(
    [string]$VcpkgRoot = "",
    [string]$Python = "",
    [switch]$Probe,
    [switch]$NoRegenerate
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tests = Join-Path $repo "tests"
$samples = Join-Path $tests "samples"
$out = Join-Path $tests "_build"
$vcxproj = Join-Path $repo "src\LilithReader.vcxproj"

function Step($s) { Write-Host "`n== $s ==" -ForegroundColor Cyan }
function Die($s) { Write-Host $s -ForegroundColor Red; exit 1 }

# ---- 1. 定位工具链 -------------------------------------------------------------

Step "定位工具链"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Die "找不到 vswhere.exe，请确认已安装 Visual Studio" }
$vsPath = & $vswhere -latest -property installationPath
if (-not $vsPath) { Die "vswhere 未找到 Visual Studio 安装" }

$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvc) { Die "找不到 MSVC 工具集目录：$msvcRoot" }
$cl = Join-Path $msvc.FullName "bin\Hostx64\x64\cl.exe"

$sdkInc = "C:\Program Files (x86)\Windows Kits\10\Include"
$sdkLib = "C:\Program Files (x86)\Windows Kits\10\Lib"
$sdkVerDir = Get-ChildItem $sdkInc -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVerDir) { Die "找不到 Windows SDK：$sdkInc" }
$sdkVer = $sdkVerDir.Name

Write-Host "  VS      : $vsPath"
Write-Host "  MSVC    : $($msvc.Name)"
Write-Host "  SDK     : $sdkVer"

# vcpkg 安装根：优先参数，其次从 vcxproj 里读（避免两处硬编码漂移）
if (-not $VcpkgRoot) {
    $line = Select-String -Path $vcxproj -Pattern "<VcpkgInstalledDir>(.+?)</VcpkgInstalledDir>" |
        Select-Object -First 1
    if (-not $line) { Die "vcxproj 里找不到 VcpkgInstalledDir，请用 -VcpkgRoot 指定" }
    $VcpkgRoot = $line.Matches[0].Groups[1].Value.Trim()
}
$tripletLine = Select-String -Path $vcxproj -Pattern "<VcpkgTriplet>(.+?)</VcpkgTriplet>" |
    Select-Object -First 1
$triplet = if ($tripletLine) { $tripletLine.Matches[0].Groups[1].Value.Trim() } else { "x64-windows-static" }

$vcpkgTarget = Join-Path $VcpkgRoot $triplet
$inc = Join-Path $vcpkgTarget "include"
$lib = Join-Path $vcpkgTarget "lib"
if (-not (Test-Path (Join-Path $inc "mupdf\fitz.h"))) {
    Die "找不到 MuPDF 头文件：$inc （用 -VcpkgRoot 指定 vcpkg 安装根）"
}
Write-Host "  vcpkg   : $vcpkgTarget"

if (-not $Python) {
    foreach ($cand in @("python", "$env:USERPROFILE\anaconda3\python.exe")) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd) { $Python = $cmd.Source; break }
        if (Test-Path $cand) { $Python = $cand; break }
    }
}
if (-not $Python) { Die "找不到 Python（用 -Python 指定）" }
Write-Host "  Python  : $Python"

# ---- 2. 生成样本 ---------------------------------------------------------------

if (-not $NoRegenerate) {
    Step "生成样本集"
    New-Item -ItemType Directory -Force -Path $samples | Out-Null
    & $Python (Join-Path $tests "make_samples.py") $samples
    if ($LASTEXITCODE -ne 0) { Die "样本生成失败" }
} else {
    Step "跳过样本生成（-NoRegenerate）"
    if (-not (Test-Path $samples)) { Die "样本目录不存在：$samples" }
}

# ---- 3. 编译 ------------------------------------------------------------------

Step "编译模块与测试"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$src = Join-Path $repo "src"
$defs = @("/nologo", "/std:c++20", "/EHsc", "/MT", "/O2", "/utf-8",
          "/D_MT", "/D_WINDOWS", "/D_CRT_SECURE_NO_WARNINGS")
$incs = @("/I$inc",
          "/I$(Join-Path $msvc.FullName 'include')",
          "/I$(Join-Path $sdkInc $sdkVer)\ucrt",
          "/I$(Join-Path $sdkInc $sdkVer)\um",
          "/I$(Join-Path $sdkInc $sdkVer)\shared")

function Invoke-Cl($argv, $what) {
    & $cl @argv
    if ($LASTEXITCODE -ne 0) { Die "$what 失败" }
}

# 注意：/ifcOutput 必须给**文件名**而不是目录。给目录时 MSVC 会按模块名命名
# （lilithreader.document.ifc），后面 /reference 就找不到文件，且**不报错**，
# 只会一路报"XXX 不是类或命名空间名称"，极难排查。
Invoke-Cl ($defs + $incs + @("/c", "/ifcOutput$out\utils.ifc", "/Fo$out\utils.ixx.obj",
    (Join-Path $src "utils\utils.ixx"))) "编译 utils.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/ifcOutput$out\document.ifc", "/Fo$out\document.ixx.obj",
    (Join-Path $src "document\document.ixx"))) "编译 document.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\document.ifc",
    "/Fo$out\document.obj", (Join-Path $src "document\document.cpp"))) "编译 document.cpp"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\utils.ifc",
    "/reference", "$out\document.ifc", "/Fo$out\doc_test.obj",
    (Join-Path $tests "doc_test.cpp"))) "编译 doc_test.cpp"

# Phase 3：画布是纯布局数学（不依赖 MuPDF），单独编译成 canvas_test.exe
Invoke-Cl ($defs + $incs + @("/c", "/ifcOutput$out\canvas.ifc", "/Fo$out\canvas.ixx.obj",
    (Join-Path $src "canvas\canvas.ixx"))) "编译 canvas.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\canvas.ifc",
    "/Fo$out\canvas.obj", (Join-Path $src "canvas\canvas.cpp"))) "编译 canvas.cpp"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\canvas.ifc",
    "/Fo$out\canvas_test.obj", (Join-Path $tests "canvas_test.cpp"))) "编译 canvas_test.cpp"

# Phase 4：页缓存纯策略（零依赖，不碰 D3D/线程/MuPDF），单独编译成 page_cache_test.exe
Invoke-Cl ($defs + $incs + @("/c", "/ifcOutput$out\page_cache.ifc", "/Fo$out\page_cache.ixx.obj",
    (Join-Path $src "render\page_cache.ixx"))) "编译 page_cache.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\page_cache.ifc",
    "/Fo$out\page_cache_test.obj", (Join-Path $tests "page_cache_test.cpp"))) "编译 page_cache_test.cpp"

# Phase 8：渲染层「文本通道 + 全文检索」端到端测试（跨线程，纯函数单测覆盖不到）。
# 设备传 nullptr：用例只走文本通道，不建纹理，故不需要真的 D3D 设备。
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\document.ifc",
    "/reference", "$out\page_cache.ifc", "/reference", "$out\utils.ifc",
    "/ifcOutput$out\render.ifc", "/Fo$out\render.ixx.obj",
    (Join-Path $src "render\render.ixx"))) "编译 render.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\render.ifc",
    "/reference", "$out\document.ifc", "/reference", "$out\page_cache.ifc",
    "/reference", "$out\utils.ifc",
    "/Fo$out\render.obj", (Join-Path $src "render\render.cpp"))) "编译 render.cpp"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\render.ifc",
    "/reference", "$out\document.ifc", "/reference", "$out\page_cache.ifc",
    "/reference", "$out\utils.ifc",
    "/Fo$out\render_search_test.obj",
    (Join-Path $tests "render_search_test.cpp"))) "编译 render_search_test.cpp"

# Phase 5：阅读状态持久化（纯序列化 + Win32 文件 I/O），单独编译成 reader_state_test.exe
Invoke-Cl ($defs + $incs + @("/c", "/ifcOutput$out\reader_state.ifc", "/Fo$out\reader_state.ixx.obj",
    (Join-Path $src "state\reader_state.ixx"))) "编译 reader_state.ixx"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\reader_state.ifc",
    "/Fo$out\reader_state.obj", (Join-Path $src "state\reader_state.cpp"))) "编译 reader_state.cpp"
Invoke-Cl ($defs + $incs + @("/c", "/reference", "$out\utils.ifc",
    "/reference", "$out\reader_state.ifc",
    "/Fo$out\reader_state_test.obj", (Join-Path $tests "reader_state_test.cpp"))) "编译 reader_state_test.cpp"

# 配色色调映射（ADR-068）：纯数学 + 无 ImGui/Win32 依赖的头，单独编译成 tone_test.exe。
# 它把"配色的骨架"（色相/饱和度/对比度）钉死，观感仍留给人工验证。
Invoke-Cl ($defs + @("/I$(Join-Path $src 'app')") + $incs + @("/c",
    "/Fo$out\tone_test.obj", (Join-Path $tests "tone_test.cpp"))) "编译 tone_test.cpp"

# 依赖库清单取自 unofficial-libmupdf 的 INTERFACE_LINK_LIBRARIES（不能改成"链上 lib\*.lib"：
# jpeg.lib 与 turbojpeg.lib 会符号冲突）
$libs = @("libmupdf.lib", "freetype.lib", "harfbuzz.lib", "jbig2dec.lib", "jpeg.lib",
          "openjp2.lib", "gumbo.lib", "zs.lib", "libpng16.lib", "brotlidec.lib",
          "brotlicommon.lib", "bz2.lib",
          "user32.lib", "gdi32.lib", "advapi32.lib", "ws2_32.lib", "crypt32.lib",
          "shell32.lib", "ole32.lib", "comdlg32.lib", "shlwapi.lib", "oleaut32.lib")
$libdirs = @("/LIBPATH:$lib", "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
             "/LIBPATH:$(Join-Path $sdkLib $sdkVer)\ucrt\x64",
             "/LIBPATH:$(Join-Path $sdkLib $sdkVer)\um\x64")
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\doc_test.exe", "$out\doc_test.obj", "$out\document.obj",
    "/link") + $libdirs + $libs) "链接 doc_test.exe"

# 画布测试只用 C++ 标准库，无需 MuPDF 依赖库。
# 必须一并链接 canvas.ixx.obj：接口单元里的内联成员（如 Canvas::state）由它发射。
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\canvas_test.exe", "$out\canvas_test.obj",
    "$out\canvas.obj", "$out\canvas.ixx.obj", "/link") + $libdirs) "链接 canvas_test.exe"

# 页缓存策略测试同样只用 C++ 标准库；必须链 page_cache.ixx.obj（导出函数由它发射）。
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\page_cache_test.exe", "$out\page_cache_test.obj",
    "$out\page_cache.ixx.obj", "/link") + $libdirs) "链接 page_cache_test.exe"

# 渲染层检索测试：链 render 接口单元 + 实现单元 + 它依赖的 document/page_cache/utils，
# 以及 d3d11（render.cpp 里建纹理用；本用例不实际建，但符号必须能解析）。
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\render_search_test.exe", "$out\render_search_test.obj",
    "$out\render.obj", "$out\render.ixx.obj", "$out\document.obj", "$out\document.ixx.obj",
    "$out\page_cache.ixx.obj", "$out\utils.ixx.obj",
    "/link") + $libdirs + $libs + @("d3d11.lib")) "链接 render_search_test.exe"

# 阅读状态测试：链 reader_state 的接口单元与实现单元。
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\reader_state_test.exe", "$out\reader_state_test.obj",
    "$out\reader_state.ixx.obj", "$out\reader_state.obj", "/link") + $libdirs) "链接 reader_state_test.exe"

# 色调测试只用 C++ 标准库。
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\tone_test.exe", "$out\tone_test.obj", "/link") +
    $libdirs) "链接 tone_test.exe"

# 坐标折算（Phase 8）：纯函数、零依赖（只用 <cmath>/<cstdio>），单独编译成 page_map_test.exe。
# 它与 tone_test 同一性质：app 层里被抽出来的"可判定"部分（旋转折算的角对应与往返一致）。
Invoke-Cl ($defs + @("/I$(Join-Path $src 'app')") + $incs + @("/c",
    "/Fo$out\page_map_test.obj", (Join-Path $tests "page_map_test.cpp"))) "编译 page_map_test.cpp"
Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\page_map_test.exe", "$out\page_map_test.obj", "/link") +
    $libdirs) "链接 page_map_test.exe"

# ---- 4. 运行测试 ---------------------------------------------------------------

Step "运行断言测试"
# doc_test.exe 按 UTF-8 输出（/utf-8 编译）；把控制台码页切到 65001 避免中文乱码
$prevCp = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\doc_test.exe" $samples
} finally {
    [Console]::OutputEncoding = $prevCp
}
$testExit = $LASTEXITCODE
Write-Host "  doc_test.exe 退出码 = $testExit"

Step "运行画布测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\canvas_test.exe"
} finally {
    [Console]::OutputEncoding = $prevCp
}
$canvasExit = $LASTEXITCODE
Write-Host "  canvas_test.exe 退出码 = $canvasExit"

Step "运行页缓存策略测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\page_cache_test.exe"
} finally {
    [Console]::OutputEncoding = $prevCp
}
$cacheExit = $LASTEXITCODE
Write-Host "  page_cache_test.exe 退出码 = $cacheExit"

Step "运行阅读状态测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\reader_state_test.exe"
} finally {
    [Console]::OutputEncoding = $prevCp
}
$stateExit = $LASTEXITCODE
Write-Host "  reader_state_test.exe 退出码 = $stateExit"

Step "运行配色色调测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\tone_test.exe"
} finally {
    [Console]::OutputEncoding = $prevCp
}
$toneExit = $LASTEXITCODE
Write-Host "  tone_test.exe 退出码 = $toneExit"

Step "运行坐标折算测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\page_map_test.exe"
} finally {
    [Console]::OutputEncoding = $prevCp
}
$pageMapExit = $LASTEXITCODE
Write-Host "  page_map_test.exe 退出码 = $pageMapExit"

Step "运行渲染层检索测试"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & "$out\render_search_test.exe" $samples
} finally {
    [Console]::OutputEncoding = $prevCp
}
$renderSearchExit = $LASTEXITCODE
Write-Host "  render_search_test.exe 退出码 = $renderSearchExit"

if ($Probe) {
    Step "编译并运行 MuPDF 诊断探针"
    Invoke-Cl ($defs + $incs + @("/c", "/Fo$out\mupdf_probe.obj",
        (Join-Path $tests "mupdf_probe.cpp"))) "编译 mupdf_probe.cpp"
    Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\mupdf_probe.exe", "$out\mupdf_probe.obj",
        "/link") + $libdirs + $libs) "链接 mupdf_probe.exe"
    & "$out\mupdf_probe.exe" $samples
}

# ---- 5. 资源类断言（不走 C++ 用例）--------------------------------------------

# 内嵌 UI 字体子集的覆盖范围（ADR-048）：断言对象是字体文件的 cmap，用 fontTools 读表最直接；
# 判据是"源码里出现的每个非 ASCII 字符都在子集里"—— 界面自述文字不依赖系统字体的硬保证。
Step "字体子集覆盖断言"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & $Python (Join-Path $repo "assets\check_font_coverage.py")
} finally {
    [Console]::OutputEncoding = $prevCp
}
$fontExit = $LASTEXITCODE
Write-Host "  check_font_coverage.py 退出码 = $fontExit"

# 图标资源断言：.ico 的帧集必须完整、每帧尺寸必须名实相符。
# 这是"图标发糊 + 一圈灰边"那类问题的防线 —— 曾经 .ico 里只剩 16×16 一帧，
# Explorer 把它放大到 48/256，整套文件图标全糊。
Step "图标资源断言"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & $Python (Join-Path $repo "assets\check_icons.py")
} finally {
    [Console]::OutputEncoding = $prevCp
}
$iconExit = $LASTEXITCODE
Write-Host "  check_icons.py 退出码 = $iconExit"

# 菜单文案宽度断言（ADR-067）：弹出菜单的宽度由最长项决定，一条超长文案会把整张菜单撑宽。
Step "菜单文案宽度断言"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & $Python (Join-Path $repo "tests\check_menu_width.py")
} finally {
    [Console]::OutputEncoding = $prevCp
}
$menuExit = $LASTEXITCODE
Write-Host "  check_menu_width.py 退出码 = $menuExit"

# 主题重置断言（ADR-068）：apply_theme_colors 的两个分支必须先整套重置 ImGui 样式，
# 否则未覆盖的颜色项会带着上一个主题的值活过来（深色勾选框曾因此变成亮奶油色）。
Step "主题重置断言"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & $Python (Join-Path $repo "tests\check_theme_reset.py")
} finally {
    [Console]::OutputEncoding = $prevCp
}
$themeExit = $LASTEXITCODE
Write-Host "  check_theme_reset.py 退出码 = $themeExit"

Step "结束"
if ($testExit -eq 0 -and $canvasExit -eq 0 -and $cacheExit -eq 0 -and $stateExit -eq 0 -and
    $toneExit -eq 0 -and $pageMapExit -eq 0 -and $renderSearchExit -eq 0 -and
    $fontExit -eq 0 -and $iconExit -eq 0 -and
    $menuExit -eq 0 -and $themeExit -eq 0) {
    Write-Host "全部通过。" -ForegroundColor Green
} else {
    Write-Host "存在失败用例。" -ForegroundColor Red
}
if ($testExit -ne 0) { exit $testExit }
if ($canvasExit -ne 0) { exit $canvasExit }
if ($cacheExit -ne 0) { exit $cacheExit }
if ($stateExit -ne 0) { exit $stateExit }
if ($toneExit -ne 0) { exit $toneExit }
if ($pageMapExit -ne 0) { exit $pageMapExit }
if ($renderSearchExit -ne 0) { exit $renderSearchExit }
if ($fontExit -ne 0) { exit $fontExit }
if ($iconExit -ne 0) { exit $iconExit }
if ($menuExit -ne 0) { exit $menuExit }
exit $themeExit
