# Match3 research and reproducibility

[简体中文](README.zh-CN.md) · [Research release and archives](https://github.com/rock2matrix/match3-strict/releases/tag/research-2026-11-24)

This directory records how the Strict solver was used to score candidates, build certified pools, compare refill controllers and expand an offline proof bank. The experiments use the pinned solver revision [71aac8a](https://github.com/rock2matrix/match3-strict/tree/71aac8aab2e3fae2fa798e075f0f7d52b1220754).

## Current decision

The 120 admitted experimental levels use fixed certified witnesses and the frozen strict-random-tier-v1 standard. Higher ease values indicate easier simulated play. The V1 hard/normal cutoff is **0.554** and the normal/easy cutoff is **0.651**. Proof-Bank v1.1 is a verified, versioned delta over the original proof collection. ACR remains a research controller.

| Proof-Bank v1.1 result | Observation |
| --- | --- |
| Added continuations | 8,628, all independently replayed |
| Certified-safe mass on 92,760 matched proposals | 72.29% → 76.78% |
| Strict game completion | 409,600 / 409,600, zero uncertified executions |
| ACR witness fallback | 5.90% with the expanded bank, against a 5% target |
| Same-bank Fixed-6 fallback | 3.79% |
| ACR CPU saving against same-bank Fixed-6 | 4.37% cold, 3.86% warm, against a 10% target |

ACR v1.1 has a separate research calibration. Its hard/normal cutoff is **0.690104** and its normal/easy cutoff is **0.744792**. The independent 200-board validation yielded 72 hard, 51 normal and 77 easy levels. These cutoffs apply to the expanded proof bank and its ACR controller.

## Source and evidence

| Stage | Source tree | Published archive |
| --- | --- | --- |
| Agent simulation, V1 calibration, pool construction | [difficulty](match3_difficulty/) and [pool](match3_pool/) | match3-research-complete-2026-11-24.zip |
| Cascade ablation | [ablation](match3_ablation/) | match3-cascade-ablation-wsl.zip |
| V4 productive cascades and controlled refill | [V4](match3_v4/) | match3-v4-refill-wsl.zip |
| ACR v1.0, cold/warm replay and WSL load | [ACR](match3_acr/) | match3-acr-wsl.zip |
| Proof-Bank v1.1, paired tests and archive verification | [Proof-Bank](match3_proofbank/) | match3-proof-bank-v11-wsl.zip |

The Git tree keeps sources, frozen plans and compact summaries. Release assets carry the original boards, certificates, full logs and packed proof deltas. [Artifact manifest](artifacts.json) · [Version and status record](status.json)

## Reproduce on WSL or Linux

Download the Proof-Bank archive from the research release. The restore tool checks the size and SHA-256 before extraction to a new directory. The embedded Git bundle supplies the historical solver revision.

~~~bash
python3 research/restore_experiment.py ~/Downloads/match3-proof-bank-v11-wsl.zip ./proof-bank-evidence
cd ./proof-bank-evidence
sha256sum -c SHA256SUMS
git clone ./solver/match3-strict.bundle ./solver-checkout
bash match3_proofbank/reproduce.sh ./solver-checkout ./proof-bank-replay --smoke
~~~

Omit `--smoke` for the full benchmark and calibration. The smoke path performs a Release build, CTest, a small paired simulation and independent archive replay. The full research runs use WSL/Linux timing and process support. The public solver builds on Linux and Windows.

The next delivery work covers a versioned service loader and rollback, production load measurement and playtesting with real players. [Integration status and acceptance criteria](productization.md)
