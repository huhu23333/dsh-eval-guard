# Neural-Pendulum：从零神经网络基础设施与双驱动三阶摆控制

## 任务目标

从零实现一套纯 C++17、仅 CPU、fp32 的神经网络基础设施，并用它训练一个神经网络控制器，控制预编译的双驱动三节摆。

目标：

- 训练与评测的每个回合都随机化物理参数、初始状态与观测噪声
- 单回合保持三根杆全部向上至少 t_tgt = 10.0 秒
- 评测在 100 个隐藏随机回合上统计成功率
- 同时保证神经网络基础设施通过 build/nn_task 固定 CLI 的数值评测

评测侧会用 PyTorch 计算算子参考值，但**工作区/候选运行时不提供 PyTorch 或 numpy**，也没有网络，无法 pip 安装。开发阶段请用 C++，或 Python 标准库/独立 double 实现做数值对照；最终提交的基础设施和控制器源码必须是纯 C++，不得出现 torch。

## 训练路线经验（强烈建议）

以下是根据已验证可行基线总结的经验，不改变评测规则，但能显著降低走弯路和结果波动；训练方法、奖励和实现仍由你自行完成：

- **显式速度特征**：观测必须包含三个关节的角速度估计。用相邻帧差分或短窗最小二乘从角度历史估计速度；只输入当前角度不够。
- **观测历史不能太短**：至少约 32 帧（100 Hz 下约 0.32 s），建议 32–64 帧。8/16 帧无法稳定判断摆动相位、速度趋势和接近顶部的动量。
- **策略网络不能太简单**：不能使用线性层、单层网络或极窄网络；至少 2 个隐藏层，每层至少约 256 个单元，输出层用 tanh 限幅。
- **训练时间按小时计**：需要远大于几分钟、几十次迭代的训练预算；按小时和数千万环境步规划，并使用并行/批量仿真提高吞吐。
- **不要复杂手工先验特征**：输入只保留原始角度（可用 sin/cos）、差分/最小二乘速度、历史窗口和上一时刻动作；不要手工加入能量、势能、LQR/控制律反馈项，让网络自己学。
- **每个控制步都更新动作**：控制频率 100 Hz，每个 0.01 s 步都重新计算动作；不要每 2 步或每 N 步保持，也不要用低通/延迟来“平滑”输出。
- **在完整随机化分布上训练**：训练必须覆盖评测所用的质量、杆长、阻尼、力矩上限、初态和观测噪声随机化；不要只在 nominal 参数或辨识模型上训练，也不要用辨识模型代替 tools/sim 做最终训练。
- **以 model-free 策略优化为主线**：优先使用 model-free 的策略优化/actor-critic 路线；系统辨识、LQR/MPC、解析控制和手工控制器只作辅助或对照，不要占用主要实验预算。
- **密集奖励**：奖励应覆盖起摆/能量注入、接近顶部的低速控制、进入容差后的保持时间；不要只使用最终成功/失败的稀疏奖励。
- **用隐藏评测选模型**：用隐藏随机 100 回合的 success_rate 和 t_can 选择最终 checkpoint；不要用训练 reward 或单次 rollout 判断。

以上是经验下限，不是固定配方；你可以做得更大、更稳，但不要低于这些要求。

## 环境

工作区提供预编译仿真器：

~~~
tools/sim
~~~

不提供仿真器源码和物理参数。

工具说明：工作区的 `tools/sim` 是训练/自测专用仿真器，每次 `reset` 都会随机化物理参数、初始状态和观测噪声；它不是评测器，也没有隐藏评测种子。评测使用工作区之外的独立仿真程序，在 100 个隐藏随机回合上统计成功率。

### 初始状态与随机化

训练和评测的每个回合：

- 三根杆接近全向下：theta_i ~ Uniform(-0.15, 0.15) rad
- 角速度：omega_i ~ Uniform(-0.2, 0.2) rad/s
- 物理参数（质量、长度、关节阻尼、力矩上限）每回合独立随机采样
- 观测噪声强度每回合独立随机采样

单回合内部参数不再改变；每次 reset 开始一个新回合。

### 驱动结构

~~~text
joint 1：驱动
joint 2：被动
joint 3：驱动
~~~

