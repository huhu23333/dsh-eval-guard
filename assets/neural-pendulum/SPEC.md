# Neural-Pendulum 任务规格 v1.5

本文件是已确定的任务内部规格。实现、评测器、workspace 素材以此为准。
后续除非明确修改规格，否则不再重新讨论这些规则。

v1.5 变更：Tier A 控制任务。每回合随机化隐藏物理参数、初始状态与观测噪声；评测改为 100 个隐藏随机回合上的成功率，成功率 >= 0.8 完成。基础设施评测协议不变。

## 1. 任务标识

- task id: neural-pendulum
- 中文名: Neural-Pendulum · 从零神经网络基础设施与双驱动三阶摆控制
- 形态: 单个 agent 在独立工作区内完成源码实现并多次提交/测试
- 语言: C++17、仅 CPU、fp32
- 最终交付: 源码（不提交编译产物）

## 2. 任务目标

agent 必须：

1. 从零实现一套神经网络基础设施（张量、算子、反向传播、优化器、线程池；控制器权重的打包方式自定，评测不要求特定序列化格式）。
2. 使用该基础设施训练一个神经网络控制器。
3. 控制预编译双驱动三节摆：第一、第三关节施加有限力矩，中间关节被动，从全向下起摆，并保持三根杆全部向上至少 t_tgt = 10.0 s；评测在 100 个随机隐藏回合上统计成功率，成功率 >= 0.8 完成。
4. 保证基础设施通过 `build/nn_task` 固定 CLI 的数值评测：评测侧用 PyTorch 在相同数据上计算参考值，逐操作比较加权平均相对误差并对照阈值。
5. 提交的源码中不得出现 torch；候选运行时不提供 Python/torch。

评测侧用 PyTorch 计算参考值；工作区/候选运行时不提供 PyTorch 或 numpy（也无网络），开发阶段请用 C++ 或独立 double 标量参考做自查。提交源码必须完全独立。

## 3. 被控对象

三节平面欠驱动摆：

- 3 根均匀杆
- 第一关节、第三关节受控；第二关节被动
- 控制量: 两个归一化力矩 [u1, u3]，每个 in [-1, 1]
- 物理力矩: 关节 1 电机力矩 tau_1 = u1 * tau_max；关节 3 电机力矩 tau_3 = u3 * tau_max（关节 3 是杆 2/杆 3 之间的内部电机，杆 2 受到 -tau_3 反作用）；关节 2 电机力矩为 0。在绝对角度坐标下广义驱动力向量为 (tau_1, -tau_3, tau_3)。
- 控制频率: 100 Hz，控制周期 dt = 0.01 s
- 每个控制周期内部用 4 个 RK4 子步
- 最大仿真时长: 30.0 s

每回合隐藏随机物理参数（不向 agent 暴露具体值）：

~~~
m_i ~ Uniform(0.7, 1.3) kg
l_i ~ Uniform(0.45, 0.55) m
r_i = l_i / 2
I_i = m_i * l_i^2 / 12
g = 9.81 m/s^2
joint damping ~ Uniform(0.005, 0.02)
tau_max ~ Uniform(15.0, 21.0) N m（每个驱动关节独立限制）
~~~

角度约定：

- theta_i = 0 表示第 i 根杆竖直向下
- theta_i = pi 表示竖直向上
- 所有角度为绝对角度

末端位置：

~~~
x_tip = sum_i l_i * sin(theta_i)
y_tip = -sum_i l_i * cos(theta_i)
~~~

## 4. 初始状态与随机化

训练和评测的每个回合都重新采样隐藏物理参数、初始状态和观测噪声：

~~~
theta_i ~ Uniform(-0.15, 0.15) rad   i = 1,2,3，独立采样
omega_i ~ Uniform(-0.2, 0.2) rad/s   i = 1,2,3，独立采样
~~~

单回合内部参数不再改变；每次 reset 开始新回合。评测初始状态与训练同分布，但评测种子和回合序列不暴露给 agent。

## 5. Agent 观测

控制器每一步只获得三个关节角度：

~~~
a_i = wrap_to_pi(theta_i - pi)     i = 1,2,3
obs = [a1, a2, a3]  (rad)
~~~

即 3 维浮点向量，单位 rad，不除以 pi。

- theta_i 为绝对角度
- a_i 为相对竖直向上的角度误差
- 不提供角速度
- 每一步返回的三个角度叠加随机观测噪声；噪声标准差每回合独立采样自 Uniform(0.002, 0.008) rad
- 不提供第三杆末端位置
- 不提供杆长/质量/惯量/真实物理参数

