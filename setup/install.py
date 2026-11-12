#!/usr/bin/env python3
# OoT Online's installer, the part that does the work: the Mac app "Install OoT Online" runs it with the Python it carries, so a player needs no
# Xcode, Homebrew or Terminal. It turns YOUR OWN Ocarina of Time dump (a .nup) into ~/Applications/Ocarina of Time Online.app:
#   1. checks this Mac (Apple chip, macOS 26.6+, room on the disk, Rosetta on macOS 26 for the shader translator)
#   2. gets nxnative (the MIT-licensed tool that runs the game natively) from GitHub at the commit OoT Online was tested with, and puts OoT
#      Online's changes into it (patches/, tools/integrate_nxnative.py)
#   3. builds the game app from the dump with nxnative --no-xcode: the shaders through macOS's own Metal compiler, the game's host and the app's
#      launcher prebuilt (the installer's prebuilt/, made by GitHub Actions from the same sources)
#   4. takes the game's fonts out of the dump for OoT Online's screens and makes the other players' stand-ins (tools/game_fonts.py, make_online_mod.py)
#   5. builds the game's render pipelines once, so the first start is smooth
# Nothing from the game is downloaded or uploaded: everything made from it stays on this Mac.
# Usage: <python> -I install.py --nup <file.nup> [--server ADDR] [--app PATH] [--work DIR] [--data DIR] [--json]
#   --json: one JSON object per line on standard output, for the installer app (events: phase, stage, step, progress, stage_end, say, done, error)
#   OOT_ONLINE_SERVER=<address:port> is the default --server; without either, the address on the website (server.txt), if any
import argparse, hashlib, json, os, platform, re, shutil, subprocess, sys, tarfile, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OOT = HERE.parent                                  # OoT Online's files (patches/, tools/, client/, setup/)
PAYLOAD = OOT.parent                               # the installer's payload: python/, prebuilt/, oot-online/
TITLE = '0400486024DEC000'
SITE = 'https://bhoneka.github.io/oot-online'
NX_URL = 'https://codeload.github.com/seedds/nxnative/tar.gz/{commit}'
PATCHES = ['nxnative-mac26-translator.patch', 'nxnative-xcode27.1-blend-ref.patch', 'nxnative-frame-step-gpu-span.patch', 'nxnative-no-xcode.patch']
HOME = Path.home()
SUPPORT = HOME / 'Library/Application Support/OoT Online'
SYS_PATH = '/usr/bin:/bin:/usr/sbin:/sbin'         # what tools this runs may find: macOS's own (no Homebrew)
JSON = False
PHASES = ['Checking this Mac', 'Getting nxnative', 'Building your game', 'Fonts and other players', 'Preparing the graphics']


class Stop(Exception): pass


def emit(ev, **kw):
    if JSON: print(json.dumps({'ev': ev, **kw, 't': round(time.time(), 3)}, ensure_ascii=False), flush=True)
    elif ev == 'phase': print(f'==> {kw["k"]}/{kw["n"]} {kw["name"]}', flush=True)
    elif ev == 'say': print(kw['msg'], flush=True)
    elif ev == 'done': print(f'==> Done: {kw["app"]}', flush=True)


def say(msg): emit('say', msg=msg)
def phase(k): emit('phase', k=k, n=len(PHASES), name=PHASES[k - 1])