第一关节和第三关节各有一个归一化力矩输入。第三关节是杆 2/杆 3 之间的内部电机：它的力矩同时作用在杆 3 和杆 2 上（反作用），不是只作用在杆 3 上的外部力矩；关节 2 没有主动电机力矩。

### 观测

控制器每一步只获得三个关节角度：

~~~text
a_i = wrap_to_pi(theta_i - pi)     i = 1,2,3
obs = [a1, a2, a3]  (rad)
~~~

即 3 维观测，单位 rad，不除以 pi。

- theta_i 是第 i 根杆的绝对角度
- theta_i = 0 表示竖直向下，theta_i = pi 表示竖直向上
- a_i 是相对竖直向上的角度误差
- 不提供角速度
- 每一步返回的三个角度都叠加随机观测噪声，噪声强度每回合随机
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

控制频率 = 100 Hz
控制周期 = 0.01 s
最大仿真时间 = 30.0 s
t_tgt = 10.0 s
全向上容差 = 每根杆与竖直方向夹角 <= 0.05 rad

每个评测回合计算：

t_can = 三根杆同时处于全向上容差内的最长连续时间
单回合成功 = (t_can >= t_tgt)

评测使用 100 个隐藏随机回合：

success_rate = 成功回合数 / 100

当：

success_rate >= 0.9
且神经网络基础设施数值评测全部通过

任务完成，计时停止。

该任务已由内部基线验证：100 回合成功率 >= 0.90 是可达的，不要怀疑可行性。在 eval_test 返回 stage=completed 之前，绝对不允许停止、总结或放弃；不要因为成功率接近 0.90、遇到平台期或短期没有提升就收尾。只要没有完成，就必须继续训练、改进、提交和测试，直到完成或外部强制停止。

最终提交的 build/control 必须由神经网络输出层直接产生两个力矩；最终控制程序中不得包含任何显式控制律（PID/LQR/MPC/滑模/自抗扰等），也不得在神经网络之外根据状态计算或修正力矩。

不得对已编译文件进行任何形式的逆向、反汇编、strings/objdump/readelf/gdb 分析，也不得读取 /proc 或其它进程内存。环境信息只能来自本文件/SPEC.md 和正常使用 tools/sim 的 reset/step 输入输出；违规者成绩无效。

后台任务管理：长时间实验必须使用宿主管理的 run_in_background，禁止 nohup、setsid、disown 或尾随 & 脱管。启动任何长任务前先 job_list；同时最多保留 2 个后台任务；同一实验不要重复启动；读完日志且不再需要后立即 job_kill 或用受控方式清理。每约 5 分钟，下一次工具结果会附带当前 running 后台任务和本工作区 OS 进程提醒；看到提醒必须立即处理，不要等用户提醒。开始新方向、提交或最终回答前，必须清理所有不用的 job、训练进程和 tools/sim，确认没有残留。

如果当前方向失败，必须记录失败原因、清理无用后台任务，并换一个实质性不同的方向继续；不要重复同一配置，也不要因为一次失败就断定任务不可做。只有 stage=completed 或外部手动停止才是结束条件。


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

可以反复 reset / step 进行数据采集、模型训练和自测。该程序只做随机训练 reset，不包含隐藏评测种子；评测器是独立程序。

环境只返回状态，不提供 reward、loss、cost 或 done 信号。  
训练信号由你自行构造。

## 交付内容

提交源码、必要权重和可选的编译产物。评测会从干净副本重新构建源码，不依赖你提交的二进制或构建目录；编译产物可以保留，但不保证被使用。

源码根目录必须支持：

~~~
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
~~~

必须产出：

~~~
build/nn_task
build/control
~~~

工作区模板 CMakeLists.txt 默认从 `src/core/*.cpp` 构建共享静态库，从 `src/nn_task.cpp` 和 `src/control.cpp` 构建两个可执行文件。可以按需调整目录结构和 CMakeLists.txt，但必须从零构建出上面两个二进制。

### 1. 神经网络基础设施入口 build/nn_task

基础设施通过固定 CLI 暴露，评测会逐操作调用。所有输入/输出文件都是连续 row-major float32，所有数值输出也必须写为 float32。

#### 逐元素

