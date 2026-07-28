# M1：不可变编辑 DAG 与 CPU 参考渲染器

状态：Implemented（本地完整门禁与目标提交的三平台 CI、Sanitizer、CodeQL 均通过）

计划版本基线：`0.2.0`

关联需求：`CORE-002`、`CORE-003`、`CORE-006`（曝光/曲线子集）、
`PRO-001`（CPU 参考渲染子集）、`SEC-002`、`SEC-004`（迁移/导出故障子集）

关联 ADR：ADR-0001、ADR-0004、ADR-0005

## 1. 里程碑定位

M1 在 [M0 可恢复内核](17-milestone-m0.md) 上建立第一条可扩展图像编辑路径：
“不可变编辑 DAG → FP32 RGBA CPU 参考渲染 → ROI/瓦片 → 可取消结果发布 →
项目格式迁移 → 原子参考导出”。

M1 仍是无桌面 UI、无 GPU、无 AI、无真实照片 codec 的工程里程碑。它不能被描述为
已经具备人像、风景、宠物、换天、移除或 Photoshop 等级的成品功能。

## 2. 状态边界

本文冻结 M1 的范围和验收方式，不记录尚未运行的测试结果。工作包只有在目标提交的
源码、测试、迁移语料和 CI 共同提供证据后才能改为 `Implemented`。

| 工作包 | M1 目标 | 当前文档状态 |
|---|---|---|
| 像素/颜色 | FP32、RGBA、预乘 Alpha、稳定颜色 ID | Locally implemented；ADR-0005 已接受 |
| 编辑模型 | 不可变 Edit DAG、快照与确定性节点版本 | Locally implemented |
| 参考算子 | 曝光、曲线、Mask16 合成 | Locally implemented |
| 调度 | ROI、固定 512×512 tile、取消与过期结果抑制 | Locally implemented |
| 项目格式 | 显式 `v1 → v2` 迁移和颜色 ID 持久化 | Locally implemented |
| 导出 | 16 位 P6 PPM 原子参考导出 | Locally implemented |
| 自动化 | 单元、属性、差分、迁移、故障和三平台 CI | 本地通过；远端待验证 |

`Locally implemented` 只表示本节记录的 Windows 本地门禁通过，不代替 Linux、
macOS、sanitizer、CodeQL 或分支保护的远端证据。

## 3. M1 范围

### 3.1 显式像素与颜色

M1 CPU 参考缓冲区遵守
[ADR-0005](adr/ADR-0005-explicit-color-pixel-contract.md)：

- 每像素四个 FP32 分量，顺序为 RGBA；
- 场景线性 RGB、覆盖率 Alpha、预乘 Alpha；
- 默认工作空间 ID 为
  `nps.color/scene-linear-rec2020-d65/v1`；
- `nps.color/scene-linear-srgb-d65/v1` 仅用于合成测试；
- ACEScg ID 为未来保留，M1 不实现；
- M0 PPM 只是合成测试兼容入口。

所有缓冲区、节点、缓存键和 v2 项目事实必须存储/校验稳定字符串 ID，不能依赖枚举
序号或默认 profile。

### 3.2 不可变 Edit DAG

M1 把 M0 的累计曝光快照扩展为最小不可变编辑 DAG：

- 源节点、曝光节点、曲线节点和蒙版引用具有稳定 ID 与 Schema 版本；
- 已发布节点及参数不可原地改写；编辑产生新节点和新快照；
- 输入引用必须存在，提交前检测并拒绝环；
- 同一规范化 DAG、源对象、颜色 ID、ROI 和质量参数产生同一 CPU 参考结果；
- 撤销/重做选择不可变快照；从旧快照继续编辑形成新分支语义，不改写旧节点；
- 渲染缓存是可删除派生数据，不属于项目真相；
- 节点只读取显式输入，不能读取 UI、当前时间、随机全局状态或平台显示设置。

M1 不要求完整图层树、组、混合模式、画笔或多文档 UI。

### 3.3 曝光、曲线与 Mask16

参考算子范围为：

- 曝光：以有限 EV 参数对场景线性 RGB 乘 `2^EV`，Alpha 不变，中间值不隐式裁剪；
- 曲线：使用版本化、规范化控制点，拒绝非有限、乱序、重复横坐标或越界结构；具体
  外推/裁剪行为必须写入节点 Schema 并由金图固定；
