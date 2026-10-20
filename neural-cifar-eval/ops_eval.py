#!/usr/bin/env python3
"""Evaluate Neural-FP32: numeric error + speed per operation.

Reference numbers are computed with PyTorch on the same data/parameters.
No expected tensors are stored in the workspace. Candidate timing only wraps
the CLI call; PyTorch reference computation is outside the timed region.
"""
from __future__ import annotations

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

from common import EVAL_ROOT, clean_copy, emit, fail, parse_workspace

torch.set_num_threads(1)
torch.set_float32_matmul_precision('highest')
torch.use_deterministic_algorithms(True)
torch.backends.mkldnn.deterministic = True
torch.manual_seed(1234)

SEEDS = (11, 22, 33)


def write_tensor(path: Path, t: torch.Tensor) -> None:
    arr = t.detach().cpu().contiguous().numpy().astype('<f4', copy=False)
    path.write_bytes(arr.tobytes())


def read_tensor(path: Path, shape: tuple[int, ...]) -> torch.Tensor:
    arr = np.frombuffer(path.read_bytes(), dtype='<f4').copy()
    return torch.from_numpy(arr).reshape(shape)


def mre(cand: torch.Tensor, ref: torch.Tensor) -> float:
    if cand.shape != ref.shape:
        return float('inf')
    c = cand.detach().cpu().numpy().astype(np.float64)
    r = ref.detach().cpu().numpy().astype(np.float64)
    if not np.isfinite(c).all():
        return float('inf')
    den = max(float(np.abs(r).sum()), 1e-30)
    return float(np.abs(c - r).sum() / den)


