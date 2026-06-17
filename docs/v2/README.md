# V2 文档入口（自动组网）

`docs/v2/` 聚焦自动 relay 选举、member 自动选父、relay 失效自愈的 1vs20 逻辑组网。

## 核心文档

- [networking-goal.md](<repo-root>/docs/v2/networking-goal.md)：V2 目标、约束、验收标准
- [review_framework.md](<repo-root>/docs/v2/review_framework.md)：审查执行框架
- [protocol.md](<repo-root>/docs/v2/protocol.md)：V2 协议总览

## 说明

- 审查结果由 `scripts/review/run_review_with_service.sh` 生成到本地 `meta/review_feedback.md`，该文件不提交到 GitHub。
- V0/V1 文档分别位于 `docs/v0` 与 `docs/v1`，用于对比基线与演进路径。
