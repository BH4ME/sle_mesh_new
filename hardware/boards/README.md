# Boards

这里预留给 SLE 硬件板卡资料。

建议每块板按版本建目录，例如：

```text
hardware/boards/sle-main-board/v1.0.0/
```

每个版本建议包含：

- `README.md`：板卡用途、版本摘要、接口说明。
- `MANIFEST.md`：Gerber、BOM、装配图、3D 模型和验证记录清单。
- `gerber/`：可制造 Gerber/钻孔文件。
- `bom/`：BOM 和替代料说明。
- `assembly/`：装配图、位号图、生产注意事项。
- `step/`：板卡 3D 模型。
