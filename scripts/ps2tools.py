"""Pieces shared by the scripts that drive scripts/port.sh: autotests.py (the free test suite) and report.py (one game).

Python 3 standard library only. Nothing here knows about a particular test or game: finding Git Bash, running port.sh
with a timeout that kills the whole process tree and per-step timings, a shared compile-job pool, the translator
tools, and reading the lines the runner logs (unhandled syscalls, missing IOP library calls, warnings, halt reason).
"""
import os
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STEPS = {1: 'disc', 2: 'executable', 3: 'translate', 4: 'explore', 5: 'vector units', 6: 'build'}
# Lines the runtime itself prints, which can land inside the program's text on the shared stderr.
RUNTIME_LINE = re.compile(r'^\[[a-z0-9_]+\]( |$)')
SYSCALL = re.compile(r'^\[sys\] unhandled syscall (0x[0-9A-Fa-f]+)')
IOP_MISSING = re.compile(r'^\[iop\] missing (\S+) #(\d+)')
WARNING = re.compile(r'^\[(?!sys\] unhandled syscall|iop\] missing)[a-z0-9_]+\] .*(unhandled|unsupported|unknown|unimplemented|not implemented|'
                     r'cannot|can\'t|\bfault\b|failed|not an ELF|did not return|out of memory|exhausted)', re.I)
HEX = re.compile(r'0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{8}\b')

PROGRAM = 'ps2tools'  # the name die() puts in front of its message; each script sets its own


def set_program(name):
    global PROGRAM
    PROGRAM = name


def say(*args):
    print(*args, flush=True)


def die(message):
    sys.exit(PROGRAM + ': ' + message)


# ---------------------------------------------------------------- running port.sh

def find_bash():
    if os.environ.get('PS2_BASH'):
        return os.environ['PS2_BASH']
    if os.name != 'nt':
        return 'bash'
    candidates = []
    git = shutil.which('git')
    if git:  # ...\Git\cmd\git.exe -> ...\Git\bin\bash.exe (not the WSL bash.exe in System32)
        top = os.path.dirname(os.path.dirname(os.path.realpath(git)))
        candidates += [os.path.join(top, 'bin', 'bash.exe'), os.path.join(top, 'usr', 'bin', 'bash.exe')]
    candidates.append(os.path.join(os.environ.get('ProgramFiles', 'C:\\Program Files'), 'Git', 'bin', 'bash.exe'))
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    die('Git Bash not found: put bash on the PATH or set PS2_BASH')


def kill_tree(process):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        import signal
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except OSError:
            process.kill()


def new_group_kwargs():
    """Popen arguments that put the child in its own process group, so that kill_tree can take the whole tree down."""
    if os.name == 'nt':
        return {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
    return {'start_new_session': True}


class JobPool:
    """A GNU make style jobserver (a named semaphore, which ninja 1.13 understands on Windows) that all the concurrent
    ninja runs share, so that four builds at once do not start four times as many compilers as the PC has cores
    (and memory: a compiler job of the generated code takes a few hundred MB)."""

    def __init__(self, tokens, prefix='ps2autotests'):
        self.handle = None
        self.flags = None
        if os.name != 'nt' or tokens <= 0:
            return
        import ctypes
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateSemaphoreW.restype = ctypes.c_void_p
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        name = '%s_%d' % (prefix, os.getpid())
        self.handle = self.kernel.CreateSemaphoreW(None, tokens, tokens, name)
        if self.handle:
            self.flags = ' -j%d --jobserver-auth=%s' % (tokens + 1, name)

    def env(self):
        return {'MAKEFLAGS': self.flags} if self.flags else {}

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def run_port(port_args, log_path, timeout, pool=None, on_line=None):
    """Run `port.sh <port_args>` with its whole output in log_path.

    Returns (exit code or None on timeout, [(step number, seconds into the run)], seconds). A run that takes longer than
    `timeout` seconds is killed with all its child processes. `pool` is a JobPool the compile jobs are taken from;
    `on_line(bytes)` is called for every line of output as it arrives (from another thread)."""
    command = [find_bash(), os.path.join(ROOT, 'scripts', 'port.sh')] + [str(a) for a in port_args]
    env = dict(os.environ)
    env.pop('MAKEFLAGS', None)
    if pool is not None:
        env.update(pool.env())
    started = time.time()
    steps = []
    with open(log_path, 'wb') as log:
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   env=env, **new_group_kwargs())

        def pump():
            for line in process.stdout:
                log.write(line)
                log.flush()
                match = re.match(rb'== \[(\d)/6\]', line)
                if match:
                    steps.append((int(match.group(1)), time.time() - started))
                if on_line:
                    on_line(line)

        pumper = threading.Thread(target=pump, daemon=True)
        pumper.start()
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(process)
            process.wait()
            code = None
        except KeyboardInterrupt:  # Ctrl+C in the terminal: do not leave port.sh and its compilers running
            kill_tree(process)
            process.wait()
            raise
        pumper.join(10)
    return code, steps, time.time() - started


