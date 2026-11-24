# Proof-Bank 证明增量归档

`bank-entries/` 每个棋盘一份 `.m3bank` 文件，每份文件按次序保存该棋盘所有新增已证安全后继。每条记录以一行 `M3PBK1` 元数据开头，紧接指定字节数的完整 `M3CERT 1`。空文件表示该棋盘未新增证明。

元数据按顺序保存棋盘 ID、证明编号、补充上限、连锁上限、行列及调色板、父状态剩余目标和步数及全棋盘格子、交换坐标、波次数及每波完整补充、子状态剩余目标和步数及全棋盘格子、证书字节数。每个颜色以两个十六进制字符保存。坐标为行优先平铺位置。导出器只写入带有完整续局证书的非终局后继。

独立验证器先检查父状态稳定且交换合法，再完整回放每一波并比对子状态。随后使用公开 `verify_certificate` 验证完整续局证书同一规则域、目标和步数。每个棋盘的新增后继按精确状态去重。附加 CLI 检查从每份非空归档取证书字节数最大的记录，复核并做错误目标拒绝检查。

`proof-bank-manifest.json` 的 SHA-256 绑定原有输入棋盘及证书、冻结源码、规则和每份新增证明归档。`verified-entries.csv` 可按棋盘追溯，`independent-replay.json` 给出全量核验计数。归档只封存本轮增加的已证后继。基线目录仍可用随包锁定的代码和原始输入复跑构建。当前测量使用 WSL 进程内冻结的索引，实际服务的库加载和版本切换另做集成验证。

在包根目录、固定目标求解器已经编译的前提下，以下命令复核封存摘要。

```bash
python3 match3_proofbank/audit_proof_bank.py verify \
  match3_proofbank/results/bank-audit/proof-bank-manifest.json \
  match3_proofbank/results/targeted-warm/bank-entries \
  match3_ablation/fresh/dataset
```
