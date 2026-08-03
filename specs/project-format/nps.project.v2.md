# `nps.project/v2`：M1 不可变编辑图项目格式

状态：Implemented（M1）

适用版本：Natural Photo Studio `0.2.x`

实现入口：`include/nps/core/project_store.hpp`、`src/core/project_store.cpp`

## 1. 范围

`nps.project/v2` 在 v1 的对象、快照、线性历史、事务和幂等语义上增加明确的颜色契约与
不可变编辑图。v2 仍是 M1 的窄格式，不代表 1.0 项目格式已经冻结。

v2 新增的项目真相是：

- 每个快照的 canonical `nps.edit-graph/v1` JSON；
- canonical JSON 的小写 SHA-256；
- 每个快照明确保存的工作颜色 ID；
- 从 v1 迁移时的不可逆向识别来源摘要。

M1 的迁移只处理 v1 已登记的嵌入式 16 位 P6 PPM 对象。真实照片 codec、ICC profile、
RAW、图层、外链资产、生成式补丁和单文件 `.nps` 包不在本规范内。

## 2. 容器和格式识别

项目仍是名称以小写 `.npsproj` 结尾的真实目录，目录骨架和对象路径沿用 v1。数据库
必须同时满足：

- SQLite `application_id = 0x4E505331`；
- SQLite `user_version = 2`；
- `meta.format = "nps.project/v2"`。

三者不一致时拒绝打开。未知未来版本不得以 v2 写入，也不得通过猜测字段继续编辑。
普通 `ProjectStore::open()` 可以明确识别 v1 或 v2，但打开 v1 不会触发迁移或改写
Schema。迁移只能由 `ProjectStore::migrate_v1_to_v2()` 或对应 CLI 操作显式发起。

## 3. 数据库

v2 沿用 v1 的 `meta`、`objects`、`history`、`transactions` 和 `idempotency` 表及其
约束。迁移保留这些表中的所有行、主键、快照引用、修订号、命令 ID、幂等键和请求
指纹。

与 v1 相同，物理 Schema 是精确的版本化契约：只允许六张规范表和 SQLite 为规范
约束自动生成的索引，不允许触发器、视图、额外表、用户创建索引、额外列或弱化后的
DDL。v2 的 `meta` 必须恰好包含六个通用键和三个迁移来源键。普通打开在任何
`clean_shutdown` 写入前验证完整 Schema、精确键集合和全部项目事实；未知对象直接
返回稳定的 Schema 错误，不把对象名、SQL 或项目路径复制到错误文本。

### 3.1 `snapshots`

| 列 | 约束 | 含义 |
|---|---|---|
| `id INTEGER` | 主键 | 迁移时保持 v1 快照 ID |
| `parent_snapshot_id INTEGER` | 可空、自引用外键 | 创建该快照时的父快照 |
| `created_revision INTEGER` | 非空、唯一 | 首次创建快照的项目修订 |
| `source_hash TEXT` | 非空、外键 | 不可变源对象 |
| `exposure_ev REAL` | 非空 | v1 兼容投影；v2 渲染真相是编辑图 |
| `edit_graph_json TEXT` | 非空、1–4 MiB | canonical `nps.edit-graph/v1` UTF-8 JSON |
| `edit_graph_sha256 TEXT` | 非空、64 字符 | canonical JSON 的小写 SHA-256 |
| `working_color_id TEXT` | 非空 | 该快照的稳定工作颜色 ID |

M1 支持的持久化颜色 ID 固定为：

```text
nps.color/scene-linear-rec2020-d65/v1
```

不得用进程内枚举序号、操作系统默认 profile 或“当前默认值”代替该字段。增加其他
颜色空间需要新的显式兼容策略。

`exposure_ev` 为 v1 API 和参考 CLI 保留；它不覆盖 `edit_graph_json`，也不能表达曲线、
蒙版等操作。`graph.replace` 后的正式渲染必须读取编辑图。