~~~bash
build/nn_task elementwise --op add|sub|mul|div --shape 64,256 --a A --b B --out O
~~~

支持广播：从尾部对齐，维度为 1 可广播。

#### 矩阵乘法

~~~bash
build/nn_task matmul --a-shape M,K --b-shape K,N --a A --b B --out O
~~~

只要求 2D 输入。

#### Linear

~~~bash
build/nn_task linear --x-shape B,IN --w-shape OUT,IN --b-shape OUT --x X --w W --bias B --out O
~~~

计算 `O = X @ W^T + bias`。

#### 激活

~~~bash
build/nn_task relu --shape N --x X --out O
build/nn_task softmax --shape B,C --x X --out O
~~~

Softmax 沿最后一维。

#### LayerNorm

~~~bash
build/nn_task layernorm --shape B,C --x X --gamma G --beta B --eps 1e-5 --out O
~~~

对最后一维归一化；gamma/beta 形状为 `C`。

#### MSE

~~~bash
build/nn_task mse --pred-shape S --target-shape S --pred P --target T --out O
~~~

输出 1 个 float32，为所有元素平均平方误差。

#### Conv2d

~~~bash
build/nn_task conv2d --x-shape N,C,H,W --w-shape O,Cg,KH,KW --b-shape O --x X --w W --bias B --stride SH,SW --padding PH,PW --dilation DH,DW --groups G --out OUT
~~~

标准 NCHW 卷积，`Cg = C / groups`。

#### 反向传播

~~~bash
build/nn_task matmul-backward --a-shape M,K --b-shape K,N --a A --b B --grad G --da DA --db DB
build/nn_task linear-backward --x-shape B,IN --w-shape OUT,IN --b-shape OUT --x X --w W --grad G --dx DX --dw DW --db DB
build/nn_task relu-backward --shape N --x X --grad G --dx DX
build/nn_task layernorm-backward --shape B,C --x X --gamma G --grad G --eps 1e-5 --dx DX --dgamma DG
build/nn_task mse-backward --pred-shape S --target-shape S --pred P --target T --grad G --dp DP --dtarget DT
build/nn_task conv2d-backward --x-shape N,C,H,W --w-shape O,Cg,KH,KW --b-shape O --x X --w W --grad G --stride SH,SW --padding PH,PW --dilation DH,DW --groups G --dx DX --dw DW --db DB
~~~

反向传播定义为：上游梯度 `G` 对输入、权重、bias 的梯度；`G` 与 forward 输出同形状。

#### 优化器

~~~bash
build/nn_task adamw --param P --grad G --m M --v V --lr LR --beta1 B1 --beta2 B2 --eps EPS --weight-decay WD --step T --out-param P2 --out-m M2 --out-v V2
build/nn_task sgd-momentum --param P --grad G --momentum-buffer MB --lr LR --momentum MU --out-param P2 --out-buffer MB2
~~~

AdamW 使用 decoupled weight decay：

~~~text
m_t = beta1*m + (1-beta1)*g
v_t = beta2*v + (1-beta2)*g*g
m_hat = m_t / (1-beta1^t)
v_hat = v_t / (1-beta2^t)
param = param*(1-lr*wd) - lr*m_hat/(sqrt(v_hat)+eps)
~~~

SGD momentum：

~~~text
buffer = momentum*buffer + grad
param = param - lr*buffer
~~~

#### 数值评测方式

评测侧在独立进程中生成数据，并用 PyTorch 在相同输入/参数上计算参考值，逐操作比较。工作区不会收到期望输出张量。

- 每个操作使用 3 组独立随机数据（固定种子），形状固定
- 误差指标为加权平均相对误差：

~~~
mre = sum(|candidate - reference|) / sum(|reference|)
~~~

- 每个操作有独立阈值，单位与 fp32 精度匹配：

| 操作 | 加权平均相对误差阈值 |
| --- | --- |
| elementwise.add / sub / mul / div | 1e-6 |
| relu | 1e-6 |
| mse | 1e-6 |
| sgd-momentum | 1e-6 |
| matmul / linear | 1e-5 |
| softmax / layernorm | 1e-5 |
| adamw | 1e-5 |
| conv2d | 1e-4 |
| matmul-backward / linear-backward / relu-backward / layernorm-backward / mse-backward / conv2d-backward | 1e-4 |

