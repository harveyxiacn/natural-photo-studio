# 命令与插件契约

## 1. 单一操作协议

按钮、快捷键、自然语言、预设、批处理、CLI 和插件最终都构造同一种版本化命令。不存在“AI 特权路径”或“插件直接改文档”。

## 2. 命令信封

```json
{
  "schema": "nps.command/v1",
  "commandId": "019c-example",
  "idempotencyKey": "client-session-12:84",
  "documentId": "doc-001",
  "expectedRevision": 128,
  "kind": "portrait.retouch",
  "target": {
    "layerIds": ["layer-main"],
    "subjects": [{"type": "face", "id": "face-0"}],
    "region": {"type": "selection", "id": "selection-current"}
  },
  "params": {
    "skinSmoothing": 0.28,
    "texturePreservation": 0.82,
    "blemishRemoval": 0.55,
    "faceShape": {"jaw": -0.06, "eyeSize": 0.02}
  },
  "constraints": ["preserve_identity", "preserve_background"],
  "execution": {
    "mode": "preview",
    "quality": "interactive",
    "device": "auto",
    "deterministicSeed": 39182
  },
  "privacy": {
    "network": "deny",
    "cloudInference": "deny"
  },
  "client": {
    "source": "natural-language",
    "locale": "zh-CN"
  }
}
```

字段语义：

- `commandId`：单次请求 ID；
- `idempotencyKey`：重试时不重复提交；
- `expectedRevision`：防止操作落到过期文档；
- `kind`：注册表中的稳定操作；
- `target`：实例、图层与空间区域；
- `params`：由对应 Schema 校验；
- `constraints`：跨操作的保护规则；
- `execution`：预览/提交、质量和设备；
- `privacy`：本命令允许的数据边界；
- `client.source`：UI、语言、预设、批量、CLI 或插件。

## 3. 操作注册表

每种 `kind` 声明：

```yaml
kind: object.remove
version: 2
risk: R3
capabilities:
  - pixels.read.selection_context
  - mask.create
  - generated_asset.create
parameters:
  includeShadow: {type: boolean, default: true}
  includeReflection: {type: boolean, default: auto}
  candidateCount: {type: integer, min: 1, max: 4, default: 3}
supports:
  preview: true
  cancel: true
  batch: guarded
  offline: degraded
```

参数必须有单位、范围、默认值、语义版本和迁移规则。未知操作或不兼容主版本必须拒绝，不能猜测。

## 4. 目标与区域

目标支持：

- 稳定图层 ID；
- 项目内主体/人脸/宠物实例 ID；
- 语义区域；
- 当前或命名选区；
- 矩形/多边形/画笔蒙版；
- 关系查询解析后的固定实例集合。

命令提交时把语言查询冻结为具体 ID。渲染期间不重新解释“右边的人”，避免对象顺序变化。

## 5. 预览与提交

`execution.mode`：

- `analyze`：只生成分析资产；
- `preview`：临时分支，可返回多候选；
- `commit`：原子正式事务；
- `export`：锁定快照的只读渲染；
- `dry_run`：只校验能力、风险、成本和预计时间。

高影响操作先 `preview`。应用时可以提升已有候选，不重复生成；若 `expectedRevision` 已过期，要求重新基于当前修订预览。

## 6. CommandBatch

复合自然语言和配方使用批次：

```json
{
  "schema": "nps.command-batch/v1",
  "batchId": "batch_01J...",
  "atomic": true,
  "expectedRevision": 42,
  "commands": [
    {"kind": "sky.replace", "...": "..."},
    {"kind": "object.remove", "...": "..."},
    {"kind": "preset.apply", "...": "..."}
  ]
}
```

- `atomic=true`：任一步失败不提交；
- 命令可声明依赖并共享分析资产；
- 批次风险为最高子命令风险；
- UI 显示为一个高层历史项，可展开子步骤；
- 大型生成候选在预览分支完成后再一次提交。

## 7. 响应与事件

同步响应：

```json
{
  "commandId": "019c-example",
  "status": "accepted",
  "taskId": "task_01J...",
  "baseRevision": 128,
  "warnings": [],
  "estimated": {"milliseconds": 2800, "vramMiB": 950}
}
```

长任务状态：