### 3.2 canonical 图与摘要

打开和完整性检查会对每个 v2 快照验证：

- JSON 字节长度和严格 canonical 编码；
- Schema、工作颜色 ID、稳定标识符、受支持节点与算法版本；
- 节点输入存在、无重复节点、无自环、无环；
- 所有节点都位于声明的 source-to-output 路径上；
- source/output 的唯一性及 M1 线性拓扑；
- 曝光、RGB 曲线和蒙版绑定的受支持参数边界；
- `edit_graph_sha256` 与 canonical JSON 字节一致；
- 快照的 `working_color_id` 与图内 `workingColorSpace` 一致。

数据库中的未知关键节点、非 canonical JSON、错误摘要、错误颜色 ID 或循环 DAG 都是
格式错误，不进行“尽力修复”。

## 4. v1 到 v2 的确定性映射

每个 v1 快照映射成一个新的 v2 快照行，并保持：

- `id`、`parent_snapshot_id`、`created_revision` 和 `source_hash`；
- 完整 `history` 游标及撤销/重做选择；
- 完整 `transactions` 和 `idempotency` 结果；
- `meta.document_id`、当前修订、当前快照和历史位置；
- v1 源对象的字节、媒体类型、大小和 SHA-256。

无 Alpha 的 M0 PPM 在 M1 中明确解释为 `A = 1`，工作颜色为
`nps.color/scene-linear-rec2020-d65/v1`。这只是 M0 合成 PPM 的版本化兼容解释，
不是从 PPM 字节推断出的 profile。

累计曝光为零时，图为 `source → output`。非零累计曝光映射为一个或多个
`adjust.exposure` 节点，每个节点参数位于 `[-10, 10] EV`，再连接到 output。节点和
graph ID 仅由快照 ID、规范化顺序和已保存曝光值决定，因此相同 v1 数据得到相同
canonical JSON 和摘要。若极端累计曝光无法在 4096 节点上限内表示，迁移明确失败，
不会发布目标。

## 5. 显式 side-by-side 迁移

迁移 API 接收不同的源和目标 `.npsproj` 路径。二者必须是同一真实父目录中的兄弟
路径，目标默认必须不存在。实现不做原地 Schema 更新，也不把 v2 重命名回 v1 路径。

流程如下：

1. 对源路径全部已存在目录祖先、项目根、数据库、对象目录和对象文件进行
   symlink/reparse-point 防护；
2. 获取源 `project.lock` 的排他租约；
3. 以 SQLite 只读连接精确校验 v1 `sqlite_schema`、`meta` 键集合、数据库事实、
   历史和所有登记对象；任何触发器、视图或未知 Schema 对象都在备份前拒绝；
4. 对迁移相关的 v1 元数据及所有表行生成 canonical 来源事实摘要；
5. 在同一父目录创建密码学随机、不可预测的
   `.nps-migrating-<随机值>.npsproj` 私有暂存目录；
6. 重新读取、校验并内容寻址写入每个已登记对象；
7. 使用 SQLite backup API 得到事务一致的数据库副本；在执行转换 SQL 前，再次对
   副本做精确 v1 Schema 校验并重新计算来源事实摘要，必须与第 4 步相同；
8. 在暂存副本的一个数据库事务中建立 v2 `snapshots`、写入所有图、切换格式标识并
   保存来源摘要；提交前把候选 v2 投影回 v1 事实并再次比对第 4 步摘要；
9. 提交后精确校验 v2 Schema，关闭、重开并再次检查 v1 事实投影、完整 v2 项目、
   数据库及发布标记，再刷新到存储；
10. 最后将整个暂存目录 rename 为目标 `.npsproj`；
11. 按正常 v2 打开流程再次校验目标，之后移除 `.nps-migrating` 标记。

源数据库连接在整个构建和发布窗口保持只读，源内容寻址对象不修改、不移动、不删除。
源 `project.lock` 是非项目真相的租约承载文件；若历史项目缺少该文件，获取租约可能
创建它。

