# Unleashed Dumper V2

本目录是 2026-11-07 新建的独立 V2 工作区。初次开档后，用户明确要求开始 C++；当前已有独立 C++20 / Win32 / DirectX 11 / Dear ImGui 首版。入口为 `build/Release/UnleashedDumperV2.exe`，构建 `build-release.ps1`，启动 `start-v2.ps1`。

- 先读 `../../docs/unleashed-dumper/V1_AUDIT_20261107.zh-CN.md` 和 `../../docs/unleashed-dumper/V2_REFACTOR_PLAN.zh-CN.md`。
- 当前使用与限制见 `../../docs/unleashed-dumper/V2_USAGE.zh-CN.md`。已经用真实 CN PE Dump 和既有捕获核对静态提取；新的 DMA 入口尚未连接设备实测，不能把旧 V1 现场记录算成 V2 验收。
- 产品方向为 Windows 本地 C++ / Dear ImGui，沿用 UN 的紧凑工具界面和配色。主流程是连接或打开样本、扫描、查看分组结果、复制帖子或导出 UN 更新参考。
- 文档统一放在 `01_PRODUCTS/docs/unleashed-dumper/`；本目录不放产品说明、审计报告、研究资料或验证记录。静态证据归 `03_DOWNP`，运行验证归 `04_SCENARIO/unleashed-dumper-v2`。
- `../Unleashed-Dumper` 是保留的 V1。本轮没有移动、删除或修改它，也没有停止旧实验目录中的服务。V2 稳定并完成替代验收后，才安排 V1 退役。
- V2 自有源码、Git、构建、配置和产物。不得依赖 V1 或 `un-dma-v2` 的运行目录；可核对后复制必要的第三方源码和已证实的读取方法，并保留来源。
- 不继承网页服务、合成演示主流程、实验版本发布/回滚、数据库事件哈希链或逐层声明确认。保留完整读取、缺页、歧义、目标身份和设备互斥等确有作用的检查。
- 结果首先供人阅读。JSON 只是辅助证据；帖子文本、工程参考和界面必须消费同一份结果。字段缺失或未验证应逐项说明，不能伪造值，也不能让一个无关字段阻断所有已有结果。
- 2026-11-16 更新：`data/rules.json` 已分别接入 CN / World / Nexon 有原始机器码依据的提取，包含三服 context map 与原生轮廓设置读取链；完整范围见 `../../docs/unleashed-dumper/V2_AUDIT_20261116.zh-CN.md`。三服覆盖不对等，骨数/完整组件类型/动态语义等缺口不可硬填。原方法基线只比较，不能回填；辅助函数从本次调用目标提取并验证指令窗口，不可取全局首个命中。
- CN、World、Nexon 是三条独立线路。按需获取三条版本候选，由用户三选一，保留各线路的手填版本；查询不自动选择 CN。CN 对应 `cn`，World 对应 `us/eu/kr`，Nexon 只对应 `nexon`。所选线路贯穿定位、归档和恢复，不能用首个配置或 CN 兜底。尚未接入的方法逐项说明，不伪造成功。分析完成自动按 `地区-版本号/时间` 本地保存。
- 2026-11-07 用户已要求 UI 重设计：UN 配色，结合 UN / NLC 的侧栏与紧凑分组组件。去掉 Windows 标题栏，使用独立关闭按钮和轻微透明背景。窗口保留拖动、缩放、最小化及键盘操作；文字与控件不随整窗降低透明度。
- 品牌使用 `assets/unleashed-logo.png`，源自 UN V2 同名资源，构建时复制到 V2 运行目录；不得运行时依赖 UN 目录。标题用 Bahnschrift，`Dumper V2` 独立下一行。顶部提供中文 / English 切换，以程序旁 `settings.json` 记忆选择。界面和新导出的 UN 参考随语言切换，技术分组、字段、解码函数和帖子文本保持英文。
- 已核实的导出名称与 UN V2 当前拥有它们的 `include/Game/Offsets.hpp`、`Cn155284.hpp`、`Decrypt.hpp` 对齐。命名映射不用于复制 UN 的值；无独立名称的字面量偏移保留描述性名称，不把成员偏移当成组件类型。既有结果保留保存时的名称。
- `start-v2.ps1` 应显示原生窗口并报告进程号；启动失败输出 `logs/launch-*.stderr.txt` 路径。GUI 初始化异常同时显示错误对话框，命令行入口仍使用标准错误。
- 原生检查在 `04_SCENARIO/unleashed-dumper-v2`，真实样本验证和界面证据在其 `implementation-20261107`。`prepare_rules.py` 仅为本轮生成规则和记录证据的开发工具，产品构建和运行均不依赖它或 Python。
- 2026-11-16 修复与验证归 `04_SCENARIO/unleashed-dumper-v2/audit-20261116`；规则生成入口为其 `regional/generate_rules.py`，原生轮廓片段由 `native_setting_rules.py` 从各服原件生成。构建和运行依然不依赖 Python。`package-v2.ps1` 生成完整便携包，`verify-package.ps1` 仅做摘要、DLL加载及帮助入口检查，不连接设备。已知 World PE 的可执行范围缺3584字节，保留 candidate，禁止补零升级为全覆盖。
- 论坛 516727 第 394 页用于确定排版和内容结构，不是本机 CN 值或解码公式的默认来源。当前值必须来自对应地区、构建的实际证据。