控制器必须自行从角度历史构造任何需要的速度、差分或其他内部状态；不强制任何模型结构。

## 6. 动作

控制器输出两个归一化力矩：

~~~
[u1, u3] ∈ [-1, 1]^2
~~~

映射：

~~~
tau_1 = u1 * tau_max
tau_3 = u3 * tau_max
tau_2(joint motor) = 0
absolute generalized force = (tau_1, -tau_3, tau_3)
~~~

非有限值、超范围或格式错误：

~~~
stage = controller
score = -1
~~~

## 7. 训练仿真接口

工作区提供预编译 tools/sim，不提供源码和内部参数。
`tools/sim` 是**训练/自测专用**：每次 reset 随机化隐藏物理参数、初始状态与观测噪声，不包含评测种子或固定评测初态。
评测使用独立程序 sim_eval；它每次 reset 采样新的隐藏物理参数、初始状态和观测噪声，并在 100 个隐藏随机回合上统计成功率。工作区不包含 sim_eval，也不包含评测种子或回合序列。
环境只提供状态转移，不提供 reward、loss、cost 或 done 信号。训练信号由 agent 自行构造。

优化目标：

~~~
从接近全向下的初始状态起摆
使三根杆全部进入竖直向上容差并尽可能长时间保持
评测指标为 100 个隐藏随机回合上的成功率（单回合 t_can >= 10 s 记成功）
~~~

不规定损失函数、奖励函数、优化器或训练算法。

JSONL 协议：

输入：

~~~json
{"cmd":"reset"}
{"cmd":"step","torque":[0.5,-0.5]}
~~~

输出：

~~~json
{"obs":[3 floats]}
~~~

每个 episode 最多 3000 步（30.0 s）。

达到最大步数后再发送 step：

~~~json
{"error":"episode_finished"}
~~~

此时不会返回新的观测，也不会继续推进仿真。
必须发送 reset 才能开始新 episode；reset 之前的所有 step 都返回该错误。

## 8. 交付源码与构建

只提交源码，不提交可执行文件。

源码根目录必须支持：

~~~
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
~~~

必须产生：

~~~
build/nn_task
build/control
~~~

### 8.1 基础设施数值评测入口

神经网络基础设施通过 `build/nn_task` 固定 CLI 暴露（协议见 TASK.md）。评测侧在独立评测进程中：

1. 用固定种子生成输入与参数
2. 用 PyTorch 在相同数据上计算参考输出
3. 调用候选 `build/nn_task` 子命令，读取其写出的 float32 结果
4. 逐操作计算加权平均相对误差

~~~
mre = sum(|candidate - reference|) / sum(|reference|)
~~~

覆盖操作与阈值：

| 操作 | 阈值 |
| --- | --- |
| elementwise add/sub/mul/div、relu、mse、sgd-momentum | 1e-6 |
| matmul、linear、softmax、layernorm、adamw | 1e-5 |
| conv2d、matmul/linear/relu/layernorm/mse/conv2d backward | 1e-4 |

每个操作使用 3 组独立数据。以下任一情况：

~~~
stage = infra
score = -1
~~~

- 子命令不可执行、崩溃或超时（单次调用 60 秒）
- 输出文件缺失或元素数不符
- 加权平均相对误差超过阈值

速度（GFLOP/s）同时统计并报告，但不参与通过判定。参考数据、阈值和评测过程均不在工作区中。

### 8.2 控制器入口

build/control 是一个常驻进程，使用 JSONL 协议：

输入：

~~~json
{"type":"reset"}
{"type":"step","step":0,"state":[3 floats]}
~~~

输出：

~~~json
{"type":"ready"}
{"type":"action","torque":[0.5,-0.5]}
~~~

规则：

- 每次测试启动一个新进程
- 先 reset，必须回复 ready
- 每个 step 必须在 100 ms 内回复
- 控制器可保存自己的历史状态
- 控制器崩溃、超时、非法输出：stage = controller，score = -1

## 9. 评测测量

固定：

~~~
dt = 0.01 s
最大仿真 = 30.0 s
t_tgt = 10.0 s
全向上容差 = 每根杆与竖直方向夹角 <= 0.05 rad
评测回合数 = 100
完成成功率阈值 = 0.8
~~~

单回合 t_can 定义：

~~~
从 reset 开始，三根杆全部处于容差内的最长连续时间
从未出现全向上则 t_can = 0
单回合成功 = (t_can >= t_tgt)
~~~

