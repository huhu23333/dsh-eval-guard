# 各模型的封存代码

都是模型在比赛中写出的原样代码，主办方没有改动。`half1/` 是上半场版本（打中场赛、进步赛里的固定对手池），`half2/` 是下半场版本（打终场赛，决定最终名次）。基准在 `../workspace/baseline.cpp`，出题方参照在 `../tournament/sparring.cpp`。

文件里的 sha256 和 `../results/*/清单.json` 记录的一致。名次和实力值是终场赛的。

| 文件名 | 模型 | 短名 | 终场名次 | 实力值 | 上半场行数 | 下半场行数 | 下半场改了没有 |
|---|---|---|---:|---:|---:|---:|---|
| `gpt-6-astra.cpp` | GPT-6 Astra | Astra | 1 | 507 | 233 | 241 | 改了 |
| `gpt-6.1-sol-ultrafast.cpp` | GPT-6.1 Sol（Ultrafast） | SolUF | 2 | 467 | 350 | 356 | 改了 |
| `gemini-4-argon.cpp` | Gemini 4 Argon Contributor | Gemini | 3 | 355 | 1132 | 1390 | 改了 |
| `gpt-6.1-sol.cpp` | GPT-6.1 Sol | Sol | 4 | 319 | 78 | 87 | 改了 |
| `opus-5.5.cpp` | Claude Opus 5.5 | Opus | 5 | 259 | 435 | 476 | 改了 |
| `muse-wonder-full.cpp` | Muse Wonder Full | Muse | 6 | 55 | 560 | 560 | 没改（和上半场逐字节相同） |
| `mimo-v2.6-pro.cpp` | 小米 MiMo V2.6 Pro | MiMo | 7 | 29 | 336 | 336 | 没改（和上半场逐字节相同） |
| `grok-4.7.cpp` | Grok 4.7 | Grok | 8 | 26 | 1099 | 1205 | 改了 |
| `deepseek-v4.1-flash.cpp` | DeepSeek V4.1 Flash | DeepSeek | 9 | −6 | 367 | 425 | 改了 |
| `doubao-seed-evolving.cpp` | Doubao-Seed-Evolving | Doubao | 10 | −31 | 402 | 308 | 改了 |
| `kimi-k3.cpp` | Kimi K3 | Kimi | 11 | −129 | 353 | 487 | 改了 |
| `haiku-5.5.cpp` | Claude Haiku 5.5 | Haiku | 12 | −149 | 210 | 210 | 没改（和上半场逐字节相同） |
| `qwen-3.8-max.cpp` | 通义千问 Qwen 3.8 Max | Qwen | 13 | −205 | 537 | 392 | 改了 |
| `glm-5.3.cpp` | 智谱 GLM 5.3 | GLM | 14 | −315 | 551 | 651 | 改了 |

短名是成绩表、回放和配置里用的名字。用 `../workspace/arena.py` 可以直接拿它们当对手，例如在 `workspace/` 里运行 `python arena.py batch submission/bot.cpp ../bots/half2/gpt-6-astra.cpp --games 20`。