- `Mask16`：无符号 16 位单通道覆盖率，`0` 表示不应用、`65535` 表示完全应用；
  CPU 参考归一化为 `m / 65535`，Mask 不携带颜色空间；
- 蒙版合成遵守预乘 Alpha，并覆盖空蒙版、全蒙版、梯度和透明边缘。

M1 的曲线和蒙版是渲染内核能力，不代表专业曲线 UI、选区工具或画笔已经存在。

### 3.4 ROI 与 512 tile

CPU 参考渲染支持矩形 ROI，并把请求编译为固定 `512 × 512` tile：

- 图像右/下边缘使用实际剩余尺寸，不填充为可观察像素；
- ROI 与画布求交；空交集返回明确空结果，不越界读取；
- 曝光和当前点曲线的 halo 为零；未来邻域算子必须显式声明 halo；
- tile 缓存键包含快照/DAG 哈希、节点版本、颜色 ID、ROI/tile 坐标和质量档；
- 同一快照全图渲染与拼接所有 tile 的结果必须在规定的 FP32 比较策略下等价；
- 只重新计算受修改 ROI 影响的 tile，优化不能改变 DAG 语义；
- 调度网格使用常量大小描述符按索引惰性生成 tile，不为每个 tile 预先分配对象；
- 单请求最多 `65,536` 个 tile，身份计算、冷缓存和热缓存入口使用同一前置校验，
  超限在分配输出或排队任务前以稳定状态拒绝。

512 是 M1 的生产参考尺寸；较小的可配置尺寸只用于确定性诊断和测试，也受同一
`65,536` 上限约束。该约束限制调度元数据放大，不替代对像素输出本身的尺寸和内存
预算。未来 GPU 后端可以选择其他内部调度尺寸，但必须保持相同可观察语义。

### 3.5 取消与过期结果

每个渲染请求绑定取消令牌、目标文档 ID、目标修订和不可变快照 ID：

- 取消令牌由项目内基于共享原子状态的轻量 C++20 原语提供，不依赖特定标准库对
  `std::stop_token` 的实现进度，从而保持 Windows、Linux 与 macOS/Apple Silicon
  的同一公开 API 和可观察语义；
- 取消后不再排队新 tile；
- 已运行的 tile 可以安全结束，但不得把取消请求的结果发布为当前预览或项目事实；
- 当前修订/快照改变后，旧请求标为过期；晚到结果不得覆盖新结果；
- 取消和过期不创建事务、不移动历史、不写入项目真相；
- 渲染失败、取消或输出未完整验证时不向缓存写入；成功后才从完整输出逐 tile、
  有界地接纳缓存项，不保留与像素数等比例的待提交副本；
- 缓存中的纯 tile 按完整不可变键隔离，不能以过期结果命中其他快照；
- 取消、过期和算子失败使用可区分的稳定状态，调用方不靠错误文本判断。

### 3.6 `nps.project/v1 → v2` 迁移

M1 必须新增独立的 v2 格式规范和显式迁移器。迁移至少满足：

- 迁移前精确校验 v1 Schema、`meta` 键集合和完整项目，未知触发器、视图、表、索引、
  弱化 DDL 或损坏格式都在任何转换前拒绝；
- 不修改内容寻址源对象，不把迁移写回原始照片；
- 把 M0 无 Alpha 合成 PPM 解释为 `A = 1`，并写入
  `nps.color/scene-linear-rec2020-d65/v1`；
- 把 v1 快照/历史的曝光事实确定性映射为 v2 不可变节点/快照，保持当前可见结果和
  撤销/重做语义；
- 迁移步骤可重入，失败不把半迁移项目发布为有效 v2；
- SQLite backup 后重新验证精确 v1 Schema 与来源摘要，v2 提交前后都把保留事实
  投影回 v1 摘要，防止来源或副本在验证与转换边界变化；
- 迁移前保留可验证回退副本或使用同父目录新目标原子发布；精确策略必须在 v2 规范
  中冻结；
- v2 重开后完整性检查覆盖 DAG 无环、引用、颜色 ID、对象哈希和历史游标；
- 未知未来主版本拒绝写入，不进行“尽力猜测”。

迁移测试只使用代码生成的公开合成项目，不使用真实用户照片。

### 3.7 原子 PPM 参考导出

M1 导出用于确定性回归，不是产品照片导出：