def ensure_tools():
    tools = os.path.join(ROOT, 'build', 'tools')
    if os.path.exists(os.path.join(tools, 'build.ninja')):  # configured before: ninja rebuilds what recomp/ changed, about 1 s when nothing did
        build = ['ninja', '-C', tools] if shutil.which('ninja') else ['cmake', '--build', tools, '--target']
        if subprocess.run(build + ['eerecomp', 'vurecomp'], cwd=ROOT).returncode != 0:
            die('building the translator tools failed')
        return
    say('building the translator tools first (scripts/setup.sh --native-only)')
    if subprocess.run([find_bash(), os.path.join(ROOT, 'scripts', 'setup.sh'), '--native-only'], cwd=ROOT).returncode != 0:
        die('scripts/setup.sh failed')


def git_state():
    def git(*args):
        try:
            return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True, timeout=30).stdout.strip()
        except Exception:
            return ''
    rev = git('rev-parse', '--short', 'HEAD')
    dirty = bool(git('status', '--porcelain', '--', 'runtime', 'host', 'recomp', 'CMakeLists.txt'))
    return rev + ('+local' if dirty else '')


# ---------------------------------------------------------------- reading logs

def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1')
    return text.replace('\r\n', '\n').replace('\r', '\n').split('\n')


def scan_runtime(lines, examples=None):
    """Count what a runner log complains about: ({syscall: n}, {IOP call: n}, {warning: n}, halt reason, frames run).

    `examples`, when given, is a dict that gets the first real line of every counted item, under ('syscall' | 'iop' |
    'warning', key): the keys have their numbers blanked out, the example line keeps them."""
    syscalls, missing, warnings = {}, {}, {}
    halted = stopped = None
    for line in lines:
        match = SYSCALL.match(line)
        if match:
            key = match.group(1).lower().replace('0x', '0x', 1)
            key = '0x%02X' % int(key, 16)
            syscalls[key] = syscalls.get(key, 0) + 1
            if examples is not None:
                examples.setdefault(('syscall', key), line.strip())
            continue
        match = IOP_MISSING.match(line)
        if match:
            key = '%s #%s' % (match.group(1), match.group(2))
            missing[key] = missing.get(key, 0) + 1
            if examples is not None:
                examples.setdefault(('iop', key), line.strip())
            continue
        if line.startswith('[ee] halted: '):
            halted = line[len('[ee] halted: '):].strip()
        elif line.startswith('[ee] stopped after '):
            stopped = line
        elif WARNING.match(line):
            key = HEX.sub('N', line.strip())[:140]
            warnings[key] = warnings.get(key, 0) + 1
            if examples is not None:
                examples.setdefault(('warning', key), line.strip()[:200])
    frames = None
    if stopped:
        match = re.match(r'\[ee\] stopped after (\d+) frames', stopped)
        frames = int(match.group(1)) if match else None
    return syscalls, missing, warnings, halted, frames


def fail_note(log_path):
    """The most telling line of a failed port.sh log."""
    lines = [l.strip() for l in read_lines(log_path) if l.strip()]
    for line in reversed(lines):
        if 'error' in line.lower() or line.startswith('FAILED'):
            return line[:160]
    return lines[-1][:160] if lines else ''
