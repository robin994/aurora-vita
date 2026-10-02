#!/usr/bin/env python3
"""Offline identity for a Vita build and an optional device capture (JSON)."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import zipfile
from pathlib import Path

BUILD_FLAGS = ('AURORA_VITA_ASYNC_GX', 'AURORA_VITA_GXM_DIRECT_STREAM_WRITE', 'AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT',
               'AURORA_VITA_DISTINCT_CPU_CORES', 'AURORA_VITA_GXM_IMMEDIATE_DRAW_VIEW')
CAPTURE_FIELDS = ('title', 'scene', 'clocks', 'resolution', 'cache_state', 'installed_eboot_sha256')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_identity(path):
    path = Path(path).resolve(strict=True)
    if not path.is_file():
        raise ValueError(f'not a file: {path}')
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha256(path.read_bytes())}


def cache_values(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        match = re.match(r'^([^/#][^:]*):[^=]+=(.*)$', line)
        if match:
            values[match[1]] = match[2]
    return values


def git_identity(repo):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args])
    diff = git('diff', 'HEAD', '--binary', '--no-ext-diff', '--no-textconv')
    untracked = sorted(p for p in git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0') if p)
    # Hash file contents as well as names; identical status is not identical work.
    additions = []
    for p in untracked:
        path = repo / p.decode()
        symlink = path.is_symlink()
        content = os.readlink(path).encode() if symlink else path.read_bytes()
        additions.append({'path': p.decode(), 'symlink': symlink, 'sha256': sha256(content)})
    source = {'diff_sha256': sha256(diff), 'untracked': additions}
    return {'commit': git('rev-parse', 'HEAD').decode().strip(),
            'dirty': bool(diff or additions), **source,
            'worktree_sha256': sha256(json.dumps(source, sort_keys=True).encode())}


def build_manifest(repo, build, elf, self_path, vpk, capture=None, capture_log=None):
    repo = Path(repo).resolve(strict=True)
    build = Path(build).resolve(strict=True)
    cache = cache_values(build / 'CMakeCache.txt')
    artifacts = {name: file_identity(path) for name, path in [('elf', elf), ('self', self_path), ('vpk', vpk)]}
    with zipfile.ZipFile(vpk) as archive:
        if archive.namelist().count('eboot.bin') != 1:
            raise ValueError('VPK must contain exactly one eboot.bin')
        eboot = archive.read('eboot.bin')
    artifacts['eboot'] = {'bytes': len(eboot), 'sha256': sha256(eboot)}
    if artifacts['self']['sha256'] != artifacts['eboot']['sha256']:
        raise ValueError('SELF differs from VPK eboot.bin')
    toolchain = {}
    for key in ('CMAKE_TOOLCHAIN_FILE', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER'):
        value = cache.get(key)
        toolchain[key] = file_identity(value) if value else 'unknown'
    compiler = cache.get('CMAKE_CXX_COMPILER')
    toolchain['compiler_version'] = (subprocess.check_output([compiler, '--version'], timeout=10).decode().strip()
                                     if compiler else 'unknown')
    supplied = {} if capture is None else json.loads(Path(capture).read_text())
    if not isinstance(supplied, dict):
        raise ValueError('capture metadata must be a JSON object')
    device = {field: supplied.get(field, 'unknown') for field in CAPTURE_FIELDS}
    installed = device['installed_eboot_sha256']
    if installed != 'unknown' and (not isinstance(installed, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', installed)):
        raise ValueError('installed_eboot_sha256 must be a SHA256 digest or unknown')
    if installed != 'unknown':
        installed = installed.lower()
        device['installed_eboot_sha256'] = installed
        if installed != artifacts['eboot']['sha256']:
            raise ValueError('installed eboot hash does not match this build')
    result = {'schema': 1, 'source': git_identity(repo), 'build_directory': str(build),
              'cmake': {key: cache[key] for key in sorted(cache) if key.startswith(('AURORA_', 'CMAKE_'))},
              'flags': {key: cache.get(key, 'unknown') for key in BUILD_FLAGS},
              'toolchain': toolchain, 'artifacts': artifacts, 'capture': device,
              'installed_hash_verified': installed != 'unknown'}
    if capture_log is not None:
        # Reuse the existing FRAME parser: averages/mixed sessions stay invalid.
        from compare_vita_performance import read_frames, summarize
        result['capture_log'] = file_identity(capture_log)
        result['frame_samples'] = summarize(read_frames(capture_log), result['capture_log']['sha256'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--self', dest='self_path', type=Path, required=True)
    parser.add_argument('--vpk', type=Path, required=True)
    parser.add_argument('--capture', type=Path, help='device metadata JSON; missing fields remain unknown')
    parser.add_argument('--capture-log', type=Path)
    parser.add_argument('--output', type=Path, help='write outside source tree to keep source identity stable')
    args = parser.parse_args()
    try:
        result = build_manifest(args.repo, args.build, args.elf, args.self_path, args.vpk, args.capture, args.capture_log)
        content = json.dumps(result, sort_keys=True, indent=2) + '\n'
        if args.output:
            args.output.write_text(content)
        else:
            print(content, end='')
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        parser.exit(2, f'manifest error: {error}\n')


if __name__ == '__main__':
    main()
