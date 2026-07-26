# M0：可恢复非破坏编辑内核

状态：Implemented（本地验证；远端 CI 待执行）

版本基线：`0.1.0`

关联需求：`CORE-002`（部分）、`CORE-003`（部分）、`CORE-006`（曝光子集）、
`SEC-001`（本地拒绝网络子集）、`SEC-002`（源字节不改写子集）、
`SEC-004`（指定故障点子集）

关联 ADR：ADR-0001、ADR-0003、ADR-0004

## 1. 里程碑定位

M0 是完整产品路线图之前的工程基线：用最小可审计闭环验证“严格命令 → 非破坏状态
变更 → 持久化 → 崩溃后恢复 → 确定性重新渲染”。它不是可交付的照片编辑应用，也
不等同于 [实施路线图](13-delivery-roadmap.md) 中范围更大的 P0 纵向技术原型。

本里程碑的核心问题是：在没有 UI、GPU 和 AI 的前提下，能否验证源像素不被编辑算子
改写、受支持的 CLI 永久修改经过统一命令边界、已提交修订可校验并可按幂等键恢复。

## 2. 已实现范围

### 2.1 严格命令边界

- 实现 `nps.command/v1` 的 M0 子集：
  `adjust.exposure`、`history.undo`、`history.redo`；
- JSON 对象各层拒绝未知字段，标识、类型、数值范围和 JSON-safe 修订号均校验；
- 所有命令必须显式声明 `network: deny` 和 `cloudInference: deny`；
- 规范化幂等载荷排除传输级 `commandId`，同键同载荷返回首次结果，同键异载荷拒绝；
- `expectedRevision` 实现乐观并发冲突检查；
- 命令总线验证目标 `documentId`，并把协议命令映射到存储事务。

机器可读契约见
[命令 JSON Schema](../specs/commands/nps.command.v1.schema.json) 和
[M0 命令说明](../specs/commands/README.md)。

### 2.2 最小图像路径

- 内存图像为 RGB、16 位无符号通道；
- 严格编码和解码 P6 PPM，16 位采样采用大端字节序；
- 代码生成确定性梯度，不需要私人或外部测试照片；
- 曝光在归一化线性样本上按 `2^EV` 计算并饱和夹紧，不发生整数回绕；
- 同一项目快照重复渲染和重新打开后的输出可做逐字节比较。

这条路径只用于验证内核不变量，不代表已经实现 RAW、JPEG、ICC 或专业显示管线。

### 2.3 非破坏状态与项目存储

- 创建 `.npsproj` 目录项目，不覆盖已存在目标；
- 把源 PPM 完整复制到 SHA-256 内容寻址对象仓库，编辑仅改变快照参数；
- SQLite STRICT 表保存项目元数据、对象、快照、线性历史、事务和幂等结果；
- 每个成功曝光、撤销或重做命令都产生单调递增修订；
- 撤销/重做移动快照游标；撤销后新编辑截断前向历史，不改写源对象；
- 同一项目通过 `project.lock` 只允许一个本机存储会话；
- 创建在同父目录暂存，完成数据库后再发布最终目录；
- 显式关闭记录干净关闭状态，并尽力截断 WAL。

持久化格式和恢复边界见
[`nps.project/v1` 规范](../specs/project-format/nps.project.v1.md)。

### 2.4 崩溃恢复与完整性

- SQLite 使用 WAL、外键、`synchronous = FULL` 和不可信 Schema 禁用；
- 打开项目时校验格式标识、SQLite 完整性、外键、修订、历史和事务一致性；
- 重新读取每个已登记对象，验证普通文件、字节数和 SHA-256；
- 拒绝项目关键路径中检测到的符号链接或 Windows reparse point；
- 统计而不自动删除孤儿对象；
- 未干净关闭的项目在接受新命令前完成完整性检查，并在报告中标明恢复状态；
- 命令提交后强制退出的测试验证已提交事务仍存在，使用原幂等键重试得到首次结果；
- 创建过程中在源对象落盘后强制退出的测试验证最终目标尚未发布，随后可对原目标
  重新创建。

