# 贡献指南

感谢你有意为本项目出力。本页只说"怎么参与"；**仓库的工程纪律与文档规约以
[AGENTS.md](AGENTS.md) 为准，动代码前请先读它**——那里固化了已经踩过的坑。

## 许可

本项目以 **AGPL-3.0** 授权（全文见 [LICENSE](LICENSE)，第三方组件见 [NOTICE](NOTICE)）。
**提交贡献即表示你同意以 AGPL-3.0 授权你的贡献。** 若你的贡献包含第三方代码或资源，
请一并说明其来源与许可，且许可须与 AGPL-3.0 兼容。

## 开发环境

- Windows 10 1809+，Visual Studio 18 (2026) Community，PlatformToolset v145
- vcpkg（manifest 模式，triplet `x64-windows-static`）
- Python 3.13（仅用于样本生成与资源断言）

vcpkg 依赖**不放在仓库内**（仓库路径含空格会触发上游端口链接失败，见 ADR-010），
安装根由 `Directory.Build.props` 中的 `LilithVcpkgRoot` 决定，可经环境变量
`LilithVcpkgRoot` 或本机 `Directory.Build.props.local` 覆盖。首次克隆后直接：

```
build.bat                       :: 编译 Release x64 → bin\Release\LilithReader.exe
tests\run_tests.ps1             :: 全量自动化回归（自带 cl.exe 编译，不依赖主工程产物）
installer\build_installer.bat   :: 打安装包（需 Inno Setup 6，先跑过 build.bat）
```

## 提交前自检

1. **编译零错、`tests\run_tests.ps1` 全绿**。
2. **文档同步**：改了实现同步 `docs/02-架构设计.md` 与相关 `src/*/*.ixx` 头注释；
   改了构建/依赖同步 `docs/02` §8。判断标准见 [AGENTS.md](AGENTS.md) §2。
3. **影响扩展性的决定先出 ADR**（接口形态、线程归属、持久化格式、缓存预算），
   追加到 `docs/03-决策记录.md` 末尾，再动代码。
4. 不在开发过程中实际运行本软件做测试（编译与自动化测试除外）；需人工确认的项写入
   `docs/04-人工验证.md`。

## 提交信息格式

统一采用**中文「范围：描述」**：一个能点明改动部位或主题的范围前缀，加一句祈使句描述。

```
渲染：拆出后台渲染服务（ADR-085）
文档：修正扩展名归一与 fz_* 边界检查
构建：vcpkg 安装根改为可覆盖配置
```

- 范围用项目里的模块名或主题词（如 `渲染`/`文档`/`画布`/`界面`/`构建`/`文档体系`）。
- 涉及决策时在描述末尾括注 ADR 编号。
- 一个提交只做一件事；同一批次的多步改动在收尾时按逻辑合并，不留零碎提交。

## 提交方式

- 从 `main` 开分支开发，完成后提 Pull Request。
- 每批改动收尾即提交（编译零错、测试通过、文档同步完成后），不与后续修改混在工作区。
- CI（`.github/workflows/pr.yml`）会在 Windows runner 上跑 Debug/Release 构建、全量回归
  与安装包体积门禁，请确保其通过。

## 报告问题

- 功能缺陷、使用问题：开 Issue，附版本、环境、复现步骤与（如有）崩溃转储。
- 安全漏洞：**不要开公开 Issue**，按 [SECURITY.md](SECURITY.md) 私下报告。
