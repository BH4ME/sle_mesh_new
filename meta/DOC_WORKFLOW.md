# Documentation Workflow

这份文件定义文档维护规则：写在哪里、什么时候更新、如何避免仓库再次变乱。

## 分层

- 根目录 [../README.md](../README.md)：项目入口、目录结构、构建、烧录、测试和硬件入口。
- [../docs/README.md](../docs/README.md)：文档索引。
- [PROJECT_OPERATION_SOP.md](PROJECT_OPERATION_SOP.md)：项目操作 SOP。
- [../versions/README.md](../versions/README.md)：版本记录索引。
- [../hardware/README.md](../hardware/README.md)：硬件资料入口。

## 写作规则

- 新功能优先更新已有文档，不为同一主题新建重复 README。
- 仓库结构变化先更新 `README.md`、`docs/repository_layout.md` 和 `docs/version_management.md`。
- 固件行为变化必须更新对应 `versions/<version>/VERSION.md` 和 `MANIFEST.md`。
- 硬件资料确认可发布后放入 `hardware/<category>/<name>/<version>/`，不要直接堆在仓库根目录。
- 临时计划、审查截图、模型草稿和本地日志不提交到 GitHub。

## 审查脚本

审查辅助入口：

```sh
scripts/review/run_review_with_service.sh --dry-run
```

审查输出固定写入本地 `meta/review_feedback.md`，该文件已加入 `.gitignore`，不提交到 GitHub。

## 修改后检查

```sh
git diff --check
npm --prefix webui test
python -m unittest discover -s automation/ws63/tests -t .
```

按变更范围选择必要检查；硬件资料变化至少检查 MANIFEST、README 和文件路径是否一致。