def tool_env(**extra):   # the environment of everything this starts: macOS's tools only, and xcrun pointed nowhere, so nothing reaches for Xcode (or offers to install it)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'DYLD_', 'CONDA', 'VIRTUAL_ENV'))}
    env.update(PATH=SYS_PATH, DEVELOPER_DIR='/var/empty/no-xcode', LC_ALL='en_US.UTF-8', LANG='en_US.UTF-8')
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(cmd, log=None, **kw):   # a tool, its output to log (or kept for the error), raising Stop with its last lines when it fails
    with (open(log, 'a') if log else tempfile.TemporaryFile('w+')) as f:
        r = subprocess.run([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT, env=kw.pop('env', None) or tool_env(), **kw)
        if r.returncode:
            if not log: f.seek(0)
            tail = (open(log).read() if log else f.read()).strip().splitlines()[-6:]
            raise Stop(f'{Path(str(cmd[0])).name} failed (exit {r.returncode}): ' + ' | '.join(tail))


def macos():   # (major, minor)
    v = (platform.mac_ver()[0] or '0.0').split('.')
    return int(v[0]), int(v[1]) if len(v) > 1 else 0


def check_mac(nup, work):
    if platform.machine() != 'arm64' or subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() != '1':
        raise Stop('OoT Online needs a Mac with an Apple chip (M1 or later).')
    if macos() < (26, 6): raise Stop(f'OoT Online needs macOS 26.6 or later; this Mac has {platform.mac_ver()[0]}.')
    if not nup.is_file(): raise Stop(f'Cannot find the game file: {nup}')
    # read now: in Downloads, Documents, Desktop, iCloud Drive or on another disk, macOS asks once whether the installer may read there (the build waits for the answer)
    guarded = [HOME / d for d in ('Downloads', 'Documents', 'Desktop', 'Library/Mobile Documents')] + [Path('/Volumes')]
    if any(nup.is_relative_to(g) for g in guarded):
        say('If macOS asks whether Install OoT Online may access the folder your game file is in, click Allow.')
    try:
        with open(nup, 'rb') as f: f.read(16)
    except PermissionError:
        raise Stop(f'macOS did not let the installer read your game file ({nup}). Open System Settings → Privacy & Security → Files & Folders, turn on '
                   'the folder your game file is in for Install OoT Online, and build again; or move the game file to another folder and choose it again.')
    work.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(work).free / 2**30
    say(f'{free:.0f} GB free on the disk')
    if free < 60: say(f'[RED] The first build needs about 60 GB free, and this disk has {free:.0f} GB: it may run out of room.')
    if macos() < (27, 0) and subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode:
        # nxnative's shader translator runs as Intel code on macOS 26 (its Apple-chip half needs macOS 27): Rosetta, which macOS installs on request
        say('Installing Rosetta (Apple\'s translation for Intel code), which the shader translator needs on macOS 26: macOS asks for your password')
        r = subprocess.run(['osascript', '-e', 'do shell script "/usr/sbin/softwareupdate --install-rosetta --agree-to-license" with administrator privileges '
                            'with prompt "OoT Online needs Rosetta to translate the game\'s shaders."'], capture_output=True, text=True)
        if r.returncode or subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode:
            raise Stop('Rosetta is not installed, and the shader translator needs it on macOS 26. Install it (open any Intel app, or update to macOS 27) and try again. '
                       + (r.stderr.strip() or ''))


def manifest():
    m = json.loads((PAYLOAD / 'prebuilt/manifest.json').read_text())
    if m.get('game') != TITLE: raise Stop(f'this installer\'s prebuilt game host is for {m.get("game")}, not Ocarina of Time')
    return m


def get_nxnative(dst_root, logs, chain):
    """nxnative at the tested commit with OoT Online's changes, in dst_root/<commit>: downloaded from GitHub (codeload's archive of the commit, no git), patched with /usr/bin/patch,
    OoT Online's module put in (integrate_nxnative.py). Made again when this installer's patches change (the stamp); its chain version must be the prebuilt host's"""
    commit = (OOT / 'patches/NXNATIVE_COMMIT').read_text().strip()
    stamp = hashlib.sha256(commit.encode() + b''.join((OOT / 'patches' / p).read_bytes() for p in PATCHES) + (OOT / 'tools/integrate_nxnative.py').read_bytes()
                           + b''.join(p.read_bytes() for p in sorted((OOT / 'client').rglob('*')) if p.is_file())).hexdigest()
    tree = dst_root / commit[:12]
    if (tree / '.oot-online-stamp').is_file() and (tree / '.oot-online-stamp').read_text() == stamp:
        say(f'nxnative {commit[:12]}: ready from an earlier install')
    else:
        dst_root.mkdir(parents=True, exist_ok=True)
        tgz = dst_root / f'nxnative-{commit[:12]}.tar.gz'   # kept: a new OoT Online on the same nxnative commit only patches it again
        with tempfile.TemporaryDirectory(dir=dst_root) as t:
            for attempt in (1, 2):
                if not tgz.is_file():
                    emit('stage', name='Downloading nxnative', step=commit[:12])
                    run(['curl', '-fL', '--retry', '3', '--retry-delay', '5', '-sS', '-o', f'{tgz}.part', NX_URL.format(commit=commit)], log=logs / 'download.log')
                    os.replace(f'{tgz}.part', tgz)
                    emit('stage_end', name='Downloading nxnative', ok=True, summary=f'{tgz.stat().st_size / 2**20:.0f} MB')
                emit('stage', name='Unpacking nxnative', step='')
                try:
                    with tarfile.open(tgz) as tf: tf.extractall(t, filter='tar')
                    break
                except (tarfile.TarError, OSError, EOFError) as e:   # a damaged download: fetched once more
                    tgz.unlink(missing_ok=True)
                    for p in Path(t).iterdir(): shutil.rmtree(p, ignore_errors=True)
                    if attempt == 2: raise Stop(f'the nxnative download is damaged ({e})')
            src = next(p for p in Path(t).iterdir() if p.is_dir() and p.name.startswith('nxnative-'))
            emit('stage_end', name='Unpacking nxnative', ok=True)
            emit('stage', name='Adding OoT Online', step='patches')
            for p in PATCHES:
                run(['patch', '-p1', '--batch', '--forward', '-s', '-d', src, '-i', OOT / 'patches' / p], log=logs / 'patch.log')
            run([sys.executable, '-I', '-B', OOT / 'tools/integrate_nxnative.py', src], log=logs / 'integrate.log')
            emit('stage_end', name='Adding OoT Online', ok=True)
            (src / '.oot-online-stamp').write_text(stamp)
            if tree.exists(): shutil.rmtree(tree)
            src.rename(tree)
    have = subprocess.run([sys.executable, '-I', '-B', tree / 'dm/gfx/tools/dmsh_chain.py'], capture_output=True, text=True, env=tool_env()).stdout.strip()
    if have != chain: raise Stop(f'nxnative\'s shader chain is {have or "unknown"}, and this installer\'s game host was built for {chain}: the download is not the tested source')
    return tree, commit


def build_game(tree, nup, app, work, env_file, logs, commit):
    """nxnative's build (scripts/build_ipa_from_nsp.py --no-xcode), its JSON progress passed on as it comes; [RED] messages kept for the error"""
    cmd = [sys.executable, '-I', tree / 'scripts/build_ipa_from_nsp.py', nup, '--platform', 'mac-app', '--no-xcode', '--prebuilt', PAYLOAD / 'prebuilt',
           '--work-dir', work, '-o', app, '--env-file', env_file, '--progress', 'json']
    env = tool_env(DM_SOURCE_COMMIT=f'{commit[:12]}+oot-online', J=os.cpu_count() or 8)
    red = []
    with open(logs / 'build.log', 'a') as log:
        log.write(f'\n==== {time.strftime("%Y-%m-%d %H:%M:%S")} {" ".join(map(str, cmd))}\n'); log.flush()
        p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=log, text=True, errors='replace', env=env, cwd=work)
        for line in p.stdout:
            log.write(line); log.flush()
            try: ev = json.loads(line)
            except ValueError: continue
            if not isinstance(ev, dict) or 'ev' not in ev: continue
            if ev['ev'] == 'say' and str(ev.get('msg', '')).startswith('[RED]'): red.append(ev['msg'][6:])
            if JSON: print(json.dumps(ev, ensure_ascii=False), flush=True)
            elif ev['ev'] in ('stage', 'stage_end', 'say'): print('   ', ev.get('name') or '', ev.get('msg') or ev.get('summary') or '', flush=True)
        if p.wait(): raise Stop(red[-1] if red else f'the build stopped (exit {p.returncode}): see {logs / "build.log"}')
    if not (app / 'Contents/MacOS/dmhost').is_file(): raise Stop(f'the build ended without the app: see {logs / "build.log"}')


