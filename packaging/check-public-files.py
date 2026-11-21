#!/usr/bin/env python3
"""Reject local deployment details in public source or staged YAGF packages."""
import argparse
import ipaddress
import json
from pathlib import Path
import re
import socket
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RULES = {
    'personal home path': rb'/' rb'home/' rb'[^/\s"<>]+(?:/|\b)',
    'deployment mount path': rb'/(?:mnt|media)/[A-Za-z0-9_.-]+(?:/|\b)',
    'machine-specific localhost alias': rb'localhost-[0-9]+',
    'private key material': rb'-----BEGIN (?:OPENSSH |RSA |EC |DSA )?PRIVATE KEY-----',
    'cloud access key': rb'\bAKIA[A-Z0-9]{16}\b',
}
URL_IP = re.compile(rb'https?://((?:[0-9]{1,3}\.){3}[0-9]{1,3})(?::[0-9]+)?(?:/|\b)')
DOCUMENTATION_NETWORKS = tuple(ipaddress.ip_network(value) for value in
                               ('192.0.2.0/24', '198.51.100.0/24', '203.0.113.0/24'))


def inspect_bytes(data, label):
    problems = [name for name, pattern in RULES.items() if re.search(pattern, data)]
    name = Path(label).name
    if (name in {'.env', 'id_rsa', 'id_ed25519'} or
            (name.startswith('.env.') and not name.endswith('.example')) or
            name.endswith(('.pem', '.key'))):
        problems.append('private configuration/key file')
    parts = Path(label).parts
    if 'screenshots' in parts and data.startswith(b'\x89PNG\r\n\x1a\n'):
        offset = 8
        while offset + 12 <= len(data):
            size = struct.unpack('>I',data[offset:offset+4])[0]
            kind = data[offset+4:offset+8]
            if kind in (b'tEXt',b'zTXt',b'iTXt',b'tIME',b'eXIf'):
                problems.append('screenshot capture metadata')
                break
            offset += size + 12
    if 'benchmarks' in parts and Path(label).suffix == '.json':
        try:
            report = json.loads(data)
        except (ValueError,UnicodeDecodeError):
            report = {}
        if isinstance(report,dict) and {'host','hardware_observation','started_utc','finished_utc'} & report.keys():
            problems.append('published host/run metadata')
    home = str(Path.home())
    # Match the current builder's private paths in UTF-8 and QString literals.
    if home not in ('/', '/root') and any(home.encode(enc) in data for enc in ('utf-8', 'utf-16le')):
        problems.append('builder home directory')
    hostname = socket.gethostname()
    if hostname not in ('localhost', 'localhost.localdomain') and hostname.encode() in data:
        problems.append('builder hostname')
    for match in URL_IP.finditer(data):
        try:
            address = ipaddress.ip_address(match[1].decode())
        except ValueError:
            continue
        if not address.is_loopback and not any(address in net for net in DOCUMENTATION_NETWORKS):
            problems.append('concrete service IP address')
            break
    if any(part == 'tests' for part in Path(label).parts):
        # Validation fixtures intentionally exercise URLs with dummy credentials.
        return problems
    if re.search(rb'https?://[^\s/:]+:[^\s/@]+@', data):
        problems.append('credentials embedded in URL')
    return problems


def inspect(path, label):
    return inspect_bytes(path.read_bytes(), label)


def staged_files():
    names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
    for name in sorted(set(names)):
        if name:
            # Inherit GIT_INDEX_FILE, including Git's temporary index for commit -a.
            data = subprocess.check_output(['git', '-C', str(ROOT), 'show', ':'+name],stderr=subprocess.DEVNULL)
            yield name, data


def source_files():
    public_dirs = {'src', 'service', 'docs', 'packaging', 'tools', 'tests', 'examples', '.githooks'}
    result = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '--cached', '--others',
                             '--exclude-standard', '-z'],capture_output=True)
    if result.returncode == 0:
        names = result.stdout.decode().split('\0')
    else:
        # Release source archives do not contain Git metadata.
        files = [path for path in ROOT.iterdir() if path.is_file()]
        for name in public_dirs:
            directory = ROOT/name
            if directory.is_dir():
                files.extend(path for path in directory.rglob('*') if path.is_file())
        names = [str(path.relative_to(ROOT)) for path in files]
    for name in sorted(set(names)):
        if not name:
            continue
        path = Path(name)
        if len(path.parts) == 1 or path.parts[0] in public_dirs:
            file = ROOT/path
            if file.is_file():
                yield file, name


def package_files(root):
    candidates = [root/'usr/bin/yagf2', root/'usr/bin/yagf2-benchmark']
    for name in ('usr/share/doc/yagf2', 'usr/share/yagf2', 'usr/share/applications', 'usr/share/metainfo'):
        directory = root/name
        if directory.exists():
            candidates.extend(path for path in directory.rglob('*') if path.is_file())
    for path in candidates:
        if path.is_file():
            yield path, str(path.relative_to(root))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--root', type=Path, help='Check an extracted .deb or AppDir instead of source')
    mode.add_argument('--staged', action='store_true', help='Audit the exact Git index snapshot for a commit')
    args = parser.parse_args()
    errors = []
    count = 0
    try:
        if args.staged:
            for label, data in staged_files():
                count += 1
                errors.extend(f'{label}: {problem}' for problem in inspect_bytes(data, label))
        else:
            for path, label in package_files(args.root) if args.root else source_files():
                count += 1
                errors.extend(f'{label}: {problem}' for problem in inspect(path, label))
    except subprocess.CalledProcessError:
        print('Cannot read the staged Git snapshot.')
        return 1
    if errors:
        print('\n'.join(errors))
        return 1
    if not count:
        print('No public package files found to audit.')
        return 1
    print(f'Public-file audit passed ({count} files).')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