- 输入锁定一个不可变快照和明确颜色 ID；
- 只输出严格 16 位 P6 PPM，通道以大端样本编码；
- 不透明像素可直接编码；非不透明像素必须由调用方提供目标颜色空间中的显式线性
  matte，未提供时拒绝导出，不能隐式选择黑色、白色或 UI 背景；
- 只在输出编码边界按明确规则裁剪/量化 RGB；
- 调用方必须提供至少一个来自权威项目安全范围、且当前真实存在的禁止路径；缺失的
  scope 不是可验证保护边界，直接拒绝。现存目录/文件按文件系统身份比较，覆盖大小写、
  短名、hard-link 和目录别名；
- 先写同父目录的独占临时普通文件，刷新、关闭并重新验证，再重命名发布；
- 在原生发布调用前失败或取消，不留下被误认为成品的目标文件，也不覆盖项目源对象；
  进程在临时文件刷新后被强制终止时可以留下私有 `.nps-export-*.tmp`，M1 不承诺
  启动清理；
- 已存在目标的覆盖/版本化策略必须由调用方显式选择，默认拒绝覆盖。

PPM 不携带本项目的颜色 ID，因此输出只能与导出任务记录或测试夹具约定共同解释。
Windows M1 参考导出只接受本地卷，UNC 和映射网络卷在创建临时文件前拒绝，避免把
SMB 响应丢失误报成确定的未提交失败。POSIX 上的原子性与同步结果仍服从目标文件系统；
远程/FUSE 文件系统不在 M1 验收保证内。真实 PNG、JPEG、TIFF、RAW、EXIF 和嵌入
ICC 在 M2。

## 4. 明确非目标

M1 不实现，也不得从本文推断已经实现：

- Qt/QML 桌面 UI、Windows/macOS 成品应用、安装器、签名或公证；
- GPU/D3D12/Metal/Core ML 渲染和 CPU/GPU 差分认证；
- PNG、JPEG、TIFF、HEIF、PSD、RAW 等真实 codec；
- 任意 ICC profile、显示器 profile、软打样、渲染意图或打印颜色；
- ACEScg 渲染/转换；M1 只保留稳定 ID；
- 图层面板、组、完整混合模式、选区、画笔、文字和变换；
- 人像/风景/宠物精修、磨皮、五官微调、换天、对象移除、预设；
- 自然语言、AI 模型、Codec/AI Worker、插件或云 Provider；
- 面向用户的 JPEG/TIFF 导出质量、元数据保留或色彩承诺。

这些能力属于后续 M2 或更晚里程碑；顺序调整必须新增/更新对应里程碑和 ADR。

## 5. 完整测试计划

### 5.1 单元与属性测试

- FP32 RGBA 通道、预乘 Alpha、有限值、透明规范化和 HDR/负值；
- 稳定颜色 ID 的序列化、未知/错配拒绝和缓存隔离；
- DAG 拓扑排序、环/悬空引用拒绝、不可变节点和规范化哈希；
- 曝光数值边界、曲线参数校验与 Mask16 的 `0/1/65535`；
- 随机合法 DAG 重复渲染确定性，随机非法 DAG 永不提交。

### 5.2 渲染差分

- 无节点、单节点和曝光→曲线→蒙版组合金图；
- 透明边缘、零 Alpha、负 RGB、超范围 RGB 和极端 EV；
- 全图、随机 ROI、跨 tile 边界、1 像素边缘和非 512 倍数尺寸；
- 单次全图与 tile 拼接差分；冷/热缓存结果一致；
- 不同调度顺序和线程数下 CPU 参考结果符合明确比较策略；
- 恰好合法的网格和超过 `65,536` tile 的网格分别通过，后者在直接渲染、身份计算和
  带缓存渲染三个入口一致拒绝且不污染缓存。

### 5.3 并发与生命周期

- 渲染前取消、tile 间取消和完成竞态；
- 修订改变造成过期，晚到结果不发布；
- 取消/过期不产生事务、历史或可见项目变更；
- M1 可验证的是同步 `CpuRenderer` 在返回前收拢其 worker，且发布门只接受完整匹配的
  请求身份；它不持有 `ProjectStore`，也没有上层异步任务生命周期接口，因此“关闭项目
  时仍有后台任务的安全收敛”延期到引入任务调度/项目会话层的后续里程碑，不能计为
  M1 已通过项；
- 故障任务不污染相同/不同快照缓存。