def online_files(work, data, logs, tree):
    images = sorted(work.glob('extract/v*/RomFS.img'), key=lambda p: int(re.sub(r'\D', '', p.parent.name) or 0))
    if not images: raise Stop(f'the build left no RomFS image in {work / "extract"}')
    romfs = images[-1]   # the newest version the build unpacked: the one the app runs
    emit('stage', name='The game\'s fonts', step=romfs.parent.name)
    (data / 'oot_online/fonts').mkdir(parents=True, exist_ok=True)
    env = tool_env(NXNATIVE_DIR=tree)   # the tools read the image with nxnative's RomFS reader
    run([sys.executable, '-I', '-B', OOT / 'tools/game_fonts.py', romfs, data / 'oot_online/fonts', 'Clarimo', 'Cetra', 'External', 'NinDev', 'Symbol', 'ZeldaGlyphs'], log=logs / 'fonts.log', env=env)
    emit('stage_end', name='The game\'s fonts', ok=True)
    emit('stage', name='The other players\' stand-ins', step='')
    run([sys.executable, '-I', '-B', OOT / 'tools/make_online_mod.py', romfs, data / 'oot_online/romfs'], log=logs / 'mod.log', env=env)
    emit('stage_end', name='The other players\' stand-ins', ok=True)
    env_txt = data / 'env.txt'
    if env_txt.is_file():   # an earlier version of the setup put OoT Online's switches in the game's shared env.txt: taken out again (they live in the app)
        keep = [l for l in env_txt.read_text().splitlines(True) if not re.match(r'(DM_MP_SERVER|DM_ROMFS_OVERLAY|DM_MP_STANDIN|DM_MP_LIVE)=', l)]
        env_txt.write_text(''.join(keep))