- 任一操作不可执行、崩溃、超时（单次调用 60 秒）、输出文件缺失或元素数不符，或 mre 超过阈值，都会判为基础设施失败（stage = infra）
- 速度（GFLOP/s）会同时报告，但速度不参与通过判定
- 不做完整训练，不要求模型保存/加载格式；控制器需要的权重可以放在源码目录或控制器自行打包

评测数据在测试进程内生成，不随工作区提供。不要伪造输出：评测直接执行你的二进制并检查它写出的文件。

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

由宿主对最新提交快照进行构建、算子数值评测和隐藏仿真评估，返回：

~~~text
stage
t_can
success_rate
episodes
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
~~~

阶段含义：

- no_submission：还没有提交快照
- compile：CMake 构建失败，或没有产出 build/nn_task / build/control
- infra：build/nn_task 算子数值评测失败（不可执行、崩溃、超时、输出缺失或误差超阈值）
- controller：控制器启动、协议、崩溃、超时或输出非法
- simulation：隐藏仿真推进异常、NaN 或超时
- scoring：测试有效，但 `success_rate < 0.9`
- completed：基础设施和仿真均通过，且 `success_rate >= 0.9`

最终排行分数不会返回给你，只用于赛后排名。

## 完成条件

当测试返回：

~~~
stage = completed
success_rate >= 0.9
~~~

即任务完成。

完成后：

- 立即停止修改
- 立即总结你的实现
- 之后的提交和测试请求都会被拒绝

## 约束

- 只允许 C++17 + CMake + C++ 标准库
- 仅 CPU，fp32
- 提交源码中不得出现 torch；候选运行时不提供 Python/torch（评测侧会用 PyTorch 计算数值参考值）
- 无网络
- 不得修改或替换 tools/sim；它只用于训练和自测
- 允许提交编译产物；评测仍以源码重建结果为准
- 赛后人工审核：控制器必须是神经网络，且必须实际调用你实现的神经网络基础设施；手写控制律、查表、固定轨迹或外部依赖会被判无效

## 优化目标

从接近全向下的初始状态起摆，使三根杆全部进入竖直向上容差，并尽可能长时间保持。

这是一个控制目标，不是固定损失函数。环境不提供 reward/loss；你需要自己决定训练信号、损失、奖励或优化方式。

## 评分规则

任务从会话开始时刻计时。

### 测试请求与分数

提交源码本身不改变分数。

只有申请测试会更新当前分数：

无效测试 / 编译失败 / 基础设施失败 / 控制器失败 / 仿真失败 / 无提交：
score(t) = -1

有效测试：
score(t) = (min(success_rate, 0.9) - 0.9) / 0.9

其中：

success_rate 的范围是 [0, 1]
success_rate = 0 时 score = -1
success_rate >= 0.9 时 score = 0

分数从测试请求时刻开始生效，直到下一次测试请求。

第一次测试请求之前，score = -1。

### 最终分数

最终分数是时间积分，不除以总时长：

final = integral_0^T_done score(t) dt

T_done 是第一次返回 completed 的测试请求时间。

## 完成时

当测试返回 completed 时：

- 当前 score 固定为 0
- 时间积分立即停止
- 之后的提交和测试全部拒绝
- 不会再有新的分数变化

### 测试结果与最终分数

测试结果会返回：

- stage
- t_can（100 回合平均保持时间，仅用于报告）
- success_rate（100 个随机隐藏回合的成功率）
- episodes
- completed
- error

最终排行分数不会返回给你，只用于赛后排名。

## 建议工作顺序

1. 阅读 build/nn_task 的 CLI 协议与 tools/sim 协议
2. 先实现最小可用的 tensor/算子/反向/优化器/线程池，并让 build/nn_task 跑通
3. 用独立 double 标量参考、有限差分梯度检查逐算子核对误差，边界形状/广播/groups/stride/dilation 都要覆盖（工作区没有 PyTorch/numpy）
4. 用 tools/sim 采集数据并训练控制器，导出权重供 build/control 使用
5. 反复提交和测试，优先把 100 回合成功率提升到 90%
6. 完成后立即总结
