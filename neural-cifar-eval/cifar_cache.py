#!/usr/bin/env python3
"""Shared CIFAR-10 cache/downloader with lock and live progress.

Cache (fixed under the user's home):
  ~/.dsh/eval-guard/cache/cifar10/
    train_images.bin, train_labels.bin
    val_images.bin,   val_labels.bin
    .download.lock   (only while a download is in progress)

A complete cache is reused directly. The lock is created with O_EXCL; if it
already exists, another DSH instance is downloading (or crashed while doing
so), and the user must wait or delete the lock file manually.
"""
from __future__ import annotations

import argparse
import os
import shutil
import signal
import socket
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

URL = 'https://www.cs.toronto.edu/~kriz/cifar-10-binary.tar.gz'
CACHE = Path.home() / '.dsh' / 'eval-guard' / 'cache' / 'cifar10'
TRAIN_N = 50000
VAL_N = 10000
TRAIN_IMAGES = TRAIN_N * 3072
VAL_IMAGES = VAL_N * 3072
TOKEN = os.environ.get('EVAL_INSTALL_TOKEN', '')
EXPECTED = {
    'train_images.bin': TRAIN_IMAGES,
    'train_labels.bin': TRAIN_N,
    'val_images.bin': VAL_IMAGES,
    'val_labels.bin': VAL_N,
}


def cache_complete(cache: Path) -> bool:
    return all((cache / name).is_file() and (cache / name).stat().st_size == size
               for name, size in EXPECTED.items())


def lock_path(cache: Path) -> Path:
    return cache / '.download.lock'


def normalize_proxy(raw: str) -> str:
    raw = raw.strip()
    if raw == '':
        return ''
    return raw if '://' in raw else 'http://' + raw


def opener(proxy: str):
    if proxy:
        handler = urllib.request.ProxyHandler({'http': proxy, 'https': proxy})
        return urllib.request.build_opener(handler)
    return urllib.request.build_opener()


def download(url: str, proxy: str) -> None:
    req = urllib.request.Request(url, headers={'User-Agent': 'dsh-eval-guard-cifar/1.0'})
    with opener(proxy).open(req, timeout=60) as response, tempfile.NamedTemporaryFile(
            prefix='cifar10-', suffix='.tar.gz', dir=str(CACHE), delete=False) as temp:
        total = int(response.headers.get('Content-Length') or 0)
        done = 0
        start = time.time()
        while True:
            chunk = response.read(1024 * 256)
            if not chunk:
                break
            temp.write(chunk)
            done += len(chunk)
            if total:
                pct = done * 100.0 / total
                print(f'\rCIFAR-10 download: {pct:5.1f}%  {done/1048576:7.1f}/{total/1048576:7.1f} MiB', end='', flush=True)
            else:
                print(f'\rCIFAR-10 download: {done/1048576:7.1f} MiB', end='', flush=True)
        print(f'\nCIFAR-10 download complete ({done/1048576:.1f} MiB, {time.time()-start:.1f}s)')
        temp.flush()
        os.fsync(temp.fileno())
        return Path(temp.name)


def materialize(tar_path: Path, cache: Path) -> None:
    print('CIFAR-10 extracting and writing cache ...', flush=True)
    with tempfile.TemporaryDirectory(prefix='cifar10-extract-', dir=str(cache)) as td:
        with tarfile.open(tar_path, 'r:gz') as tf:
            tf.extractall(td)
        base = Path(td) / 'cifar-10-batches-bin'
        tmp_train_images = cache / 'train_images.bin.tmp'
        tmp_train_labels = cache / 'train_labels.bin.tmp'
        tmp_val_images = cache / 'val_images.bin.tmp'
        tmp_val_labels = cache / 'val_labels.bin.tmp'
        with tmp_train_images.open('wb') as images, tmp_train_labels.open('wb') as labels:
            for i in range(1, 6):
                raw = (base / f'data_batch_{i}.bin').read_bytes()
                for off in range(0, len(raw), 3073):
                    labels.write(raw[off:off + 1])
                    images.write(raw[off + 1:off + 3073])
        raw = (base / 'test_batch.bin').read_bytes()
        with tmp_val_images.open('wb') as images, tmp_val_labels.open('wb') as labels:
            for off in range(0, len(raw), 3073):
                labels.write(raw[off:off + 1])
                images.write(raw[off + 1:off + 3073])
        for name in EXPECTED:
            os.replace(cache / (name + '.tmp'), cache / name)


def copy_into(cache: Path, split: str, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    if split in ('train', 'both'):
        shutil.copyfile(cache / 'train_images.bin', target / 'train_images.bin')
        shutil.copyfile(cache / 'train_labels.bin', target / 'train_labels.bin')
    if split in ('val', 'both'):
        shutil.copyfile(cache / 'val_images.bin', target / 'val_images.bin')
        shutil.copyfile(cache / 'val_labels.bin', target / 'val_labels.bin')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--cache', default=str(CACHE))
    parser.add_argument('--proxy', default=os.environ.get('EVAL_CIFAR_PROXY', ''))
    parser.add_argument('--url', default=os.environ.get('EVAL_CIFAR_URL', '') or URL)
    parser.add_argument('--copy-train', default='')
    parser.add_argument('--copy-val', default='')
    args = parser.parse_args()
    cache = Path(args.cache).expanduser()
    cache.mkdir(parents=True, exist_ok=True)
    lock = lock_path(cache)

    if lock.exists():
        try:
            info = lock.read_text(encoding='utf-8', errors='replace').strip()
        except Exception:
            info = ''
        print('CIFAR-10 cache is locked by another download.', file=sys.stderr)
        print('If no other DSH instance is downloading, delete:', str(lock), file=sys.stderr)
        if info:
            print('Lock info:', info, file=sys.stderr)
        return 2

    if not cache_complete(cache):
        fd = None
        try:
            fd = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            print('CIFAR-10 cache is locked by another download. Wait or delete:', str(lock), file=sys.stderr)
            return 2
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as handle:
                fd = None
                handle.write(f'pid={os.getpid()} host={socket.gethostname()} time={time.strftime("%Y-%m-%d %H:%M:%S")} token={TOKEN}\n')
            def _terminate(signum, _frame):
                try:
                    lock.unlink()
                except FileNotFoundError:
                    pass
                os._exit(128 + signum)
            signal.signal(signal.SIGTERM, _terminate)
            signal.signal(signal.SIGINT, _terminate)
            if not cache_complete(cache):
                tar_path = download(args.url, normalize_proxy(args.proxy))
                try:
                    materialize(tar_path, cache)
                finally:
                    try:
                        tar_path.unlink()
                    except OSError:
                        pass
        finally:
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
            try:
                lock.unlink()
            except FileNotFoundError:
                pass

    if not cache_complete(cache):
        print('CIFAR-10 cache is incomplete after download.', file=sys.stderr)
        return 1
    print('CIFAR-10 cache:', cache)
    if args.copy_train:
        copy_into(cache, 'train', Path(args.copy_train))
        print('copied training split to', args.copy_train)
    if args.copy_val:
        copy_into(cache, 'val', Path(args.copy_val))
        print('copied validation split to', args.copy_val)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
