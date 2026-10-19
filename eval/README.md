# 单工作区评测脚本

设置页的「评测当前工作区」会调用这些脚本；每个脚本只接收一个工作区，输出单条结果 JSON，不生成排行。

| 任务 | 脚本 | 评分 |
|---|---|---|
| tank | eval/tank.py | 官方引擎 + BT，对基线 |
| warehouse | eval/warehouse.py | 正确性 + 相对基线加速比 |
| land | eval/land.py | 官方 round_robin/rate |
| bomberman | eval/bomberman.py | 官方 tournament/run |
| ctf | eval/ctf.py | 官方 arena plan/run/rate |
| commit0 | eval/commit0.py | 单元测试通过率 |
| swe-bench-pro | eval/swe-bench-pro.py | 隐藏 F2P/P2P 测试 |
| featurebench | eval/featurebench.py | 隐藏 F2P/P2P 测试；全部通过才 resolved |

eval/legacy/ 保存从独立评测目录迁移过来的评测器；eval/arena/ 保存官方引擎、裁判和历史对手。common.py 负责临时工作区、结果解析和结果文件写入。

后台任务由 host 的 JobManager 执行，日志和结果保存在 $DSH_HOME/eval-guard/jobs/<job-id>/。
