#!/usr/bin/env python3
"""Exercise launcher decisions with inert shell fixtures, never EP122 or DSOs."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parents[1] / 'src/native-launch.sh').read_text()
rk = '33c093bdc4fbdaeb191942fa39fe1ca5ca8426440981b93d785d517934af52bc'
r8 = 'ae5ce5dcb007bbc9cf24b482959c3fc26c7cdb7f6d011ac04eaed71bd259dbe5'
pre = ['730e1fbb25e3b2151fc97d08724d0f97a3bb178501db44596cf582d391b90415',
       '49a1f650fd0e0abc9a30b85e45ab769474725b0b14fd3c108476ff9e07deb3a2']
with tempfile.TemporaryDirectory(prefix='prestems-launch-test-') as tmp:
    root = Path(tmp)
    (root / 'pdj').mkdir()
    (root / 'mods').mkdir()
    ep = root / 'pdj/EP122'
    ep.write_text('#!/bin/bash\nprintf "%s|%s|%s\\n" "${EP122_NATIVE_OVERCUE:-0}" "${STEMS_NATIVE_ENABLE:-unset}" "$LD_PRELOAD" >> "$PST_TEST_LOG"\n[ "${PST_TEST_FAIL:-0}" != 1 ] || [ "${EP122_NATIVE_OVERCUE:-0}" != 1 ]\n')
    ep.chmod(0o755)
    (root / 'mods/ep122_shim.so').touch()
    (root / 'mods/preui.so').touch()
    script = source.replace('EP_DIR=/home/root/pdj', f'EP_DIR={root}/pdj').replace('MOD_DIR=/home/root/mods', f'MOD_DIR={root}/mods').replace('CONTROL=/mnt/cdj3k-mods-native', f'CONTROL={root}/control')
    script = script.replace('/usr/bin/sha256sum "$1" 2>/dev/null | /usr/bin/awk \'{print $1}\'', 'case "$1" in */EP122) printf "%s" "$PST_TEST_EP_HASH";; *) printf "%s" "$PST_TEST_PRE_HASH";; esac')
    (root / 'launcher').write_text(script)
    log = root / 'attempts'
    def run(ep_hash, pre_hash, fail=False):
        log.write_text('')
        env = {k:v for k,v in os.environ.items() if not (k.startswith(('STEMS_', 'GATECUE_', 'OVERCUE_', 'EP122_')) or k == 'LD_PRELOAD')}
        env.update(PST_TEST_LOG=str(log), PST_TEST_EP_HASH=ep_hash, PST_TEST_PRE_HASH=pre_hash, PST_TEST_FAIL=str(int(fail)))
        subprocess.run(['bash', str(root / 'launcher')], env=env, check=True, capture_output=True, timeout=15)
        return log.read_text().splitlines()
    for ep_hash, pre_hash in zip([rk,r8],pre):
        lines=run(ep_hash,pre_hash)
        assert lines == [f'1|0|{root}/mods/ep122_shim.so:{root}/mods/preui.so'], lines
    for ep_hash,pre_hash in [('unsupported',pre[0]),(rk,'bad-companion')]:
        assert run(ep_hash,pre_hash) == [f'0|unset|{root}/mods/ep122_shim.so']
    lines=run(rk,pre[0],True)
    assert len(lines)==2 and lines[0].startswith('1|0|') and lines[1].startswith('0|unset|'), lines
    assert (root/'control/disabled').is_file()
    assert run(rk,pre[0]) == [f'0|unset|{root}/mods/ep122_shim.so']
    print('Launcher fixture checks: both exact profiles, wrong app, wrong companion, crash fallback, persistent fallback passed.')
