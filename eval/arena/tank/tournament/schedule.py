"""赛程（1 对 1）：每张图上，要打的每一对都换边各打一局。

和往期 公共/竞技场/schedule.py 的区别：往期两人局是"每张图随机两两分组"，一张图上每个程序只碰到一个对手；
这里是每张图上全部配对都打，所以任何两个程序的交手次数、坐两个座位的次数、遇到的地图都完全相同。

    make_schedule(names, maps, seed)                       全配对
    make_schedule(names, maps, seed, pairs=[[a, b], ...])  只打指定的这些对（比如"下半场版本 对 固定对手池"）
    make_schedule(names, maps, seed, map_seeds=[种子, ...]) 地图不抽，按给定的种子依次用（和另一场比赛用同样的图时）

地图风格按图的序号轮换（第 k 张图的风格是 k % 风格数），不靠种子取余，所以每种风格的图数最多差 1。
"""
from __future__ import annotations

import hashlib
import itertools
import json
import random
from collections import Counter

SEED_LOW, SEED_HIGH = 1_000_000, 2 ** 31    # 正式赛的地图种子从这个范围里抽，避开选手本地常用的小种子


def all_pairs(names):
    return [list(p) for p in itertools.combinations(sorted(names), 2)]


def make_schedule(names, maps, seed, pairs=None, n_styles=4, avoid_seeds=(), map_seeds=None):
    """返回局列表。每局：{"id", "map", "map_seed", "style", "seats": [0 号位的名字, 1 号位的名字]}。

    同一张图上的局排在一起（"map" 相同），图内各对的先后顺序打乱；每一对的两局紧挨着。
    map_seeds 给了就不抽地图：第 m 张图用 map_seeds[m]（要和另一场比赛用同样的图时用；这时不看 avoid_seeds）。
    风格仍按图的序号轮换，所以"第 m 张图"在两场比赛里种子和风格都相同。"""
    names = sorted(names)
    if len(set(names)) != len(names):
        raise ValueError("参赛名字有重复")
    if pairs is None:
        pairs = all_pairs(names)
    else:
        seen = set()
        for a, b in pairs:
            if a == b or a not in names or b not in names:
                raise ValueError(f"配对不对：{a} 对 {b}")
            key = tuple(sorted((a, b)))
            if key in seen:
                raise ValueError(f"配对重复：{a} 对 {b}")
            seen.add(key)
        pairs = [sorted(p) for p in pairs]
    if maps < 1 or not pairs:
        raise ValueError("至少要有一张图、一对选手")
    if map_seeds is not None:
        map_seeds = [int(s) for s in map_seeds]
        if len(map_seeds) < maps:
            raise ValueError(f"指定的地图种子只有 {len(map_seeds)} 个，不够 {maps} 张图")
        if len(set(map_seeds[:maps])) != maps:
            raise ValueError("指定的地图种子有重复")
    rng = random.Random(seed)
    used = set(int(s) for s in avoid_seeds)
    games = []
    for m in range(maps):
        if map_seeds is not None:
            map_seed = map_seeds[m]
        else:
            map_seed = rng.randrange(SEED_LOW, SEED_HIGH)
            while map_seed in used:
                map_seed = rng.randrange(SEED_LOW, SEED_HIGH)
            used.add(map_seed)
        order = [list(p) for p in pairs]
        rng.shuffle(order)
        for a, b in order:
            if rng.random() < 0.5:
                a, b = b, a
            for seats in ([a, b], [b, a]):
                games.append({"id": f"m{m:03d}-g{len(games) + 1:05d}", "map": m, "map_seed": map_seed,
                              "style": m % n_styles, "seats": seats})
    return games


def audit(games):
    """赛程是否均衡：每对的局数、每个程序坐两个座位的次数、每种风格的图数。"""
    pair = Counter(tuple(sorted(g["seats"])) for g in games)
    ordered = Counter(tuple(g["seats"]) for g in games)
    seat = Counter((n, i) for g in games for i, n in enumerate(g["seats"]))
    names = sorted({n for g in games for n in g["seats"]})
    styles = Counter()
    for m, style in {(g["map"], g["style"]) for g in games}:
        styles[style] += 1
    maps = len({g["map"] for g in games})
    return {"games": len(games), "maps": maps, "pairs": len(pair),
            "games_per_pair_min": min(pair.values()), "games_per_pair_max": max(pair.values()),
            "swap_balanced": all(ordered[a, b] == ordered[b, a] for a, b in pair),
            "seat_balanced": all(seat[n, 0] == seat[n, 1] for n in names),
            "games_per_program": {n: seat[n, 0] + seat[n, 1] for n in names},
            "maps_per_style": {str(k): v for k, v in sorted(styles.items())},
            "distinct_map_seeds": len({g["map_seed"] for g in games}) == maps}


def games_hash(games):
    """赛程的指纹：续跑前用它确认赛程没有被改过。"""
    raw = json.dumps(games, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()
