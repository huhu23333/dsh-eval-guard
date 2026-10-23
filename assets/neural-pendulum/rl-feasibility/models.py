"""Networks: PPO actor-critic MLP, SAC squashed-Gaussian actor, twin Q.

PPO: 2x128 tanh, separate actor and critic, state-independent log_std capped
to [-2.3, -0.25] (the recipe of the sibling rounds).
SAC: 2x256 tanh actor with tanh-Gaussian head and a shared-trunk twin Q (two
output heads).  Sharing the critic trunk keeps the CPU update affordable while
preserving the double-Q target.
"""
from __future__ import annotations

import math

import numpy as np
import torch
import torch.nn as nn


class MLP(nn.Module):
    def __init__(self, in_dim, hidden=128, depth=2, out_dim=1, out_gain=0.01):
        super().__init__()
        layers = []
        d = in_dim
        for _ in range(depth):
            lin = nn.Linear(d, hidden)
            nn.init.orthogonal_(lin.weight, gain=np.sqrt(2))
            nn.init.zeros_(lin.bias)
            layers += [lin, nn.Tanh()]
            d = hidden
        self.body = nn.Sequential(*layers)
        self.out = nn.Linear(d, out_dim)
        nn.init.orthogonal_(self.out.weight, gain=out_gain)
        nn.init.zeros_(self.out.bias)

    def forward(self, x):
        return self.out(self.body(x))


# ------------------------------- PPO ---------------------------------------
LOG_STD_MIN, LOG_STD_MAX = -2.3, -0.25


class ActorCritic(nn.Module):
    def __init__(self, obs_dim, act_dim=2, hidden=128, depth=2):
        super().__init__()
        self.act_dim = act_dim
        self.actor = MLP(obs_dim, hidden, depth, act_dim)
        self.critic = MLP(obs_dim, hidden, depth, 1, out_gain=1.0)
        self.log_std = nn.Parameter(torch.full((act_dim,), -0.7))

    def dist(self, obs):
        mean = self.actor(obs)
        std = self.effective_log_std().exp().expand_as(mean)
        return torch.distributions.Normal(mean, std)

    def effective_log_std(self):
        return self.log_std.clamp(LOG_STD_MIN, LOG_STD_MAX)

    def value(self, obs):
        return self.critic(obs).squeeze(-1)

    def deterministic_action(self, obs):
        return self.actor(obs)


# ------------------------------- SAC ---------------------------------------
SAC_LOG_STD_MIN, SAC_LOG_STD_MAX = -5.0, 2.0


class SquashedGaussianActor(nn.Module):
    def __init__(self, in_dim, act_dim=2, hidden=256, depth=2):
        super().__init__()
        self.body = nn.Sequential()
        d = in_dim
        for _ in range(depth):
            lin = nn.Linear(d, hidden)
            nn.init.orthogonal_(lin.weight, gain=math.sqrt(2))
            nn.init.zeros_(lin.bias)
            self.body.extend([lin, nn.Tanh()])
            d = hidden
        self.mean = nn.Linear(hidden, act_dim)
        self.log_std = nn.Linear(hidden, act_dim)
        nn.init.orthogonal_(self.mean.weight, gain=0.01)
        nn.init.zeros_(self.mean.bias)
        nn.init.orthogonal_(self.log_std.weight, gain=0.01)
        nn.init.zeros_(self.log_std.bias)
        self.act_dim = act_dim

    def forward(self, obs):
        h = self.body(obs)
        return self.mean(h), self.log_std(h).clamp(SAC_LOG_STD_MIN, SAC_LOG_STD_MAX)

    def sample(self, obs, det=False):
        if det:
            return torch.tanh(self.mean(self.body(obs))), None
        mean, log_std = self(obs)
        std = log_std.exp()
        eps = torch.randn_like(mean)
        x = mean + std * eps
        a = torch.tanh(x)
        logp = (-0.5 * eps ** 2 - log_std - 0.5 * math.log(2 * math.pi)).sum(-1)
        logp = logp - torch.log(1 - a ** 2 + 1e-6).sum(-1)
        return a, logp


class TwinQ(nn.Module):
    """Shared trunk, two Q heads; forward returns (B, 2)."""

    def __init__(self, in_dim, act_dim=2, hidden=256, depth=2):
        super().__init__()
        self.body = nn.Sequential()
        d = in_dim + act_dim
        for _ in range(depth):
            lin = nn.Linear(d, hidden)
            nn.init.orthogonal_(lin.weight, gain=math.sqrt(2))
            nn.init.zeros_(lin.bias)
            self.body.extend([lin, nn.Tanh()])
            d = hidden
        self.q = nn.Linear(hidden, 2)
        nn.init.orthogonal_(self.q.weight, gain=1.0)
        nn.init.zeros_(self.q.bias)

    def forward(self, obs, act):
        x = torch.cat([obs, act], dim=-1)
        return self.q(self.body(x))