这些测试模拟指定进程终止边界，不是任意硬件故障、断电、远程文件系统或恶意本地
攻击者下的数据安全证明。

### 2.5 CLI、构建与公开仓库基线

`nps-cli` 提供：

```text
nps-cli demo <project.npsproj>
nps-cli verify <project.npsproj>
nps-cli crash-create-after-object <project.npsproj>
nps-cli crash-commit <project.npsproj>
nps-cli verify-recovery <project.npsproj>
```

仓库基线包括：

- C++23、CMake Presets、固定 vcpkg baseline；
- Catch2 单元/集成测试与 CTest 进程级恢复测试；
- Windows、Linux、macOS 的 GitHub Actions 构建测试矩阵，Windows 至少构建
  一个 Release 配置；
- CMake CLI 暂存安装，包含声明文件、Windows vcpkg 运行时 DLL，并对已安装
  `demo`/`verify` 做合成像素 smoke；
- 最小权限工作流、按完整提交 SHA 固定的 Actions；
- UTF-8、文档链接、公开文件安全和扫描器自测；
- Apache-2.0 项目许可与贡献、安全、隐私文档。

远端 CI 只有在公共仓库完成首次推送后才会产生结果；工作流文件存在不等于远端已经
通过。

## 3. 明确非目标

M0 未实现、也不应从当前代码推断已经实现：

- 桌面 UI、快捷/专业工作区、自然语言输入和交互预览；
- 人像、风景、宠物精修，磨皮、五官调整、换天和对象移除；
- 图层、组、蒙版、选区、画笔、编辑 DAG 和多文档；
- RAW/JPEG/PNG/TIFF/PSD 导入导出、EXIF、ICC、HDR 和软打样；
- GPU 分块渲染、24–100MP 性能目标、CPU/GPU 差分；
- AI 模型、Codec/AI Worker、插件、云 Provider 和联网；
- 外链源、自动保存预览分支、项目加密、对象回收和修复工具；
- 单文件 `.nps` 分享包、项目迁移器和跨版本兼容承诺；
- 正式 Windows/macOS 安装器、代码签名、公证、更新器、遥测、崩溃上报和
  成品发布。M0 的 `cmake --install` 只产生供开发与 CI 验证的 CLI 暂存树。

完整产品能力仍按 01–16 号 Proposed 设计文档和后续 ADR 评审推进。

## 4. 验收证据

### 4.1 自动化覆盖映射

| 不变量 | 自动化证据 |
|---|---|
| 命令严格解析、未知字段、数值边界、隐私拒绝、稳定错误码 | `tests/command_protocol_test.cpp` |
| 命令总线到存储的曝光、文档隔离、冲突、幂等、撤销/重做映射 | `tests/command_bus_test.cpp` |
| 16 位 PPM 往返、大端确定性、曝光倍增/夹紧、畸形输入拒绝 | `tests/imaging_test.cpp` |
| 源字节不变、SHA-256、唯一文档 ID、修订、幂等、撤销/重做/分支 | `tests/project_store_test.cpp` |
| 已存在目标不覆盖、干净重开、排他租约、孤儿计数、损坏与链接拒绝 | `tests/project_store_test.cpp` |
| 创建发布前退出可重试、提交后退出可恢复且幂等结果保留 | `tests/recovery_test.cmake` |
| UTF-8、Markdown 内链、私人媒体/密钥/路径/未固定 Action 检测 | `scripts/check-*.mjs` |
| 扫描器正负样例，防止门禁自身静默失效 | `scripts/check-public-safety.test.mjs` |

### 4.2 验收命令

本地与 CI 使用的标准入口为：