```text
queued → running → preview_ready → committed
                  ↘ needs_review
queued/running/preview_ready → cancelled
任意非终态 → failed
```

事件带单调序号和任务 ID：

- `progress`：阶段、百分比、可取消性；
- `candidate_ready`：候选、缩略图和 QA；
- `needs_review`：风险或异常区域；
- `degraded`：设备/模型降级；
- `revision_committed`：新修订与变更清单；
- `failed`：稳定错误码和恢复建议。

## 8. 错误分类

| 前缀 | 示例 | 行为 |
|---|---|---|
| `CMD_*` | 参数范围、未知操作 | 修正请求，不重试 |
| `REV_*` | 修订冲突、目标过期 | 重新解析/预览 |
| `CAP_*` | 硬件、模型、格式不支持 | 降级或提示 |
| `PERM_*` | 文件、网络、云端权限 | 请求明确授权 |
| `RES_*` | 内存、显存、磁盘不足 | 分块/释放/改目标 |
| `MODEL_*` | 加载、输出、QA 失败 | 换模型/回滚 |
| `IO_*` | 解码、保存、导出失败 | 不提交，保留原文件 |
| `PLUGIN_*` | 超时、崩溃、越权 | 终止并隔离插件 |
| `INTERNAL_*` | 未分类故障 | 记录脱敏诊断 ID |

错误消息不能只显示代码，必须说明项目是否已修改和下一步。

## 9. 权限能力

能力最小化：

- `document.read.structure`
- `pixels.read.viewport`
- `pixels.read.selection_context`
- `mask.create`
- `node.create.<namespace>`
- `generated_asset.create`
- `file.open.picker`
- `file.export.picker`
- `network.domain.<host>`
- `cloud_inference.<provider>`
- `metadata.read/write`
- `gpu.compute`

能力绑定项目、目标、时间和调用者。插件或语言计划不能把一次“读取选区”升级为“读取所有照片”。

## 10. 插件类型

- 调整/滤镜节点；
- 导入/导出和元数据；
- AI 模型 Provider；
- 自动化与批处理配方；
- 面板和资源浏览器；
- 选区、画笔、生成式工具。

隔离顺序：

1. WASM/WASI：普通滤镜和分析器首选；
2. 独立原生进程：模型、编解码、需要原生库的插件；
3. 主进程原生模块：只允许签名、审计并随应用发布的第一方组件。

## 11. 插件清单

```yaml
id: com.example.film-look
name: Example Film Look
version: 1.2.0
sdkVersion: 1
entrypoints:
  filter: module.wasm
capabilities:
  - filter.tile
  - preset.register
permissions:
  projectPixels: selection-only
  projectMetadata: read
  fileExport: none
  networkDomains: []
resources:
  maxMemoryMiB: 512
  maxGpuMiB: 1024
  timeoutMs: 10000
signature:
  required: true
```

安装前显示人类可读权限。插件不能读取项目数据库、获取永久 GPU 指针、枚举任意文件或绕过导出选择器。

## 12. 插件像素接口

宿主以短期句柄提供：

- 指定 ROI、分辨率、颜色域和 Alpha 语义；
- 带 halo 的只读输入瓦片；
- 受限输出缓冲区；
- 取消、进度和内存预算；
- 参数 Schema；
- 可选确定性种子。

插件输出经过边界、NaN/Inf、Alpha、颜色域和资源使用检查，再成为临时节点结果。

## 13. 兼容与撤销

- 命令、插件 SDK 和清单使用语义化主版本；
- 当前版本至少提供上一主版本的迁移窗口；
- 节点插件缺失时使用冻结结果或占位只读节点；
- 插件崩溃不能破坏项目；
- 支持签名吊销和安全模式；
- 插件生成的正式编辑仍是普通事务，可撤销；
- 模型或插件更新不得静默重渲染已冻结结果。

## 14. 审计记录

每个事务记录：

- 命令/批次 ID、来源和时间；
- 基础/新修订；
- 操作类型和规范化参数摘要；
- 目标 ID、蒙版修订；
- 模型/插件/算法版本；
- 本地/云端和授权策略；
- 生成资产哈希；
- QA 与降级；
- 用户应用、取消或覆盖行为。

默认不记录像素、提示原文、完整路径或人脸特征。
