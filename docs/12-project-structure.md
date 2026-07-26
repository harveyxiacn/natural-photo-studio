# 项目目录结构

## 1. 当前阶段

仓库已完成 M0 工程基线：`include/nps`、`src`、`apps/cli`、`tests`、
`specs/commands`、`specs/project-format`、`cmake`、`scripts` 和
`.github/workflows` 已实际创建。M0 采用紧凑目录承载命令、CPU 参考成像和
项目存储；下文仍是产品扩展到桌面端、GPU、色彩、AI 与插件后的目标结构。
目录只有在对应里程碑产生可运行代码和测试时才继续展开。

## 2. 计划结构

```text
natural-photo-studio/
├─ README.md
├─ CHANGELOG.md
├─ CMakeLists.txt
├─ CMakePresets.json
├─ cmake/
├─ docs/
│  ├─ 00-document-map.md
│  ├─ 01-product-vision-and-scope.md
│  ├─ 02-product-requirements.md
│  ├─ 03-ux-interaction-design.md
│  ├─ 04-feature-specifications.md
│  ├─ 05-natural-language-editing.md
│  ├─ 06-ai-image-pipeline.md
│  ├─ 07-system-architecture.md
│  ├─ 08-edit-document-model.md
│  ├─ 09-command-and-plugin-contracts.md
│  ├─ 10-security-privacy-ethics.md
│  ├─ 11-quality-performance-testing.md
│  ├─ 12-project-structure.md
│  ├─ 13-delivery-roadmap.md
│  ├─ 14-model-data-governance.md
│  ├─ 15-release-operations.md
│  ├─ 16-glossary-open-questions.md
│  └─ adr/
├─ specs/
│  ├─ commands/
│  ├─ project-format/
│  ├─ plugin-manifest/
│  ├─ model-manifest/
│  └─ error-catalog/
├─ apps/
│  ├─ desktop/
│  │  ├─ qml/
│  │  ├─ src/
│  │  ├─ resources/
│  │  └─ platform/
│  └─ cli/
├─ engine/
│  ├─ document/
│  ├─ commands/
│  ├─ render/
│  ├─ imaging/
│  ├─ color/
│  ├─ selection/
│  ├─ geometry/
│  ├─ metadata/
│  └─ project_store/
├─ services/
│  ├─ codec_worker/
│  ├─ ai_worker/
│  ├─ plugin_host/
│  └─ updater/
├─ ai/
│  ├─ contracts/
│  ├─ registry/
│  ├─ router/
│  ├─ runtimes/
│  ├─ adapters/
│  ├─ analysis/
│  │  ├─ segmentation/
│  │  ├─ matting/
│  │  ├─ depth/
│  │  ├─ face/
│  │  └─ pet/
│  ├─ pipelines/
│  │  ├─ portrait/
│  │  ├─ landscape/
│  │  ├─ sky/
│  │  ├─ pet/
│  │  └─ inpainting/
│  └─ quality/
├─ language/
│  ├─ intent/
│  ├─ references/
│  ├─ planner/
│  ├─ edit_dsl/
│  ├─ validators/
│  └─ providers/
├─ presets/
│  ├─ schema/
│  ├─ first_party/
│  └─ importers/
├─ sdk/
│  ├─ action/
│  ├─ plugin/
│  └─ provider/
├─ plugins/
│  ├─ first_party/
│  └─ examples/
├─ models/
│  ├─ manifests/
│  ├─ cards/
│  └─ licenses/
├─ resources/
│  ├─ icons/
│  ├─ themes/
│  ├─ color/
│  └─ licensed_assets/
├─ tests/
│  ├─ fixtures/
│  ├─ unit/
│  ├─ graph/
│  ├─ color/
│  ├─ model_contract/
│  ├─ golden_images/
│  ├─ visual_regression/
│  ├─ integration/
│  ├─ e2e/
│  ├─ fuzz/
│  ├─ performance/
│  ├─ security/
│  └─ recovery/
├─ tools/
│  ├─ benchmark_runner/
│  ├─ project_inspector/
│  ├─ model_validator/
│  ├─ asset_license_scanner/
│  └─ model_conversion/
├─ packaging/
│  ├─ windows/
│  └─ macos/
├─ third_party/
│  ├─ manifests/
│  ├─ patches/
│  └─ notices/
└─ .github/
   └─ workflows/
```