100 回合统计：

~~~
success_rate = 成功回合数 / 100
t_can = 100 回合 t_can 的平均值（仅用于报告）
~~~

完成条件：

~~~
infra_ok == true（全部算子可执行且误差不超阈值）
且 success_rate >= 0.8
~~~

## 10. 提交与测试事件

### 提交事件

~~~json
{
  "event": "submit",
  "time": 123.4,
  "submission_id": "s003",
  "source_hash": "sha256:..."
}
~~~

提交事件不包含分数和测试结果。

### 测试请求

~~~json
{
  "event": "test",
  "time": 135.0,
  "submission_id": "s003"
}
~~~

- 测试使用请求时刻的最新提交快照
- 测试期间的新提交不影响本次测试

### 测试阶段枚举

~~~
no_submission   没有提交快照
compile         CMake 构建失败，或未产出 build/nn_task / build/control
infra           build/nn_task 算子数值评测失败（不可执行/崩溃/超时/输出缺失/误差超阈值）
controller      控制器启动/协议/崩溃/超时/非法输出
simulation      仿真异常/NaN/仿真超时
scoring         正常测试但 success_rate < 0.8
completed       success_rate >= 0.8，任务完成
~~~

### 返回给 agent 的测试结果

~~~json
{
  "ok": true,
  "stage": "scoring",
  "t_can": 8.1,
  "success_rate": 0.62,
  "episodes": 100,
  "completed": false,
  "error": ""
}
~~~

返回 stage、t_can、success_rate、episodes、completed，不返回最终分数。

## 11. 计分

从任务开始时刻计时。

### 11.1 单时刻分数

无效/失败/无提交：

~~~
score = -1
~~~

有效测试：

~~~
score = (min(success_rate, 0.8) - 0.8) / 0.8
~~~

范围：

~~~
[-1, 0]
~~~

### 11.2 分数随时间变化

- 提交事件不改变 score
- 只有测试请求改变 score
- 测试失败：从该测试请求时刻起 score = -1
- 测试成功：从该测试请求时刻起按 success_rate 计分
- 第一次测试请求之前：score = -1
- 下一次测试请求之前，score 保持不变

### 11.3 最终分数

不除以总时间：

~~~
final = integral_0^T_done score(t) dt
~~~

T_done 是第一次返回 stage = completed 的测试请求时间。

未完成任务不参与最终排名；手动停止由外部处理。

## 12. 完成与终止

测试返回：

~~~
stage = completed
success_rate >= 0.8
infra_ok = true
~~~

则：

- T_done = 该测试请求时间
- 任务立即结束
- 测试结果提示：

~~~
任务完成，请立即停止修改并总结实现。
~~~

- 之后的提交和测试请求全部拒绝
- 不产生新的提交/测试事件
- 最终分数固定在 T_done 前

## 13. 异常处理

以下情况均返回 score = -1 并进入对应阶段：

- 无提交
- 源码缺失
- 编译失败/超时
- 算子数值评测失败
- 控制器加载/接口/崩溃/超时/非法输出
- 仿真 NaN/异常/超时
- 测试请求本身失败

异常只影响当前测试请求之后的区间，直到下一次成功测试。

## 14. 事件记录

插件记录所有提交和测试事件，至少包含：

~~~
event
time
submission_id
source_hash
stage
ok
infra_ok
t_can
success_rate
episodes
score
completed
~~~

提交事件不含 score/result。测试事件含 score 和结果。

插件不画图；曲线由外部根据事件记录绘制。

## 15. 编译与依赖

evaluator 在干净目录构建源码：

~~~
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
~~~

规则：

- 候选仅允许 C++17 + CMake + C++ 标准库
- 候选无网络；评测侧不向候选提供 Python/torch
- 候选源码中出现 torch/Python 依赖：算子评测失败或赛后人工审核无效
- 评测侧使用 PyTorch（CPU、确定性、单线程）计算数值参考值
- 编译失败：stage = compile，score = -1

## 16. 赛后人工审核

插件只记录和计分。赛后人工审核：

- 控制器是否为神经网络
- 是否实际调用提交的神经网络基础设施
- 是否存在手写控制律、查表、固定轨迹、外部依赖
- 源码中是否出现 torch/Python

审核不通过：成绩无效。

## 17. 排名接口

插件只负责产生可比较的最终积分和事件记录。

最终积分不除以总时间；未完成任务不参与排名。

排名、人工审核和提交有效性由外部处理。
