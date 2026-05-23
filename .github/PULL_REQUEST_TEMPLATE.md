## 变更摘要

- 说明这次改了什么
- 说明为什么要改

## 影响范围

- [ ] ELF / DWARF 解析
- [ ] 分析模型 / 快照
- [ ] GUI
- [ ] CLI
- [ ] 配置 / 版本检查
- [ ] 构建 / 发布
- [ ] 测试 / fixture
- [ ] 文档

## 验证

请贴出你实际运行过的命令和结果摘要：

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build
ctest --test-dir build --output-on-failure
```

如果没有运行，请说明原因。

## 截图或示例输出

- 如有 GUI 改动，请补截图
- 如有 CLI / JSON 变更，请补关键输出

## 兼容性与发布

- [ ] 不影响现有配置文件
- [ ] 需要更新 `README.md`
- [ ] 需要更新发布说明
- [ ] 需要在 Release 中特别提示

## 关联

- Closes #

