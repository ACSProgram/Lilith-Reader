# Phase 1 — 应用外壳（✅ 完成 2026-10-05，✅ 人工验证通过 2026-10-05）

## 实施内容
- Win32 + D3D11 + ImGui RAII 初始化（延续 Phase 0 骨架）
- 命令行解析 `LilithReader.exe <路径>`（CommandLineToArgvW，相对路径转绝对）
- 拖放打开（DragAcceptFiles / WM_DROPFILES，多文件取第一个）
- 窗口状态持久化：位置/尺寸/最大化 → exe 同目录 LilithReader.ini；恢复前校验（至少 100×100 落在虚拟屏幕内、最小 400×300，无效回退默认）；负坐标经 GetPrivateProfileStringW 读取以支持多显示器
- 格式识别占位页：受支持格式（pdf/epub/mobi/fb2/cbz/xps）→"已识别，Phase 2 接入渲染核心"；不受支持 →"暂不支持的格式"；路径不存在 →"文件不存在"
- 新增 src/utils/utils.ixx（项目首个 C++20 模块）：路径/扩展名/UTF-8/ini 工具
- F3 调试浮层；Esc 有文档返回引导页、无文档退出；WM_DPICHANGED 跟随；最小窗口 480×320
- 构建验收：0 警告 0 错误，exe 655KB（LTCG），dumpbin 依赖仅系统 DLL

## 事故记录
1. **产物错位（已修复）**：build.bat 直接编译 vcxproj 时 `$(SolutionDir)` 被解析为 `src\`，新 exe 落到 `src\bin\Release\`，`bin\Release\` 下仍是 Phase 0 旧 exe，导致首次人工验证时拖放无效。修复：vcxproj 的 IntDir/OutDir 改为基于 `$(ProjectDir)`；清理 src\bin、src\build 及散落 .obj。
2. **vcpkg 工具下载失败（瞬时）**：首次构建时 vcpkg 下载 7zr.exe 网络错误（curl 56），重试即恢复，未处理。

## 人工验证结果（2026-10-05，全部通过）
- 1.1 拖放打开与格式识别占位页 ✅
- 1.2 命令行打开（绝对/相对/不存在路径）✅
- 1.3 Esc 关闭文档 / 退出 ✅
- 1.4 窗口状态持久化（含最大化、屏幕外坐标回退）✅
- 1.5 最小尺寸与 DPI ✅
- 1.6 F3 调试浮层 ✅
- 1.7 依赖纯净性（dumpbin 静态确认）✅