### 5.4 迁移、恢复与导出

- 从真实 M0 实现生成的 v1 合成语料迁移到 v2；
- 空历史、曝光、撤销/重做、分支及幂等事实的语义保持；
- 迁移各持久化阶段故障注入、重试和旧项目回退；
- 带未知触发器或非规范 Schema 的 v1 在打开/迁移的首次写入前拒绝；触发器不得改变
  来源事务、幂等记录、格式标识或来源摘要；
- v2 重开与对象/DAG/颜色 ID 损坏拒绝；
- PPM 导出逐字节金图、透明像素显式 matte、缺失 matte 拒绝、已存在目标、取消、
  I/O 失败和发布前强制终止；
- 缺失 forbidden scope、项目内/文件身份别名、Windows UNC/网络卷和设备 namespace
  在临时文件创建前拒绝；
- 原生发布前失败不改变项目修订或原目标；发布原子性只按上节明确的本地文件系统边界
  验收。

进程级导出恢复测试通过
`crash-export-after-flush <v2-project> <output.ppm>` 强制终止：允许留下一个不具备
成品名称的私有临时文件，但不得出现目标 `.ppm`。随后以
`export-demo <v2-project> <output.ppm>` 重试并验证完整 P6 输出；自动清理上一次
崩溃遗留的临时文件不是 M1 范围。

### 5.5 门禁

目标提交必须完成：

1. 高警告级别 Debug 与至少一个 Release 构建；
2. 完整 CTest，而不是只运行新增测试；
3. UTF-8、文档链接、公开文件安全、Gitleaks 与依赖检查；
4. Windows、Linux、macOS CI 的 M1 CPU 路径；
5. 项目迁移和原子导出的进程级故障测试；
6. 人工检查仓库不含真实照片、项目、路径、凭据或构建产物。

CI 配置文件存在不等于远端三平台通过；只有对应提交的实际检查结果可作为证据。

### 5.6 2026-07-26 本地验证证据

在全新、独立的 Windows x64 构建目录中，使用 CMake 4.4.0、Ninja Multi-Config、
MSVC 19.50.35730.0、固定 vcpkg baseline
`40f3c709db80acf154ac4b17a1f83c564ebd022e`，并启用
`NPS_WARNINGS_AS_ERRORS=ON`：

- Debug 从零配置和编译通过，最终跨工具链及路径兼容修复后完整 CTest 为
  `112/112`，并额外连续重复完整套件三次，均为 `112/112`；
- Release 从零编译及最终增量安全构建通过，完整 CTest 为 `112/112`；
- Catch2 单元层为 `111` 个用例、`2799` 条断言；
- 串行进程测试实际覆盖提交恢复、`v1 → v2` 迁移故障、导出 hard-exit 和安全重试；
- Release 安装树包含 CLI、OpenSSL/SQLite 运行时和许可声明；从安装树实际完成
  `demo → verify v1 → migrate-v1-v2 → verify v2 → export-demo`，输出通过 P6
  头与完整长度校验；
- npm 公共仓库门禁通过：`100` 个 UTF-8 文本文件、`37` 份 Markdown、`51` 个内部
  链接、`100` 个公开安全扫描文件和 `23/23` 项策略测试；
- 固定 Gitleaks 8.30.1 使用评审后的公共配置完成工作树和可达 Git 历史扫描，未发现
  泄漏。

Windows 当前账户没有创建文件或目录符号链接的权限，因此对应 symlink 夹具以明确
warning 跳过；hard-link、ADS、Windows namespace、缺失保护范围和网络路径拒绝分支
均在本机实际执行。POSIX 父目录别名、`symlink/..` 真实解析语义，以及最终项目根和
数据库的 symlink 拒绝必须由远端 Linux/macOS 任务实际覆盖。CTest 外壳通过不代表
这些分支已在本机执行，远端首次运行仍需逐项核对。

### 5.7 CI 与公共仓库安全契约

M1 工作流配置必须保持以下边界：

- `core` 使用显式版本的 `ubuntu-24.04`、`windows-2022` 和 Apple Silicon
  `macos-15` runner；这些托管镜像仍会由 GitHub 更新，精确镜像版本以实际任务日志
  为准，不能声称位级可复现；