def prebuild_pipelines(app, data, logs):
    """The base pipeline of every shader pair, built now into the app's pipeline archive (what the game would otherwise build in the background on its first start):
    the app's own host in its tool mode (DM_PSO_PREBUILD=tool), with the folders the launcher gives it (launcher.c)"""
    res = app / 'Contents/Resources'
    emit('stage', name='Render pipelines', step='a few minutes')
    for d in ('save', 'temp'): (data / d).mkdir(parents=True, exist_ok=True)
    env = tool_env(DM_PSO_PREBUILD='tool', DM_ROOT=data, DM_SAVE=data / 'save', DM_TEMP=data / 'temp', DM_MSL_DIR=res / 'msl', DM_NVN_SHMAP=res / 'dm/shader_map.tsv',
                   DM_FONT_DIR=res / 'dm/font', DM_ROMFS=res / 'romfs.img')
    t0 = time.time()
    try: run([app / 'Contents/MacOS/dmhost'], log=logs / 'pipelines.log', env=env, cwd=data)
    except Stop as e:   # an optimization: the game builds them itself on its first start
        say(f'[RED] the render pipelines were not built now ({e}); the game builds them on its first start')
        emit('stage_end', name='Render pipelines', ok=True, warn=True, summary='left to the first start'); return
    built = re.findall(r'pipeline prebuild: .*', (logs / 'pipelines.log').read_text(errors='replace'))
    emit('stage_end', name='Render pipelines', ok=True, summary=(built[-1][:120] if built else f'{time.time() - t0:.0f} s'))


def server_address(given):
    """The server the game joins: the window's (--server), else OOT_ONLINE_SERVER, else the public one: the website's server.txt, read now (so the
    server can move without a new installer), or, when the website cannot be reached, the address this installer was built with (server.txt)"""
    if given: return given
    if os.environ.get('OOT_ONLINE_SERVER'): return os.environ['OOT_ONLINE_SERVER']
    r = subprocess.run(['curl', '-fsSL', '--max-time', '20', f'{SITE}/server.txt'], capture_output=True, text=True, env=tool_env())
    live = (r.stdout.split() or [''])[0] if r.returncode == 0 else ''
    if re.fullmatch(r'[A-Za-z0-9.\-\[\]:]+:\d+', live): return live
    built = OOT / 'server.txt'
    return ((built.read_text().split() or [''])[0] if built.is_file() else '')


def main():
    global JSON
    ap = argparse.ArgumentParser(description='Build Ocarina of Time Online from your own game file')
    ap.add_argument('--nup', required=True, help='your own dump of the game (.nup)')
    ap.add_argument('--server', default='', help='the server (address:port); default OOT_ONLINE_SERVER, else the website\'s server.txt')
    ap.add_argument('--app', default=str(HOME / 'Applications/Ocarina of Time Online.app'))
    ap.add_argument('--work', default=str(SUPPORT / 'build'), help='the build folder (the unpacked game, the shader cache: kept for updates)')
    ap.add_argument('--data', default=str(HOME / f'Library/Application Support/nxnative/{TITLE}'), help='the game\'s data folder (saves, OoT Online\'s fonts and stand-ins)')
    ap.add_argument('--support', default=str(SUPPORT), help='OoT Online\'s own folder (nxnative\'s source, the logs, the online switches)')
    ap.add_argument('--skip-pipelines', action='store_true', help='leave the render pipelines to the game\'s first start (tests: that step runs the game\'s host)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    JSON = a.json
    nup, app, work, data, support = (Path(os.path.expanduser(x)).resolve() for x in (a.nup, a.app, a.work, a.data, a.support))
    logs = support / 'logs'; logs.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    try:
        phase(1); check_mac(nup, work)
        m = manifest()
        phase(2); tree, commit = get_nxnative(support / 'source', logs, m['chain'])
        server = server_address(a.server)
        say(f'server: {server}' if server else 'no server address (the website could not be reached): the game is built without one; open the installer again later to add it')
        env_file = support / 'online_env.txt'   # OoT Online's switches go into the app's own env file (Contents/Resources/dm/env.txt): the save is never written online (hle_mp.c)
        env_file.write_text(f'DM_ROMFS_OVERLAY={data}/oot_online/romfs\nDM_MP_STANDIN=OnlinePlayer\nDM_MP_LIVE=1\n' + (f'DM_MP_SERVER={server}\n' if server else ''))
        phase(3); app.parent.mkdir(parents=True, exist_ok=True); build_game(tree, nup, app, work, env_file, logs, commit)
        phase(4); online_files(work, data, logs, tree)
        phase(5)
        if a.skip_pipelines: say('render pipelines: left to the game\'s first start (--skip-pipelines)')
        else: prebuild_pipelines(app, data, logs)
    except Stop as e:
        emit('error', msg=str(e), log=str(logs));
        if not JSON: print(f'✗ {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        emit('error', msg='stopped'); return 130
    emit('done', app=str(app), seconds=round(time.time() - t0), server=server)
    return 0


if __name__ == '__main__':
    sys.exit(main())
