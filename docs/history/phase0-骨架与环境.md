# Phase 0 — 骨架与环境（✅ 完成 2026-10-05）

## 第一步：第三方库环境准备
- 全新下载最新版 ImGui（1.93.0 WIP @ ed73ef4），未复用 Lilith 的 1.91.9 旧版
- 全新下载最新版 ImPlot（1.1 WIP @ 09e2ba7）作为交互参考基准与调试可视化备用
- 采用 DX11 后端（imgui_impl_dx11 + imgui_impl_win32），替换原项目 DX12 后端
- vcxproj：/MT 静态 CRT、vcpkg manifest、triplet x64-windows-static
- 跑通基本程序：Win32 + D3D11 + ImGui 清屏 + 框架状态窗口
- 验收通过：Release 编译，dumpbin 确认仅依赖系统组件 DLL，窗口可拖动缩放、Per-Monitor DPI aware

## 第二步：项目骨架收尾
- git 仓库 + README + docs 框架；vcpkg.json manifest（mupdf 留待 Phase 2）
- 目录骨架 src/{app,document,canvas,render,utils}；ImGui/ImPlot 源码入 third_party/ 随仓库提交
- 未尽项：MuPDF 经 vcpkg 就位 → 归入 Phase 2 开工时执行
