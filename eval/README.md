# 单工作区评测脚本

设置页的「评测当前工作区」会调用这些脚本；每个脚本只接收一个工作区，输出单条结果 JSON，不生成排行。

| 任务 | 脚本 | 评分 |
|---|---|---|
| tank | eval/tank.py | 官方引擎 + BT，对基线（quick 20 局） |
| warehouse | eval/warehouse.py | 正确性 + 相对基线加速比（quick 仅 pub_small） |
| land | eval/land.py | 官方 round_robin/rate（quick 仅单挑 2 图） |
| bomberman | eval/bomberman.py | 官方 tournament/run（quick 仅 2 人 1 种子） |
| ctf | eval/ctf.py | 官方 arena plan/run/rate（quick 仅 2/3 方） |
| commit0 | eval/commit0.py | 单元测试通过率（测试来自安装快照） |
| swe-bench-pro | eval/swe-bench-pro.py | 隐藏 F2P/P2P 节点全部通过才 resolved |
| featurebench | eval/featurebench.py | 隐藏 feature 测试 + 全部回归文件通过才 resolved |
| neural-cpp | eval/neural-cpp.py | 隐藏 C++ 测试；accuracy / matrixAccuracy / speedGeomean / score |
| neural-fp32 | eval/neural-fp32.py | 逐操作 MRE/阈值/速度；无统一 score |
| neural-cifar | eval/neural-cifar.py | FP32 算子指标 + 隐藏验证集准确率/FPS（独立目录） |
| neural-pendulum | eval/neural-pendulum.py | FP32 算子指标 + 固定初态控制 t_can（独立目录） |

eval/legacy/ 保存从独立评测目录迁移过来的评测器；eval/arena/ 保存官方引擎、裁判和历史对手。common.py 负责临时工作区、结果解析和结果文件写入。

## 完整性约束（宿主注入）

- `EVAL_BASELINE_DIR`：安装成功时快照的测试/配置目录。commit0/SWE/FeatureBench 的 wrapper 会在评测前用它覆盖候选副本，并删除候选新增的 `conftest.py`；同时校验 `.venv` 指纹（`venv.json`）。
- `EVAL_RESULT_FILE` / `EVAL_JOB_DIR`：只由顶层 evaluator 使用；所有 evaluator 再启动子进程时会剥掉这些变量，候选二进制拿不到结果文件路径。
- 隐藏资产（neural-cpp 的隐藏测试、CIFAR 的验证标签）在候选构建/推理开始前读入内存；CIFAR 的图像副本会放到独立临时目录，标签不落盘到候选可见路径。

后台任务由 host 的 JobManager 执行，日志和结果保存在 $DSH_HOME/eval-guard/jobs/<job-id>/。Job 子进程的环境变量已收敛为白名单（PATH/HOME/LANG/LC_*/TMPDIR/SSL_* 等）。