- 第三方 Action 使用完整提交 SHA，checkout 明确关闭凭据持久化；
- vcpkg 在 bootstrap 前核验检出的 `HEAD` 与 `vcpkg.json` 的完整 baseline 一致；
  policy job 使用精确 Node.js 版本，并对固定 Gitleaks 下载做 SHA-256 校验后同时扫描
  当前工作树和可达 Git 历史；
- 普通 CI 令牌仅有 `contents: read`；CodeQL 顶层默认无权限，只在分析 job 授予
  `contents: read` 与 `security-events: write`；
- 公共安全策略拒绝 `pull_request_target`、self-hosted runner、继承全部 secrets、
  持久化 checkout 凭据、宽泛权限与未按 digest 固定的容器；
- 迁移/导出 hard-exit 场景属于串行进程级 CTest，具有独立标签和有界超时。

远端仓库仍需配置分支保护，把 policy、三平台 core、sanitizer 与 CodeQL 设为后续
合并前必需检查。仓库内工作流不能证明远端保护规则已经启用；该项属于仓库治理状态，
必须通过 GitHub 设置的独立只读复核确认，不能由源码提交自行宣称。

### 5.8 2026-07-27 远端验证证据

PR #1 的目标 head 为
[`203039fd9f7f71130f7b357d9fc976f34a92286c`](https://github.com/harveyxiacn/natural-photo-studio/commit/203039fd9f7f71130f7b357d9fc976f34a92286c)；
GitHub 在合并候选 `887b4f85b110af4da77600b7643705e61df39b38` 上完成以下实际任务：

- [CI run 30320734172](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172)
  总体成功；
- [Public-source policy](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172/job/90155872494)
  成功，包含公共源码策略与固定 Gitleaks 历史扫描；
- [Ubuntu 24.04 dev](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172/job/90155909913)
  成功，`110/110`；
- [macOS 15 arm64 dev](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172/job/90155909895)
  成功，`110/110`；日志确认 POSIX 父目录别名、`symlink/..`、悬空最终链接、最终
  项目根/数据库链接拒绝及运行时校准缓存测试均实际执行并通过；
- [Windows Server 2022 release](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172/job/90155909920)
  成功，`112/112`；
- [ASan/UBSan](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734172/job/90155909873)
  成功，`110/110`；
- [CodeQL run 30320734148](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734148)
  及其 [Analyze C/C++](https://github.com/harveyxiacn/natural-photo-studio/actions/runs/30320734148/job/90155872224)
  成功，分析结果完成上传。

Unix 与 Windows 的 CTest 数量差异来自平台条件测试，不是缺测：Unix 实际执行 POSIX
链接语义，Windows 实际执行其专属路径/替换测试。上述链接是首次全绿目标提交的证据；
后续仅更新验收文档的提交仍须再次通过同一组门禁。

## 6. M1 完成定义

只有下列条件全部满足，本文状态才可改为 `Implemented`：

1. 第 3 节所有范围在源码、规范和测试之间可追踪；
2. 第 5 节本地完整门禁通过，并记录精确命令、平台、用例和断言数量；
3. 公共 GitHub 目标提交的三平台 CI、安全与 CodeQL 检查通过；
4. `nps.project/v2` 格式与 `v1 → v2` 迁移规范进入版本控制；
5. 迁移和导出故障测试证明不会发布半完成结果；
6. M1 非目标在 README、CLI 帮助和发布说明中没有被夸大；
7. 里程碑提交经隐私/安全复核后提交并推送。

截至 2026-07-27，本地实现、Windows Debug/Release、进程恢复、安装冒烟、公共仓库
策略，以及目标提交的 Windows/Linux/macOS、Sanitizer 和 CodeQL 均形成上述证据，
因此 M1 达到 `Implemented`。分支保护、仓库安全设置和 PR 合并仍按仓库治理流程单独
执行与验证，不扩大 M1 的产品功能范围。

## 7. 后续入口

M2 应在 M1 CPU 参考与项目 v2 契约上增加真实 codec 和颜色管理，至少另行决定：

- Codec Worker、恶意图片隔离和元数据策略；
- JPEG/PNG/TIFF 与首批 RAW 支持矩阵；
- ICC v2/v4、缺失/损坏 profile、显示转换和渲染意图；
- 内建 Rec.2020 与 ACEScg 是否都成为面向用户的工作空间；
- GPU 后端与 FP16 优化相对 M1 FP32 参考的颜色/数值预算；
- 面向用户的原子导出、覆盖保护和元数据往返。
