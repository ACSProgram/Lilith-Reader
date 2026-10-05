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

if ($Probe) {
    Step "编译并运行 MuPDF 诊断探针"
    Invoke-Cl ($defs + $incs + @("/c", "/Fo$out\mupdf_probe.obj",
        (Join-Path $tests "mupdf_probe.cpp"))) "编译 mupdf_probe.cpp"
    Invoke-Cl (@("/nologo", "/MT", "/Fe:$out\mupdf_probe.exe", "$out\mupdf_probe.obj",
        "/link") + $libdirs + $libs) "链接 mupdf_probe.exe"
    & "$out\mupdf_probe.exe" $samples
}

Step "结束"
if ($testExit -eq 0) {
    Write-Host "全部通过。" -ForegroundColor Green
} else {
    Write-Host "存在失败用例。" -ForegroundColor Red
}
exit $testExit