def build_candidate(workspace: Path, root: Path) -> Path:
    candidate = root / 'candidate'
    clean_copy(workspace, candidate)
    source = candidate
    if not (source / 'CMakeLists.txt').is_file() and (candidate / 'workspace' / 'CMakeLists.txt').is_file():
        source = candidate / 'workspace'
    if not (source / 'CMakeLists.txt').is_file():
        raise RuntimeError('CMakeLists.txt missing at workspace root or workspace/')
    build = source / 'build'
    if build.exists():
        shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    cfg = subprocess.run(['cmake', '-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release'],
                         cwd=str(source), capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
    if cfg.returncode != 0:
        raise RuntimeError('cmake configure failed\n' + (cfg.stdout + cfg.stderr)[-4000:])
    made = subprocess.run(['cmake', '--build', 'build', '-j4'], cwd=str(source), capture_output=True,
                          text=True, encoding='utf-8', errors='replace', timeout=1800)
    if made.returncode != 0:
        raise RuntimeError('build failed\n' + (made.stdout + made.stderr)[-4000:])
    binary = build / 'nn_task'
    if not binary.is_file():
        raise RuntimeError('build/nn_task missing')
    return binary


class Case:
    def __init__(self, inputs: dict[str, torch.Tensor], outputs: dict[str, torch.Tensor],
                 args, flops: float):
        self.inputs = inputs
        self.outputs = outputs
        self.args = args
        self.flops = flops


def run_operation(name: str, threshold: float, cases: list[Case], binary: Path, root: Path) -> dict:
    op_dir = root / 'ops' / name
    op_dir.mkdir(parents=True, exist_ok=True)
    executable = True
    total_diff = 0.0
    total_ref = 0.0
    total_sec = 0.0
    total_flops = 0.0
    first_error = ''
    compared = False
    for idx, case in enumerate(cases):
        case_dir = op_dir / str(idx)
        case_dir.mkdir(parents=True, exist_ok=True)
        paths: dict[str, str] = {}
        for key, tensor in case.inputs.items():
            p = case_dir / (key + '.bin')
            write_tensor(p, tensor)
            paths[key] = str(p)
        for key in case.outputs:
            paths[key] = str(case_dir / (key + '.bin'))
        cmd = [str(binary)] + [str(x) for x in case.args(paths)]
        started = time.perf_counter()
        try:
            proc = subprocess.run(cmd, cwd=str(case_dir), capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', timeout=300)
        except subprocess.TimeoutExpired:
            executable = False
            first_error = first_error or 'timeout'
            continue
        elapsed = time.perf_counter() - started
        if proc.returncode != 0:
            executable = False
            first_error = first_error or (proc.stderr or proc.stdout or ('exit ' + str(proc.returncode)))[-500:]
            continue
        total_sec += elapsed
        total_flops += case.flops
        for key, ref in case.outputs.items():
            p = case_dir / (key + '.bin')
            if not p.is_file():
                executable = False
                first_error = first_error or ('missing output ' + key)
                continue
            try:
                cand = read_tensor(p, tuple(ref.shape))
            except Exception as exc:
                executable = False
                first_error = first_error or ('failed to read ' + key + ': ' + str(exc))
                continue
            # Numeric comparison is done even if the candidate is otherwise
            # valid; a failed numeric comparison does not suppress speed.
            total_ref += max(float(ref.detach().cpu().numpy().astype(np.float64).__abs__().sum()), 1e-30)
            total_diff += float(np.abs(cand.detach().cpu().numpy().astype(np.float64) -
                                      ref.detach().cpu().numpy().astype(np.float64)).sum())
            compared = True
    value = (total_diff / total_ref) if compared and total_ref > 0 else None
    passed = executable and value is not None and value <= threshold
    speed = (total_flops / total_sec / 1e9) if total_sec > 0 else None
    return {
        'name': name,
        'executable': executable,
        'passed': passed,
        'weighted_mean_relative_error': value,
        'threshold': threshold,
        'speed_gflops': speed,
        'error': first_error if not executable else '',
    }


def t(shape, seed, scale=1.0):
    g = torch.Generator().manual_seed(seed)
    return torch.randn(shape, generator=g, dtype=torch.float32) * scale


# ---------------------------------------------------------------------------
# Case builders. Every operation uses three independent data/parameter sets.
# ---------------------------------------------------------------------------

def cases_elementwise(op: str) -> list[Case]:
    out = []
    for s in SEEDS:
        a = t((64, 256), s)
        b = t((64, 256), s + 100)
        if op == 'div':
            b = torch.where(b.abs() < 0.2, b + 0.5, b)
        ref = {'add': a + b, 'sub': a - b, 'mul': a * b, 'div': a / b}[op]
        out.append(Case({'a': a, 'b': b}, {'out': ref},
                        lambda p: ['elementwise', '--op', op, '--shape', '64,256',
                                   '--a', p['a'], '--b', p['b'], '--out', p['out']],
                        a.numel()))
    return out


def cases_matmul() -> list[Case]:
    out = []
    for s in SEEDS:
        a = t((256, 256), s)
        b = t((256, 256), s + 1)
        out.append(Case({'a': a, 'b': b}, {'out': a @ b},
                        lambda p: ['matmul', '--a-shape', '256,256', '--b-shape', '256,256',
                                   '--a', p['a'], '--b', p['b'], '--out', p['out']],
                        2 * 256 * 256 * 256))
    return out


def cases_linear() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((256, 256), s)
        w = t((128, 256), s + 1)
        b = t((128,), s + 2)
        ref = x @ w.T + b
        out.append(Case({'x': x, 'w': w, 'bias': b}, {'out': ref},
                        lambda p: ['linear', '--x-shape', '256,256', '--w-shape', '128,256',
                                   '--b-shape', '128', '--x', p['x'], '--w', p['w'],
                                   '--bias', p['bias'], '--out', p['out']],
                        2 * 256 * 256 * 128))
    return out


def cases_relu() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((512, 512), s)
        out.append(Case({'x': x}, {'out': torch.relu(x)},
                        lambda p: ['relu', '--shape', '512,512', '--x', p['x'], '--out', p['out']],
                        x.numel()))
    return out


def cases_softmax() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((256, 256), s)
        out.append(Case({'x': x}, {'out': torch.softmax(x, dim=-1)},
                        lambda p: ['softmax', '--shape', '256,256', '--x', p['x'], '--out', p['out']],
                        5 * x.numel()))
    return out


def cases_layernorm() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((256, 256), s)
        gamma = torch.randn(256, generator=torch.Generator().manual_seed(s + 1)) * 0.2 + 1.0
        beta = t((256,), s + 2) * 0.1
        ref = F.layer_norm(x, (256,), gamma, beta, 1e-5)
        out.append(Case({'x': x, 'gamma': gamma, 'beta': beta}, {'out': ref},
                        lambda p: ['layernorm', '--shape', '256,256', '--x', p['x'],
                                   '--gamma', p['gamma'], '--beta', p['beta'],
                                   '--eps', '1e-5', '--out', p['out']],
                        8 * x.numel()))
    return out


def cases_mse() -> list[Case]:
    out = []
    for s in SEEDS:
        pred = t((512, 512), s)
        target = t((512, 512), s + 1)
        ref = ((pred - target) ** 2).mean().reshape(1)
        out.append(Case({'pred': pred, 'target': target}, {'out': ref},
                        lambda p: ['mse', '--pred-shape', '512,512', '--target-shape', '512,512',
                                   '--pred', p['pred'], '--target', p['target'], '--out', p['out']],
                        2 * pred.numel()))
    return out


def conv_output(h: int, w: int, k: int, stride: int, pad: int, dil: int) -> tuple[int, int]:
    return ((h + 2 * pad - dil * (k - 1) - 1) // stride + 1,
            (w + 2 * pad - dil * (k - 1) - 1) // stride + 1)


def cases_conv2d() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((8, 3, 32, 32), s)
        w = t((8, 3, 3, 3), s + 1)
        b = t((8,), s + 2)
        ref = F.conv2d(x, w, b, stride=1, padding=1, dilation=1, groups=1)
        ho, wo = conv_output(32, 32, 3, 1, 1, 1)
        out.append(Case({'x': x, 'w': w, 'bias': b}, {'out': ref},
                        lambda p: ['conv2d', '--x-shape', '8,3,32,32', '--w-shape', '8,3,3,3',
                                   '--b-shape', '8', '--x', p['x'], '--w', p['w'], '--bias', p['bias'],
                                   '--stride', '1,1', '--padding', '1,1', '--dilation', '1,1',
                                   '--groups', '1', '--out', p['out']],
                        2 * 8 * 8 * ho * wo * 3 * 3 * 3))
    return out


def cases_matmul_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        a = t((256, 256), s).requires_grad_()
        b = t((256, 256), s + 1).requires_grad_()
        g = t((256, 256), s + 2)
        c = a @ b
        da, db = torch.autograd.grad(c, (a, b), g)
        out.append(Case({'a': a.detach(), 'b': b.detach(), 'grad': g},
                        {'da': da, 'db': db},
                        lambda p: ['matmul-backward', '--a-shape', '256,256', '--b-shape', '256,256',
                                   '--a', p['a'], '--b', p['b'], '--grad', p['grad'],
                                   '--da', p['da'], '--db', p['db']],
                        4 * 256 * 256 * 256))
    return out


def cases_linear_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((256, 256), s).requires_grad_()
        w = t((128, 256), s + 1).requires_grad_()
        bias = t((128,), s + 2).requires_grad_()
        y = x @ w.T + bias
        g = t((256, 128), s + 3)
        dx, dw, db = torch.autograd.grad(y, (x, w, bias), g)
        out.append(Case({'x': x.detach(), 'w': w.detach(), 'bias': bias.detach(), 'grad': g},
                        {'dx': dx, 'dw': dw, 'db': db},
                        lambda p: ['linear-backward', '--x-shape', '256,256', '--w-shape', '128,256',
                                   '--b-shape', '128', '--x', p['x'], '--w', p['w'], '--bias', p['bias'],
                                   '--grad', p['grad'], '--dx', p['dx'], '--dw', p['dw'], '--db', p['db']],
                        4 * 256 * 256 * 128))
    return out


def cases_relu_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((512, 512), s).requires_grad_()
        g = t((512, 512), s + 1)
        y = torch.relu(x)
        dx = torch.autograd.grad(y, x, g)[0]
        out.append(Case({'x': x.detach(), 'grad': g}, {'dx': dx},
                        lambda p: ['relu-backward', '--shape', '512,512', '--x', p['x'],
                                   '--grad', p['grad'], '--dx', p['dx']],
                        x.numel()))
    return out


def cases_layernorm_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((256, 256), s).requires_grad_()
        gamma = (torch.randn(256, generator=torch.Generator().manual_seed(s + 1)) * 0.2 + 1.0).requires_grad_()
        g = t((256, 256), s + 2)
        y = F.layer_norm(x, (256,), gamma, None, 1e-5)
        dx, dgamma = torch.autograd.grad(y, (x, gamma), g)
        out.append(Case({'x': x.detach(), 'gamma': gamma.detach(), 'grad': g},
                        {'dx': dx, 'dgamma': dgamma},
                        lambda p: ['layernorm-backward', '--shape', '256,256', '--x', p['x'],
                                   '--gamma', p['gamma'], '--grad', p['grad'], '--eps', '1e-5',
                                   '--dx', p['dx'], '--dgamma', p['dgamma']],
                        10 * x.numel()))
    return out


def cases_mse_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        pred = t((512, 512), s).requires_grad_()
        target = t((512, 512), s + 1).requires_grad_()
        loss = ((pred - target) ** 2).mean()
        g = torch.tensor(1.0)
        dp, dt = torch.autograd.grad(loss, (pred, target), g)
        out.append(Case({'pred': pred.detach(), 'target': target.detach(), 'grad': g.reshape(1)},
                        {'dp': dp, 'dtarget': dt},
                        lambda p: ['mse-backward', '--pred-shape', '512,512', '--target-shape', '512,512',
                                   '--pred', p['pred'], '--target', p['target'], '--grad', p['grad'],
                                   '--dp', p['dp'], '--dtarget', p['dtarget']],
                        4 * pred.numel()))
    return out


def cases_conv2d_backward() -> list[Case]:
    out = []
    for s in SEEDS:
        x = t((4, 3, 16, 16), s).requires_grad_()
        w = t((8, 3, 3, 3), s + 1).requires_grad_()
        b = t((8,), s + 2).requires_grad_()
        y = F.conv2d(x, w, b, stride=1, padding=1)
        g = t((4, 8, 16, 16), s + 3)
        dx, dw, db = torch.autograd.grad(y, (x, w, b), g)
        out.append(Case({'x': x.detach(), 'w': w.detach(), 'bias': b.detach(), 'grad': g},
                        {'dx': dx, 'dw': dw, 'db': db},
                        lambda p: ['conv2d-backward', '--x-shape', '4,3,16,16', '--w-shape', '8,3,3,3',
                                   '--b-shape', '8', '--x', p['x'], '--w', p['w'], '--bias', p['bias'],
                                   '--grad', p['grad'], '--stride', '1,1', '--padding', '1,1',
                                   '--dilation', '1,1', '--groups', '1',
                                   '--dx', p['dx'], '--dw', p['dw'], '--db', p['db']],
                        4 * 4 * 8 * 16 * 16 * 3 * 3 * 3))
    return out


def adamw_ref(p, g, m, v, lr, b1, b2, eps, wd, step):
    m2 = b1 * m + (1 - b1) * g
    v2 = b2 * v + (1 - b2) * g * g
    mh = m2 / (1 - b1 ** step)
    vh = v2 / (1 - b2 ** step)
    p2 = p * (1 - lr * wd) - lr * mh / (torch.sqrt(vh) + eps)
    return p2, m2, v2


def cases_adamw() -> list[Case]:
    out = []
    for s, step in zip(SEEDS, (1, 3, 7)):
        p = t((256, 256), s)
        g = t((256, 256), s + 1)
        m = torch.zeros_like(p)
        v = torch.zeros_like(p)
        ref = adamw_ref(p, g, m, v, 1e-3, 0.9, 0.999, 1e-8, 0.01, step)
        out.append(Case({'param': p, 'grad': g, 'm': m, 'v': v},
                        {'param_out': ref[0], 'm_out': ref[1], 'v_out': ref[2]},
                        lambda pth, step=step: ['adamw', '--param', pth['param'], '--grad', pth['grad'],
                                     '--m', pth['m'], '--v', pth['v'], '--lr', '0.001',
                                     '--beta1', '0.9', '--beta2', '0.999', '--eps', '1e-8',
                                     '--weight-decay', '0.01', '--step', str(step),
                                     '--out-param', pth['param_out'], '--out-m', pth['m_out'], '--out-v', pth['v_out']],
                        10 * p.numel()))
    return out


def cases_sgd_momentum() -> list[Case]:
    out = []
    for s in SEEDS:
        p = t((256, 256), s)
        g = t((256, 256), s + 1)
        buf = t((256, 256), s + 2) * 0.01
        lr, mu = 0.05, 0.9
        b2 = mu * buf + g
        p2 = p - lr * b2
        out.append(Case({'param': p, 'grad': g, 'buffer': buf}, {'param_out': p2, 'buffer_out': b2},
                        lambda pth: ['sgd-momentum', '--param', pth['param'], '--grad', pth['grad'],
                                     '--momentum-buffer', pth['buffer'], '--lr', '0.05',
                                     '--momentum', '0.9', '--out-param', pth['param_out'],
                                     '--out-buffer', pth['buffer_out']],
                        6 * p.numel()))
    return out


OPERATIONS = [
    ('elementwise.add', 1e-6, lambda: cases_elementwise('add')),
    ('elementwise.sub', 1e-6, lambda: cases_elementwise('sub')),
    ('elementwise.mul', 1e-6, lambda: cases_elementwise('mul')),
    ('elementwise.div', 1e-6, lambda: cases_elementwise('div')),
    ('matmul', 1e-5, cases_matmul),
    ('linear', 1e-5, cases_linear),
    ('relu', 1e-6, cases_relu),
    ('softmax', 1e-5, cases_softmax),
    ('layernorm', 1e-5, cases_layernorm),
    ('mse', 1e-6, cases_mse),
    ('conv2d', 1e-4, cases_conv2d),
    ('matmul-backward', 1e-4, cases_matmul_backward),
    ('linear-backward', 1e-4, cases_linear_backward),
    ('relu-backward', 1e-4, cases_relu_backward),
    ('layernorm-backward', 1e-4, cases_layernorm_backward),
    ('mse-backward', 1e-4, cases_mse_backward),
    ('conv2d-backward', 1e-4, cases_conv2d_backward),
    ('adamw', 1e-5, cases_adamw),
    ('sgd-momentum', 1e-6, cases_sgd_momentum),
]


def main() -> int:
    workspace = parse_workspace()
    if not torch.__version__:
        return fail({'task': 'neural-fp32', 'ok': False, 'error': 'PyTorch unavailable'})
    with tempfile.TemporaryDirectory(prefix='neural-fp32-eval-') as td:
        root = Path(td)
        try:
            binary = build_candidate(workspace, root)
        except Exception as exc:
            return fail({'task': 'neural-fp32', 'ok': False, 'error': str(exc)})
        rows = []
        for name, threshold, builder in OPERATIONS:
            try:
                cases = builder()
                row = run_operation(name, threshold, cases, binary, root)
            except Exception as exc:
                row = {'name': name, 'executable': False, 'passed': False,
                       'weighted_mean_relative_error': None, 'threshold': threshold,
                       'speed_gflops': None, 'error': str(exc)}
            rows.append(row)

        all_exec = all(row['executable'] for row in rows)
        errs = [row['weighted_mean_relative_error'] for row in rows
                if row['weighted_mean_relative_error'] is not None]
        speeds = [row['speed_gflops'] for row in rows if row['speed_gflops'] and row['speed_gflops'] > 0]
        max_err = max(errs) if errs else None
        geomean = math.exp(sum(math.log(v) for v in speeds) / len(speeds)) if speeds else None

        print(f"all_operations_executable = {str(all_exec).lower()}")
        print(f"max_weighted_mean_relative_error = {max_err}")
        print(f"speed_geomean = {geomean}")
        for row in rows:
            print(f"{row['name']}: executable={row['executable']} pass={row['passed']} "
                  f"mre={row['weighted_mean_relative_error']} speed_gflops={row['speed_gflops']}")
            if row['error']:
                print(f"  error={row['error'][:300]}")

        result = {
            'task': 'neural-fp32',
            'ok': True,
            'all_operations_executable': all_exec,
            'max_weighted_mean_relative_error': max_err,
            'speed_geomean': geomean,
            'operations': rows,
            'pytorch': torch.__version__,
        }
        emit(result)
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