## 3. 目录职责

| 目录 | 所有权 | 说明 |
|---|---|---|
| `specs` | 架构委员会 | 跨语言、跨进程、对外兼容的机器可读规范 |
| `apps` | 客户端团队 | UI 和组合层，不承载图像业务真相 |
| `engine` | 图像内核团队 | 确定性文档、渲染、颜色和项目存储 |
| `services` | 平台/安全团队 | 隔离 Worker 与更新 |
| `ai` | AI 工程团队 | 模型能力、分析、管线和 QA |
| `language` | 智能交互团队 | 受限计划器和引用解析 |
| `presets` | 产品/影像团队 | 配方 Schema、第一方风格和导入器 |
| `sdk/plugins` | 平台生态团队 | 外部扩展契约与第一方插件 |
| `models` | 模型治理负责人 | 清单、模型卡和许可证；大权重不进普通 Git |
| `resources` | 设计系统/法务 | 应用资源及授权记录 |
| `tests` | 各模块 + QA | 测试按类型集中，fixtures 有许可 |
| `tools` | 开发体验团队 | 检查器、基准和离线工具 |
| `third_party` | 构建/法务 | 锁定、补丁和 NOTICE |

## 4. 依赖规则

- `engine/document` 不依赖 UI、网络、AI 运行时或文件选择器；
- `engine/render` 只读不可变快照；
- `ai` 返回资产和建议，不调用文档写 API；
- `language` 只产生 `specs/commands` 中的操作；
- `apps` 所有永久修改都经 `engine/commands`；
- `services` 通过版本化 IPC，不能链接 UI 私有状态；
- 第三方插件只依赖 `sdk`；
- 测试 fixture 不引用项目目录外的私人照片；
- 大模型权重、客户素材、缓存和生成结果禁止提交。

建议在构建图中自动验证依赖方向，发现反向依赖直接失败。

## 5. Schema 为先

以下内容先在 `specs` 定义，再生成 C++/QML/测试绑定：

- 命令、响应和事件；
- 节点、蒙版和项目 manifest；
- 插件与模型清单；
- 稳定错误目录；
- Provider 能力。

生成文件放在构建目录，不手工修改；Schema 兼容测试比较上一个稳定版本。

## 6. 测试素材规则

- `fixtures/public`：许可明确、可提交的小型测试图；
- `fixtures/generated`：程序生成的颜色、几何和异常文件；
- `fixtures/private-local`：只用于本机，强制忽略；
- 大型金图存专用制品库，仓库只存 manifest 和哈希；
- 人像评测集加密、访问审计，不进入普通 CI；
- 每项素材记录来源、许可、人物同意、用途和删除日期。

## 7. 分支与代码所有权

- `main` 始终可构建、可迁移项目；
- 公开 Schema、项目格式、色彩、命令和安全边界需双人评审；
- 模型/预设更新与应用代码同等走质量门禁；
- 生成式大文件使用制品仓库，不用 Git LFS 作为隐私边界；
- `CODEOWNERS` 覆盖 `specs`、`engine/color`、`project_store`、`security`、模型清单和 updater。

## 8. 初始化顺序

技术尖峰通过后按顺序建立：

1. `specs` 和 Schema 兼容测试；
2. `engine/document`、`commands`、`project_store`；
3. CPU 参考 `imaging/color/render`；
4. `apps/desktop` 最小画布；
5. GPU 后端；
6. Codec Worker 与 RAW；
7. AI Worker 和一个分割模型；
8. Language Planner 的受限命令；
9. 插件 Host；
10. 完整测试、工具和包装。

这个顺序先验证项目完整性和像素链路，再叠加 AI。
