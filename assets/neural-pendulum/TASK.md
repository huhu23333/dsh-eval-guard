# Neural-Pendulum：从零神经网络基础设施与双驱动三阶摆控制

## 任务目标

从零实现一套纯 C++17、仅 CPU、fp32 的神经网络基础设施，并用它训练一个神经网络控制器，控制预编译的双驱动三节摆。

目标：

- 从三杆全部向下的固定初始状态起摆
- 保持三根杆全部向上至少 t_tgt = 10.0 秒
- 同时保证神经网络基础设施全部通过公开测试

允许使用 PyTorch 做开发阶段的自查和数值对照。最终提交源码中不得出现 torch，评测环境不提供 Python/torch。

## 环境

工作区提供预编译仿真器：

~~~
tools/sim
~~~

不提供仿真器源码和物理参数。

### 初始状态

训练时每次 reset：

- 三根杆接近全向下
- 角度以全向下为中心，只加小范围随机噪声
- 不添加固定角度偏差
- 带小范围随机初始角速度

评测时：

- 使用固定初始状态
- 三个固定角度互不相同
- 三个固定角度的绝对值也互不相同
- 初始角速度为 0
- 不加任何随机扰动

### 驱动结构

~~~text
joint 1：驱动
joint 2：被动
joint 3：驱动
~~~

第一关节和第三关节各有一个归一化力矩输入。

### 观测

控制器每一步只获得三个关节角度：

~~~text
a_i = wrap_to_pi(theta_i - pi)     i = 1,2,3
obs = [a1, a2, a3]  (rad)
~~~

即 3 维观测，单位 rad，不除以 pi。

- theta_i 是第 i 根杆的绝对角度
- a_i 是相对竖直向上的角度误差
- 不提供角速度
- 不提供第三杆末端位置
- 不提供杆长、质量、惯量等物理参数

控制器的内部结构和训练方法由你自行设计。

### 动作

控制器输出两个归一化力矩：

~~~text
[u1, u3] ∈ [-1, 1]^2
~~~

分别作用于第一关节和第三关节。第二关节始终无驱动。

非有限值、超范围或格式错误会判为控制器失败。

### 控制与仿真

~~~
控制频率 = 100 Hz
控制周期 = 0.01 s
最大仿真时间 = 30.0 s
t_tgt = 10.0 s
全向上容差 = 每根杆与竖直方向夹角 <= 0.05 rad
~~~

评测会计算：

~~~
t_can = 三根杆同时处于全向上容差内的最长连续时间
~~~

当：

~~~
t_can >= t_tgt
且基础设施测试全部通过
~~~

任务完成，计时停止。

## 训练用仿真接口

tools/sim 使用 JSONL 协议。

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

此时不返回新观测，也不会继续推进仿真。  
必须 reset 后才能开始新 episode；reset 前的 step 都返回该错误。

可以反复 reset / step 进行数据采集、模型训练和自测。

环境只返回状态，不提供 reward、loss、cost 或 done 信号。  
训练信号由你自行构造。

## 交付内容

只提交源码，不提交编译产物。

源码根目录必须支持：

~~~
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
~~~

必须产出两个入口：

~~~
build/nn_selftest
build/control
~~~

### 1. 基础设施入口

build/nn_selftest 必须运行公开基础设施测试并返回 0 表示通过。

必须包含的基础设施：

- fp32 tensor，形状检查与广播
- elementwise add/sub/mul/div
- matmul / linear
- relu / softmax
- layernorm
- conv2d，支持 stride/padding/dilation/groups
- MSE
- matmul/linear/relu/layernorm/conv2d/MSE 反向
- SGD momentum
- AdamW
- 模型保存/加载
- 线程池

占位符或空实现会被判为基础设施错误。

公开测试文件在工作区中，评测时使用评测侧的干净副本。

### 2. 控制器入口

build/control 是一个常驻进程，通过 stdin/stdout 使用 JSONL 通信。

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

- 每次评测启动一个新进程
- 先收到 reset，必须回复 ready
- 每个 step 必须在 100 ms 内回复
- 控制器可以保存内部历史状态
- 崩溃、超时、非法输出会判为控制器失败

## 提交与测试

使用宿主工具：

~~~text
eval_submit
eval_test
~~~

### eval_submit

把当前工作区源码快照提交到宿主侧不可读目录。每次提交独立保留，不覆盖旧提交。提交本身不改变分数。

### eval_test

由宿主对最新提交快照进行构建和隐藏仿真评估，返回：

~~~text
stage
t_can
completed
error
~~~

只有 eval_test 会更新分数。第一次 eval_test 之前必须先 eval_submit，否则 stage = no_submission。

阶段枚举：

~~~text
no_submission
compile
infra
controller
simulation
scoring
completed
busy
~~~

最终排行分数不会返回给你，只用于赛后排名。

## 完成条件

当测试返回：

~~~
stage = completed
t_can >= 10.0
~~~

即任务完成。

完成后：

- 立即停止修改
- 立即总结你的实现
- 之后的提交和测试请求都会被拒绝

## 约束

- 只允许 C++17 + CMake + C++ 标准库
- 仅 CPU，fp32
- 提交源码中不得出现 torch
- 评测环境无 Python/torch
- 无网络
- 不得修改仿真器或公开测试
- 不得提交编译产物
- 赛后人工审核：控制器必须是神经网络，且必须实际调用你实现的神经网络基础设施；手写控制律、查表、固定轨迹或外部依赖会被判无效

## 优化目标

从接近全向下的初始状态起摆，使三根杆全部进入竖直向上容差，并尽可能长时间保持。

这是一个控制目标，不是固定损失函数。环境不提供 reward/loss；你需要自己决定训练信号、损失、奖励或优化方式。

## 评分规则

任务从会话开始时刻计时。

### 测试请求与分数

提交源码本身不改变分数。

只有申请测试会更新当前分数：

~~~
无效测试 / 编译失败 / 基础设施失败 / 控制器失败 / 仿真失败 / 无提交：
score(t) = -1

有效测试：
score(t) = (min(t_can, t_tgt) - t_tgt) / t_tgt
~~~

其中：

~~~
t_tgt = 10.0 s
score(t) 的范围是 [-1, 0]
t_can = 0 时 score = -1
t_can >= t_tgt 时 score = 0
~~~

分数从测试请求时刻开始生效，直到下一次测试请求。

第一次测试请求之前，score = -1。

### 最终分数

最终分数是时间积分，不除以总时长：

~~~
final = integral_0^T_done score(t) dt
~~~

T_done 是第一次返回 completed 的测试请求时间。

因此：

- 越早达到完成条件，负分区间越短，最终分数越高
- 拖得越久，负分区间越长
- 完成后停止积分
- 未完成不参与最终排名

### 完成时

当测试返回 completed 时：

- 当前 score 固定为 0
- 时间积分立即停止
- 之后的提交和测试全部拒绝
- 不会再有新的分数变化

### 测试结果与最终分数

测试结果会返回：

- stage
- t_can
- completed
- error

最终排行分数不会返回给你，只用于赛后排名。

## 建议工作顺序

1. 阅读公开测试和 tools/sim 协议
2. 先实现最小可用的 tensor/算子/反向/优化器
3. 跑通 nn_selftest
4. 用 tools/sim 采集数据并训练控制器
5. 反复提交和测试，优先把 t_can 提升到 10 秒
6. 完成后立即总结