```sh
npm ci --ignore-scripts
npm run check:policy
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Windows 本地若因非 ASCII 工作区与 vcpkg 工具链组合受限，可以把依赖和构建目录放在
ASCII 临时路径；这不改变源码或项目格式。

### 4.3 截至本地验证

当前本地验证已确认：

- MSVC Debug 构建在高警告级别并把警告视为错误时通过；
- Catch2 命令、图像和项目存储测试为 30/30 用例、251 个断言通过；
- CTest 为 31/31 通过，包含进程级 `nps.recovery.committed-transaction`；
- 独立 Windows Release 构建、完整 CLI 暂存安装以及收窄 `PATH` 后的
  `demo`/`verify` smoke 通过；
- `npm ci` 未报告已知依赖漏洞；
- UTF-8、文档链接、公开文件安全检查通过，扫描器自测为 21/21 通过。

以上数字记录于 2026-07-25 的目标提交前本地验收，后续会随测试增强而变化，因此仍
以对应提交的 CTest 日志为最终证据。当前 Windows 测试进程没有创建目录符号链接的
权限，相关用例记录警告后跳过实际链接创建；Linux CI 的真实链接分支必须在首次推送
后通过。
远端 GitHub Actions 尚未执行时，不将本地结果表述为跨平台 CI 通过。

## 5. 已知限制与剩余风险

### 5.1 创建残留含私人像素

进程在项目发布前终止时，目标 `.npsproj` 不出现，但同一父目录可能留下
`.nps-creating-*`。它包含完整源 PPM，M0 没有自动发现或安全清理 API。备份、同步、
诊断和人工维护必须把它视为私人项目数据。清理工具在验证精确路径和标记前不得执行
批量递归删除。

### 5.2 未加密、非安全沙箱

项目对象和 SQLite/WAL 未静态加密。SHA-256 只检测一致性，不提供保密或真实性。
链接/reparse 防护与 no-follow 标志减少路径替换风险，但对象读取仍有检查后再打开的
竞争窗口；M0 不能作为对抗同权限恶意本地进程的隔离边界。

### 5.3 崩溃保证范围有限

已验证的是指定进程退出故障点及 SQLite WAL 恢复。尚未覆盖真实断电、磁盘满、I/O
部分写、文件系统损坏、网络盘、目录元数据未持久化和大规模随机故障注入。关闭时
WAL checkpoint 失败被视为可恢复优化失败，不会被报告为项目事实回滚。

### 5.4 格式与功能仍会演进

当前数据库无迁移器，`.npsproj` v1 只保存一个源对象和曝光参数。`previews/`、
`recovery/`、`manifests/` 只是预留目录。不得把此格式当作 1.0 冻结格式，或手工写入
未来字段并期望 M0 忽略。

`ProjectStore::execute(StoreCommand)` 当前仍是可直接调用的 C++ API；它是命令总线的
内部存储接口，不携带协议层的 `documentId` 或隐私字段。M0 CLI 会先走严格命令总线，
但内核尚未用进程或语言级能力边界禁止其他集成直接调用存储接口。后续应用必须把
用户和插件输入限制在命令总线之外，并收紧这一 API 边界。

### 5.5 公开发布证据尚待远端产生

本地公开安全门禁能发现已知敏感扩展名、路径、密钥模式与未固定 Actions，但不证明
仓库绝无隐私材料或供应链风险。首次公开前仍需人工审查 Git 状态、提交作者信息、
二进制和历史；推送后需要等待三平台 CI 与 CodeQL 的实际结果。

## 6. M0 完成定义

M0 的工程实现可在满足以下条件的具体提交上标记完成：

1. 高警告级别构建无警告；
2. Catch2 与完整 CTest 全绿；
3. 公开文件和文档门禁全绿；
4. 项目格式规范与实现一致；
5. 提交只包含项目目录内的公开源码和生成式测试素材；
6. 远端 CI 状态与本地结果分开记录，不以工作流配置代替执行证据。

这一定义只关闭 M0，不放宽后续 P0、Alpha、Beta 或 1.0 的图像质量、安全、隐私和
可靠性门槛。