精确 Schema 门禁在源和 backup 两侧都执行，因此迁移器不会复制后再执行未知触发器。
来源摘要的三次比较分别约束“已校验源”“待转换副本”和“待发布 v2 的保留投影”；
任一阶段不一致都回滚或丢弃本次独占 staging，不发布目标。

v1 已声明为非项目真相的 `previews/`、`recovery/`、`manifests/` 内容不会复制到 v2；
目标会建立空目录。未登记在 `objects` 表中的磁盘孤儿也不会复制。源 v1 完整保留，
因此这些非真相资料仍可由用户审查或手工保留。

## 6. 幂等与目标冲突

v2 保存：

| `meta` 键 | 含义 |
|---|---|
| `migration_source_format` | 固定 `nps.project/v1` |
| `migration_source_document_id` | 来源文档不透明 ID |
| `migration_source_fingerprint` | 忽略运行态 `clean_shutdown` 后，对 v1 项目事实的 SHA-256 |

如果目标已经存在，迁移器只在以下条件全部成立时把调用视为幂等成功：

- 目标是完整且可打开的 v2；
- 目标文档 ID 与源相同；
- 保存的来源文档 ID 与源相同；
- 保存的来源事实摘要与当前源相同；
- v2 完整性检查通过。

匹配目标可以已经在迁移后继续产生 v2 编辑；重试不会覆盖这些编辑。任何不匹配、
损坏、未知版本或被其他进程锁定的目标均拒绝，不删除、不替换、不合并。

## 7. 崩溃边界

| 故障位置 | 正式可见状态 | 重试 |
|---|---|---|
| 暂存目录创建或对象/数据库写入中 | 源仍是完整 v1；目标不存在；可能留下私密随机 staging | 对同一目标重新迁移 |
| 暂存 v2 完整校验后、rename 前 | 源仍是完整 v1；目标不存在；staging 是完整但未发布 v2 | 对同一目标重新迁移 |
| rename 成功后、首次目标打开前 | 源完整 v1；目标是带合法标记的完整 v2 | 再次迁移或打开会校验并完成标记 |
| 目标首次打开后 | 源完整 v1；目标完整 v2 | 来源摘要匹配时幂等返回目标 |

故障注入使用独立进程立即退出，覆盖“暂存完成但未发布”和“rename 已完成但未最终
打开”两个边界。任何可捕获异常都会尽力删除本次调用独占的 staging；强制终止可能
留下它。

目录 rename 的原子性和持久性仍取决于本地文件系统。M1 在 rename 前同步数据库和
标记，但没有对父目录执行跨平台目录 `fsync`，也不承诺远程文件系统、损坏设备或任意
断电组合具有相同保证。

## 8. v2 编辑事务

v2 保留 v1 的修订、乐观并发、历史、事务和幂等原子性。

- `adjust.exposure` 在新快照图尾部增加版本化曝光节点；
- `graph.replace` 只接受 canonical 图、正确摘要和明确颜色 ID，在同一 SQLite 事务
  中创建新不可变快照、截断前向历史并记录事务/幂等结果；
- v1 收到 `graph.replace` 必须返回 `IO_PROJECT_MIGRATION_REQUIRED`，不能把图静默
  压缩成累计曝光；
- undo/redo 只移动到已有不可变快照，仍会增加项目修订；
- 未知命令和无效图在事务提交前失败，项目修订、历史和当前快照不变。

## 9. 隐私与安全

迁移全程本地执行，不上传源、图、项目路径或诊断内容。公开测试只使用代码生成的
确定性合成 PPM。

源、目标、SQLite WAL、对象暂存和 `.nps-migrating-*` 都可能包含完整照片。随机名称
防止猜测冲突，不是加密。清理失败不等于安全擦除；备份、同步和诊断工具必须把迁移
staging 当作正式项目同等私密的数据。
