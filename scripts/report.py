#!/usr/bin/env python3
"""Compatibility report for one disc image: port it, run it, and say in plain words how far it got.

    python scripts/report.py <name> <image> [options]

Runs `scripts/port.sh <name> <image> --native-only` (the options --frames, --press, --rounds, --pnach, --pnach-section and
--widescreen go to it), then reads what the run left behind and writes two files next to the game's work files:

    games/<name>/report.md     the report, for a person
    games/<name>/report.json   the same facts for a script (and the base of the next report's "what changed")

Python 3 standard library only. Everything it writes goes into the git-ignored games/<name>/ folder: a report about a game
holds game-derived details and is never committed. docs/report.md explains the report.
"""
import argparse
import collections
import json
import os
import re
import shutil
import sys
import time
import zlib

import ps2tools
from ps2tools import HEX, IOP_MISSING, ROOT, STEPS, SYSCALL, WARNING, ensure_tools, fail_note, git_state, read_lines, say, scan_runtime

ps2tools.set_program('report')

FORMAT = 1
NAME_OK = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,60}$')
DEFAULT_FRAMES = 3600  # what port.sh uses when it is not told; the report always passes it so that it knows
DEFAULT_TIMEOUT = 3600
HISTORY = 12  # earlier reports kept in report.json
SHOT_EVERY = 300  # port.sh asks the runner for a screenshot every 300 frames

# What each port.sh step does, in words for the report.
STEP_PLAIN = {1: 'read the disc image', 2: 'get the program ready', 3: 'translate its machine code to C++',
              4: 'explore: build the runner and run the game headless', 5: 'compile the vector unit programs', 6: 'final build'}

# The same, short enough for a headline.
STEP_SHORT = {1: 'reading the disc image', 2: 'getting the program ready', 3: 'translating the code', 4: 'building and running the game',
              5: 'compiling the vector unit programs', 6: 'the final build'}

# The files port.sh makes in games/<name>/ (--fresh deletes exactly these, never anything else).
PORT_FILES = ('disc', 'disc.iso', 'vu1', 'shots', 'overlays', 'patched', 'modules', 'entries.txt', 'misses.txt', 'explore.log',
              'explore.ps2', 'explore_ram.bin', 'patches.txt', 'core.elf')

# Lines the runner prints that the shared WARNING pattern of ps2tools does not catch but that say something is not done.
EXTRA_WARNING = re.compile(r'^\[(?!sys\] unhandled syscall|iop\] missing)[a-z0-9_]+\] .*(not supported|ignored|not emulated|not handled|no handler|'
                           r'timed out|unmapped|never ends|\berror\b|no adapter|map failed|\btrap\b|\bbreak at\b|unavailable|'
                           r'blocking call outside|inside an interrupt handler|no translated code|stopped at|bad entry)', re.I)
STOP_LINE = re.compile(r'^\[ee\] stopped at (0x[0-9A-Fa-f]+): (.*)')
STOPPED_AFTER = re.compile(r'^\[ee\] stopped after (\d+) frames, (\d+) cycles: ?(.*)')
NO_CODE = re.compile(r'^\[ee\] no translated code at (0x[0-9A-Fa-f]+)')
PRIMITIVES = re.compile(r'^\[gs\] (\d+) primitives drawn')
RUNNER_EXIT = re.compile(r'^\[port\] ps2run exited with code (\d+)')
ROUND = re.compile(r'^round (\d+): (.*), (\d+) new entry points, (\d+) new window images')

# Which kind of warning a line is, by the tag in front of it.
TAG_GROUPS = {'ee': 'code', 'gs': 'graphics', 'gpu': 'graphics', 'gif': 'graphics', 'vif': 'graphics', 'vu': 'graphics', 'vu1': 'graphics', 'jit': 'graphics',
              'spu2': 'sound_video', 'ipu': 'sound_video',
              'cdvd': 'disc_files', 'cdvdfsv': 'disc_files', 'disc': 'disc_files', 'io': 'disc_files', 'fileio': 'disc_files',
              'loadfile': 'disc_files', 'mc': 'disc_files'}
MISSING_TITLES = [('syscalls', 'System calls the runner does not handle',
                   'The game asked the console\'s kernel for something the runner does not do yet.'),
                  ('iop_calls', 'IOP library calls that are missing',
                   'A module on the console\'s second processor (sound, disc, pad, memory card) called a function the runner does not have.'),
                  ('code', "The game's own code",
                   'The runner reached code the translator did not provide or could not handle.'),
                  ('graphics', 'Graphics and vector units', 'Something the graphics side could not do.'),
                  ('sound_video', 'Sound and video', 'Something the sound or video decoder could not do.'),
                  ('disc_files', 'Disc, files and memory card', 'The game asked for a disc, file or memory card feature that is not there.'),
                  ('other', 'Other warnings', 'Everything else the runner complained about.')]

# ---------------------------------------------------------------- the picture: reading screenshots


def read_ppm(path):
    """(width, height, RGB bytes) of a binary PPM, or None when the file is not one."""
    try:
        with open(path, 'rb') as f:
            raw = f.read()
    except OSError:
        return None
    if raw[:2] != b'P6':
        return None
    pos, values = 2, []
    while len(values) < 3:
        while pos < len(raw) and raw[pos:pos + 1].isspace():
            pos += 1
        if raw[pos:pos + 1] == b'#':
            pos = raw.find(b'\n', pos)
            if pos < 0:
                return None
            continue
        end = pos
        while end < len(raw) and raw[end:end + 1].isdigit():
            end += 1
        if end == pos:
            return None
        values.append(int(raw[pos:end]))
        pos = end
    pos += 1  # the one whitespace byte after the maximum value
    width, height, maxval = values
    if maxval != 255 or width <= 0 or height <= 0 or len(raw) - pos < width * height * 3:
        return None
    return width, height, raw[pos:pos + width * height * 3]


def write_png(path, width, height, rgb):
    """A plain RGB PNG, so that the pictures open in any viewer (Windows has none for PPM)."""
    def chunk(kind, body):
        data = kind + body
        return len(body).to_bytes(4, 'big') + data + (zlib.crc32(data) & 0xFFFFFFFF).to_bytes(4, 'big')
    stride = width * 3
    scan = b''.join(b'\0' + rgb[y * stride:(y + 1) * stride] for y in range(height))
    header = width.to_bytes(4, 'big') + height.to_bytes(4, 'big') + bytes([8, 2, 0, 0, 0])
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(scan, 6)) + chunk(b'IEND', b''))


def quantize(rgb):
    """One small number per pixel (5 bits per channel), so that equal-looking pixels are equal and counting is cheap."""
    return [((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3) for r, g, b in zip(rgb[0::3], rgb[1::3], rgb[2::3])]


def lit(key):
    """A pixel counts as lit when one of its channels is at least 24 of 255."""
    return (key >> 10) >= 3 or ((key >> 5) & 31) >= 3 or (key & 31) >= 3


LIT_BLACK_PCT = 0.2  # a picture with fewer lit pixels than this is black (1 pixel in 500)
FLAT_PCT = 99.8      # one colour on this much of the picture is flat
CHANGED_PCT = 1.0    # two shots differ clearly when this many percent of their pixels differ
STILL_PCT = 0.1      # and are the same picture below this


def picture_stats(width, height, quantized):
    counts = collections.Counter(quantized)
    total = len(quantized)
    lit_pixels = sum(count for key, count in counts.items() if lit(key))
    return {'size': '%dx%d' % (width, height), 'lit_pct': round(100.0 * lit_pixels / total, 2), 'colors': len(counts),
            'top_color_pct': round(100.0 * max(counts.values()) / total, 2)}


def collect_shots(shots_dir, since, last_frame):
    """[(frame, path)] of the screenshots the run made: newer than `since`, and not past the frame the last run stopped at
    (an earlier exploring round that ran longer leaves such files behind)."""
    found = []
    if os.path.isdir(shots_dir):
        for name in os.listdir(shots_dir):
            match = re.fullmatch(r'frame_(\d+)\.ppm', name)
            path = os.path.join(shots_dir, name)
            if not match or os.path.getmtime(path) < since - 2:
                continue
            frame = int(match.group(1))
            if last_frame is None or frame <= last_frame:
                found.append((frame, path))
    return sorted(found)


def analyse_pictures(shots, pictures_dir=None):
    """Look at the screenshots in frame order. Returns the 'picture' part of the report."""
    entries, previous = [], None
    if pictures_dir and os.path.isdir(pictures_dir):
        for name in os.listdir(pictures_dir):  # the pictures of the report before; nothing else lives there
            if re.fullmatch(r'frame_\d+\.png', name):
                os.remove(os.path.join(pictures_dir, name))
    for frame, path in shots:
        image = read_ppm(path)
        if image is None:
            continue
        width, height, rgb = image
        q = quantize(rgb)
        entry = dict(frame=frame, **picture_stats(width, height, q))
        if previous is not None:
            if previous[0] != len(q):
                entry['change_pct'] = 100.0
            else:
                entry['change_pct'] = round(100.0 * sum(1 for a, b in zip(previous[1], q) if a != b) / len(q), 2)
        previous = (len(q), q)
        if pictures_dir:
            try:
                os.makedirs(pictures_dir, exist_ok=True)
                write_png(os.path.join(pictures_dir, 'frame_%05d.png' % frame), width, height, rgb)
                entry['png'] = 'frame_%05d.png' % frame
            except (OSError, zlib.error):
                pass
        entries.append(entry)
    return dict(judge_pictures(entries), shots=entries)


def judge_pictures(entries):
    """The rule, in order: no shot = none; every shot black = black; every shot one flat colour = flat; one shot = cannot tell;
    the last two shots black after a picture = went-black; then by how the picture changes from shot to shot (a clear change is
    more than CHANGED_PCT percent of the pixels, the same picture is less than STILL_PCT): never = frozen, only tiny = almost-still,
    clear at first and not at the end = froze, else moving. docs/report.md explains it in words; keep the two in step."""
    if not entries:
        return dict(verdict='none', text='No screenshot was written, so nothing was ever shown.', pairs=0, changing_pairs=0, froze_at=None)
    if all(e['top_color_pct'] >= FLAT_PCT for e in entries):
        if all(e['lit_pct'] < LIT_BLACK_PCT for e in entries):
            return dict(verdict='black', text='Every screenshot is black.', pairs=len(entries) - 1, changing_pairs=0, froze_at=None)
        return dict(verdict='flat', text='Every screenshot is one flat colour: nothing but the background is drawn.', pairs=len(entries) - 1,
                    changing_pairs=0, froze_at=None)
    changes = [e['change_pct'] for e in entries[1:]]
    changing = sum(1 for c in changes if c > CHANGED_PCT)
    info = dict(pairs=len(changes), changing_pairs=changing, froze_at=None)
    if not changes:
        return dict(info, verdict='single', text='Only one screenshot was taken, so it cannot be said whether the picture moves. It shows a picture.')
    dark = 0
    for e in reversed(entries):
        if e['lit_pct'] >= LIT_BLACK_PCT or e['top_color_pct'] < FLAT_PCT:
            break
        dark += 1
    if dark >= 2:
        frame = entries[len(entries) - dark]['frame']
        return dict(info, verdict='went-black', froze_at=frame, text='The game showed a picture, then the screen went black and stayed black from frame %d on.' % frame)
    if changing == 0:
        if all(c <= STILL_PCT for c in changes):
            return dict(info, verdict='frozen', text='The picture does not move: all %d screenshots are the same.' % len(entries))
        return dict(info, verdict='almost-still', text='The picture hardly moves: the screenshots differ only in tiny details (a blinking cursor, a clock).')
    tail = 0
    for c in reversed(changes):
        if c > STILL_PCT:
            break
        tail += 1
    if tail >= 2:
        frame = entries[len(entries) - 1 - tail]['frame']
        return dict(info, verdict='froze', froze_at=frame, text='The picture moved at first, then froze: it is the same from frame %d on.' % frame)
    return dict(info, verdict='moving', text='The picture moves: %d of %d screenshot pairs differ clearly.' % (changing, len(changes)))


# ---------------------------------------------------------------- the run: reading the logs


def parse_port_log(lines):
    """What port.sh itself printed: text per step, boot program, exploring rounds, the line to play with."""
    info = dict(steps_text={}, boots=None, crc=None, units=None, rounds=[], play=None, patch_notes=[], windows=None, vu_line=None)
    step = 0
    for raw in lines:
        line = raw.rstrip()
        match = re.match(r'== \[(\d)/6\]', line)
        if match:
            step = int(match.group(1))
            continue
        if not line:
            continue
        if step:
            info['steps_text'].setdefault(step, []).append(line)
        if line.startswith('boots '):
            info['boots'] = line[6:].strip()
        match = re.match(r'PCSX2 patch files for it end in _([0-9A-Fa-f]+)\.pnach', line)
        if match:
            info['crc'] = match.group(1).upper()
        match = re.match(r'(\d+) units written', line)
        if match:
            info['units'] = int(match.group(1))
        match = ROUND.match(line)
        if match:
            info['rounds'].append(dict(n=int(match.group(1)), ended=match.group(2), new_entry_points=int(match.group(3)), new_window_images=int(match.group(4))))
        if line.startswith('Play it natively:'):
            info['play'] = line.split(':', 1)[1].strip()
        if re.match(r'(\d+ patches, |applied \[|left out \[|no section named |warning: .*pnach|skipped |new code at |the executable grew)', line):
            info['patch_notes'].append(line)
        if line.startswith('code windows the game rewrites:'):
            info['windows'] = line.split(':', 1)[1].strip()
    if 5 in info['steps_text']:
        info['vu_line'] = info['steps_text'][5][-1]
    return info


def failure_line(log_path, lines):
    """The line that says why port.sh failed: its last line, except after a failed compile, where the error is further up."""
    tail = [line.strip() for line in lines if line.strip()]
    if not tail:
        return ''
    if tail[-1].startswith(('ninja:', 'FAILED', 'make')):
        return fail_note(log_path)
    return tail[-1][:200]


def group_of(key):
    match = re.match(r'\[([a-z0-9_]+)\]', key)
    return TAG_GROUPS.get(match.group(1), 'other') if match else 'other'


def ranked(counts, examples, kind):
    rows = [dict(what=key, count=count, example=examples.get((kind, key), key)) for key, count in counts.items()]
    return sorted(rows, key=lambda r: (-r['count'], r['what']))


def scan_run(lines):
    """Everything the exploring run's log says about the game, deduplicated, with counts, most frequent first."""
    examples = {}
    syscalls, iop, warnings, halted, frames = scan_runtime(lines, examples)
    extra = {}
    stop = stopped = no_code = primitives = runner_exit = None
    for line in lines:
        if WARNING.match(line) or SYSCALL.match(line) or IOP_MISSING.match(line):
            continue
        if EXTRA_WARNING.match(line):
            key = HEX.sub('N', line.strip())[:140]
            extra[key] = extra.get(key, 0) + 1
            examples.setdefault(('warning', key), line.strip()[:200])
    for line in lines:
        match = STOP_LINE.match(line)
        if match and stop is None:
            stop = dict(address=match.group(1), what=match.group(2).strip())
        match = STOPPED_AFTER.match(line)
        if match:
            stopped = dict(frames=int(match.group(1)), cycles=int(match.group(2)), reason=match.group(3).strip())
        match = NO_CODE.match(line)
        if match and no_code is None:
            no_code = match.group(1)
        match = PRIMITIVES.match(line)
        if match:
            primitives = int(match.group(1))
        match = RUNNER_EXIT.match(line)
        if match:
            runner_exit = int(match.group(1))
    for key, count in extra.items():
        warnings[key] = warnings.get(key, 0) + count
    missing = {'syscalls': ranked(syscalls, examples, 'syscall'), 'iop_calls': ranked(iop, examples, 'iop')}
    grouped = collections.defaultdict(dict)
    for key, count in warnings.items():
        grouped[group_of(key)][key] = count
    for group in ('code', 'graphics', 'sound_video', 'disc_files', 'other'):
        missing[group] = ranked(grouped.get(group, {}), examples, 'warning')
    return dict(missing=missing, halted=halted, frames=frames if stopped else None, cycles=stopped['cycles'] if stopped else None,
                stopped_line=stopped is not None, stop=stop, no_code=no_code, primitives=primitives, runner_exit=runner_exit)


def halt_kind(halted, has_stop_line, timed_out=False):
    """A short name for why the run ended, for the headline and for comparing two reports."""
    if not has_stop_line:
        return 'running' if timed_out else 'crash'
    text = halted or ''
    if text == 'frame limit reached':
        return 'frame-limit'
    if text == 'Exit':
        return 'exit'
    if text.startswith('deadlock'):
        return 'deadlock'
    if text.startswith('missing code'):
        return 'missing-code'
    if text.startswith('program asked to run another executable'):
        return 'exec'
    if text == 'bad entry':
        return 'bad-entry'
    return 'stop' if text else 'other'


HALT_PLAIN = {
    'frame-limit': 'It did not stop by itself: the run ended at the frame limit. Nothing went wrong.',
    'exit': 'The program ended by itself (it called Exit). A test program does that when it is done; a game should not do it early.',
    'deadlock': 'The game got stuck: every thread waits for something that never happens (a deadlock). Often a missing piece of the console that '
                'the game waits for, such as an interrupt or an answer from the disc.',
    'missing-code': 'The game jumped to code that is not in the translated program. Usually that is code the game loads from the disc later or '
                    'writes itself; running longer, pressing other buttons (--press) or allowing more rounds (--rounds) may find it.',
    'exec': 'The program asked to start another executable (ExecPS2 or LoadExecPS2). The runner cannot switch programs yet.',
    'bad-entry': 'Internal error in the translated code (a jump into the middle of a translated unit).',
    'stop': 'The translator could not handle an instruction or pattern here, so the game was stopped. It needs a fix in the recompiler.',
    'crash': 'The runner ended without its normal stop message. It most likely crashed or was killed.',
    'running': 'The run had not ended when the time limit was reached (the game was still running or stuck in a loop) and port.sh was killed.',
    'other': 'The run ended for a reason the report does not know.',
}


def gather_run(game, started, timed_out):
    """Facts about the exploring run, from games/<name>/explore.log (ignored when that file is from an earlier run)."""
    log = os.path.join(game, 'explore.log')
    fresh = os.path.exists(log) and os.path.getmtime(log) >= started - 2
    if not fresh:
        return dict(ran=False)
    scan = scan_run(read_lines(log))
    stopped = scan['stopped_line']
    runner_exit = scan['runner_exit']  # port.sh adds this line to the log only when the exit code was not 0
    kind = halt_kind(scan['halted'], stopped, timed_out)
    run = dict(ran=True, frames=scan['frames'], cycles=scan['cycles'], halt=scan['halted'], halt_kind=kind, runner_exit=runner_exit,
               primitives=scan['primitives'], stop=scan['stop'], no_code=scan['no_code'], missing=scan['missing'])
    return run


# ---------------------------------------------------------------- putting the facts together


def settings_of(options):
    return dict(frames=options.frames, rounds=options.rounds, press=list(options.press), pnach=[os.path.basename(p) for p in options.pnach],
                pnach_sections=list(options.pnach_section), widescreen=bool(options.widescreen), timeout=options.timeout)


def step_table(steps, seconds, completed, timed_out):
    """[{n, name, seconds, result}] for the steps port.sh started, with how long each took."""
    rows = []
    for i, (n, start) in enumerate(steps):
        end = steps[i + 1][1] if i + 1 < len(steps) else seconds
        last = i + 1 == len(steps)
        result = 'done'
        if last and not completed:
            result = 'timed out' if timed_out else 'failed'
        rows.append(dict(n=n, name=STEPS.get(n, str(n)), plain=STEP_PLAIN.get(n, ''), seconds=round(end - start, 1), result=result))
    return rows


def round_times(steps, rounds_at, seconds):
    """Seconds per exploring round (build, translate again, run), from when each 'round N:' line appeared."""
    begin = next((t for n, t in steps if n == 4), None)
    times, previous = [], begin
    for n, at in rounds_at:
        if previous is not None:
            times.append(dict(n=n, seconds=round(at - previous, 1)))
        previous = at
    return times


def judge(r):
    """The one-line verdict: (level, headline)."""
    port, run, pic = r['port'], r['run'], r['picture']
    if not port['completed']:
        n = port['failed_step'] or 0
        where = 'step %d of 6, %s' % (n, STEP_SHORT.get(n, '')) if n else 'the start'
        if port['timed_out']:
            return 'timeout', 'Took too long: no result after %s (it was in %s).' % (nice_seconds(r['settings']['timeout']), where)
        return 'build-failed', 'Could not be ported: it failed in %s.' % where
    kind = run.get('halt_kind') if run.get('ran') else None
    frames = run.get('frames')
    if kind is None:
        return 'build-failed', 'Ported, but the game was not run (no run log was found).'
    after = ' after %d frames' % frames if frames is not None else ''
    if frames is None and run.get('last_shot_frame') is not None:
        after = ' (its last screenshot is from frame %d)' % run['last_shot_frame']
    if kind in ('crash', 'running', 'missing-code', 'stop', 'deadlock', 'exec', 'bad-entry', 'other'):
        reasons = {'crash': 'the runner crashed', 'running': 'it never ended', 'missing-code': 'it jumped to code that was not translated', 'stop': 'the translator hit something it cannot do',
                   'deadlock': 'every thread got stuck waiting', 'exec': 'it asked to start another executable', 'bad-entry': 'internal error',
                   'other': run.get('halt') or 'unknown reason'}
        return 'stops-early', 'Stops early%s: %s.' % (after, reasons[kind])
    if kind == 'exit':
        return 'ended', 'The program ended by itself%s (it called Exit). %s' % (after, PICTURE_SHORT.get(pic['verdict'], ''))
    verdict = pic['verdict']
    ran = 'Runs the whole %d frames' % frames if frames is not None else 'Runs without stopping'
    if verdict in ('moving',):
        return 'moving', '%s and the picture moves.' % ran
    if verdict in ('froze', 'frozen', 'almost-still'):
        return 'frozen', '%s, but the picture %s.' % (ran, {'froze': 'freezes at frame %s' % pic.get('froze_at'), 'frozen': 'does not move',
                                                           'almost-still': 'hardly moves'}[verdict])
    if verdict == 'single':
        return 'shows-picture', '%s and shows a picture (only one screenshot, so movement is not judged).' % ran
    return 'no-picture', '%s, but %s.' % (ran, {'black': 'the screen stays black', 'flat': 'the screen shows one flat colour', 'none': 'no picture was ever shown',
                                                 'too-short': 'it is too short for a screenshot (the first one is at frame %d)' % SHOT_EVERY,
                                                 'went-black': 'the screen goes black at frame %s' % pic.get('froze_at')}[verdict])


PICTURE_SHORT = {'moving': 'The picture moved.', 'froze': 'The picture froze.', 'frozen': 'The picture did not move.', 'black': 'The screen stayed black.',
                 'flat': 'The screen showed one flat colour.', 'none': 'No picture was shown.', 'almost-still': 'The picture hardly moved.',
                 'single': 'It showed a picture.', 'went-black': 'The screen went black.',
                 'too-short': 'The run was too short for a screenshot.'}


PICTURE_WORDS = {'moving': 'it moves', 'froze': 'it moved, then froze', 'frozen': 'it does not move', 'black': 'the screen is black',
                 'flat': 'the screen is one flat colour', 'none': 'no picture is shown', 'almost-still': 'it hardly moves', 'single': 'there is only one screenshot', 'went-black': 'it went black',
                 'too-short': 'the run is too short for a screenshot'}


def nice_seconds(s):
    s = float(s)
    if s < 10:
        return '%.1f s' % s
    if s < 90:
        return '%.0f s' % s
    if s < 5400:
        return '%d min %02d s' % (s // 60, s % 60)
    return '%d h %02d min' % (s // 3600, (s % 3600) // 60)


def build_report(options, image, started, code, steps, rounds_at, seconds, log_path):
    game = os.path.join(ROOT, 'games', options.name)
    port_lines = read_lines(log_path)
    port = parse_port_log(port_lines)
    last_step = steps[-1][0] if steps else 0
    timed_out = code is None
    completed = code == 0 and last_step == 6 and port['play'] is not None
    run = gather_run(game, started, timed_out)
    shots = collect_shots(os.path.join(game, 'shots'), started, run.get('frames')) if run.get('ran') else []
    nothing = dict(verdict='none', pairs=0, changing_pairs=0, froze_at=None, shots=[])
    if run.get('ran'):
        try:
            picture = analyse_pictures(shots, os.path.join(game, 'report-pictures'))
        except (OSError, ValueError, MemoryError, zlib.error) as problem:  # a broken screenshot must not cost the whole report
            picture = dict(nothing, text='The screenshots could not be read (%s).' % problem)
    else:
        picture = dict(nothing, text='The game was not run, so there is no picture.')
    if run.get('ran') and run.get('frames') is None and shots:
        run['last_shot_frame'] = shots[-1][0]
    if picture['verdict'] == 'none' and run.get('frames') is not None and run['frames'] < SHOT_EVERY:
        picture.update(verdict='too-short', text='No screenshot was taken: the first one comes at frame %d and the run ended at frame %d.' % (SHOT_EVERY, run['frames']))
    failure = None
    if not completed:
        failure = dict(line=failure_line(log_path, port_lines) if not timed_out else '', tail=[l.strip() for l in port_lines if l.strip()][-8:])
    rounds = port['rounds']
    for item, timing in zip(rounds, round_times(steps, rounds_at, seconds)):
        item['seconds'] = timing['seconds']
    settled = bool(rounds) and rounds[-1]['new_entry_points'] == 0 and rounds[-1]['new_window_images'] == 0
    try:
        size = os.path.getsize(image)
    except OSError:
        size = None
    report = dict(
        format=FORMAT, name=options.name, created=time.strftime('%Y-%m-%d %H:%M:%S'), label=options.label or '', project=git_state(),
        image=dict(file=os.path.basename(image), size=size, boots=port['boots'], pcsx2_crc=port['crc']),
        settings=settings_of(options),
        port=dict(completed=completed, exit_code=code, timed_out=timed_out, seconds=round(seconds, 1), step_reached=last_step,
                  failed_step=None if completed else last_step, steps=step_table(steps, seconds, completed, timed_out),
                  rounds=rounds, settled=settled, units=port['units'], vu_line=port['vu_line'], code_windows=port['windows'],
                  patch_notes=port['patch_notes'], play=port['play'], failure=failure,
                  log=rel(log_path)),
        run=run, picture=picture)
    report['verdict'] = dict(zip(('level', 'headline'), judge(report)))
    return report


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


# ---------------------------------------------------------------- what changed since the last report


def picture_rank(verdict):
    return {'moving': 3, 'froze': 2, 'frozen': 1, 'almost-still': 1, 'single': 1, 'went-black': 1}.get(verdict, 0)


def outcome_rank(r):
    if not r['port']['completed']:
        return 1 if r['port']['timed_out'] else 0
    kind = r['run'].get('halt_kind')
    if kind in ('frame-limit', 'exit'):
        return 4
    if kind == 'crash':
        return 2
    return 3 if kind else 1


def compare(prev, cur):
    """[{kind: better | worse | note, text}] of how this report differs from the one before."""
    changes = []

    def add(kind, text):
        changes.append(dict(kind=kind, text=text))

    if prev.get('format') != FORMAT:
        return [dict(kind='note', text='The earlier report has an older layout, so nothing is compared.')]
    before, after = dict(prev['settings'], timeout=None), dict(cur['settings'], timeout=None)  # the time limit does not change what a run does
    same_settings = before == after
    if not same_settings:
        parts = ['%s: %s, was %s' % (SETTING_NAMES.get(k, k), setting_text(k, after.get(k)), setting_text(k, before.get(k))) for k in sorted(after)
                 if before.get(k) != after.get(k)]
        add('note', 'The settings are not the same as last time (%s), so the comparison is not like for like.' % '; '.join(parts))
    if prev.get('project') != cur.get('project'):
        add('note', 'The project itself changed since last time (version %s, was %s).' % (cur.get('project'), prev.get('project')))
    if prev['image'].get('size') != cur['image'].get('size') or prev['image'].get('file') != cur['image'].get('file'):
        add('note', 'The disc image is a different file than last time (%s, %s bytes before; %s, %s bytes now).' % (
            prev['image'].get('file'), prev['image'].get('size'), cur['image'].get('file'), cur['image'].get('size')))
    # overall, then the steps
    pa, pb = prev['port'], cur['port']
    if pa['completed'] and not pb['completed']:
        add('worse', 'It no longer gets through the port: it fails in step %s (%s); last time it did all 6 steps.' % (pb['failed_step'], STEPS.get(pb['failed_step'], '?')))
    elif pb['completed'] and not pa['completed']:
        add('better', 'The port goes through now (last time it failed in step %s, %s).' % (pa['failed_step'], STEPS.get(pa['failed_step'], '?')))
    elif not pa['completed'] and not pb['completed'] and pa['failed_step'] != pb['failed_step']:
        later = (pb['failed_step'] or 0) > (pa['failed_step'] or 0)
        add('better' if later else 'worse', 'It now fails in step %s (%s); last time in step %s (%s).' % (
            pb['failed_step'], STEPS.get(pb['failed_step'], '?'), pa['failed_step'], STEPS.get(pa['failed_step'], '?')))
    ra, rb = prev['run'], cur['run']
    if ra.get('ran') and rb.get('ran'):
        if ra.get('halt_kind') != rb.get('halt_kind') or ra.get('halt') != rb.get('halt'):
            kind = 'better' if outcome_rank(cur) > outcome_rank(prev) else 'worse' if outcome_rank(cur) < outcome_rank(prev) else 'note'
            add(kind, 'It stops for a different reason: "%s" (frame %s), last time "%s" (frame %s).' % (
                rb.get('halt') or 'no stop message', rb.get('frames'), ra.get('halt') or 'no stop message', ra.get('frames')))
        elif ra.get('frames') != rb.get('frames') and ra.get('halt_kind') not in ('frame-limit',) and None not in (ra.get('frames'), rb.get('frames')):
            add('better' if rb['frames'] > ra['frames'] else 'worse', 'It stops at frame %d, last time at frame %d (same reason).' % (rb['frames'], ra['frames']))
        elif ra.get('frames') != rb.get('frames') and same_settings:
            add('note', 'It ran %s frames, last time %s.' % (rb.get('frames'), ra.get('frames')))
    elif rb.get('ran') and not ra.get('ran'):
        add('better', 'The game was run this time; last time it never got that far.')
    elif ra.get('ran') and not rb.get('ran'):
        add('worse', 'The game was not run this time; last time it was.')
    va, vb = prev['picture']['verdict'], cur['picture']['verdict']
    if va != vb:
        ranks = (picture_rank(va), picture_rank(vb))
        kind = 'better' if ranks[1] > ranks[0] else 'worse' if ranks[1] < ranks[0] else 'note'
        add(kind, 'The picture is different: now %s, last time %s.' % (PICTURE_WORDS.get(vb, vb), PICTURE_WORDS.get(va, va)))
    if ra.get('ran') and rb.get('ran'):
        compare_missing(ra, rb, same_settings and ra.get('frames') == rb.get('frames'), add)
    ta, tb = pa['seconds'], pb['seconds']
    if abs(tb - ta) >= 10 and abs(tb - ta) >= 0.3 * max(ta, tb):
        add('note', 'It took %s, last time %s (timings vary with what else the PC is doing).' % (nice_seconds(tb), nice_seconds(ta)))
    return changes


SETTING_NAMES = {'frames': 'frames per run', 'rounds': 'rounds at most', 'press': 'buttons pressed', 'pnach': 'patch files',
                 'pnach_sections': 'patch sections', 'widescreen': 'widescreen'}


def setting_text(key, value):
    if isinstance(value, bool):
        return 'on' if value else 'off'
    if isinstance(value, list):
        return ', '.join(value) if value else 'the default' if key == 'press' else 'none'
    return 'the default' if value in (0, None) else str(value)


def times(n):
    return '%d time%s' % (n, '' if n == 1 else 's')


def compare_missing(ra, rb, same_length, add):
    """New and fixed items per list. Counts are only compared when both runs ran equally long."""
    for key, title, _ in MISSING_TITLES:
        before = {e['what']: e['count'] for e in ra['missing'].get(key, [])}
        after = {e['what']: e['count'] for e in rb['missing'].get(key, [])}
        for what in sorted(after, key=lambda w: -after[w]):
            if what not in before:
                add('worse', 'New in "%s": %s (%s).' % (title.lower(), what, times(after[what])))
        for what in sorted(before, key=lambda w: -before[w]):
            if what not in after:
                add('better', 'Gone from "%s": %s (was %s).' % (title.lower(), what, times(before[what])))
        if same_length:
            for what in sorted(after):
                if what in before and before[what] != after[what]:
                    add('better' if after[what] < before[what] else 'worse', '"%s": %s now %s, was %d.' % (title.lower(), what, times(after[what]), before[what]))


# ---------------------------------------------------------------- writing the report


def table(rows, header):
    out = ['| ' + ' | '.join(header) + ' |', '|' + '|'.join('---' for _ in header) + '|']
    out += ['| ' + ' | '.join(str(c) for c in row) + ' |' for row in rows]
    return out


def press_text(settings):
    if settings['press']:
        return ', '.join(settings['press'])
    return 'the default (start at frame 300, then cross every 300 frames)'


JUDGING_RULE = (
    'How this is judged: a pixel is "lit" when one of its colours is at least 24 of 255. A shot with fewer than 1 lit pixel in 500 is black, and one '
    'colour on %.1f %% of the picture is flat. Two shots "differ clearly" when more than %.0f %% of their pixels differ, and are the same picture '
    'below %.1f %%. Then: every shot black or flat = black or flat; the last two shots black after a picture = went black; shots that never differ = '
    'frozen; only tiny differences = hardly moves; differing at first but the same at the end = froze; anything else = moves. It is a simple rule: '
    'a game that waits at a menu looks frozen, and a screen that moves can still look wrong.' % (FLAT_PCT, CHANGED_PCT, STILL_PCT))


def exit_code_text(run):
    code = run.get('runner_exit')
    if run['halt_kind'] == 'running':
        return 'none (it was still running when port.sh was killed)'
    if run['halt_kind'] == 'crash':
        return '%s (the runner did not end normally)' % ('unknown' if code is None else code)
    return '0 (normal)' if not code else '%s (not 0: the runner did not end normally)' % code


def rounds_text(run, port):
    """What the exploring rounds say about how complete the translation is."""
    rounds = port['rounds']
    if not rounds:
        return None
    last = rounds[-1]
    if last['new_entry_points'] or last['new_window_images']:
        text = ('The last round still found %d new entry points, so the exploring stopped before the translation settled. More rounds '
                '(--rounds) or more frames may get further.' % last['new_entry_points'])
    elif run['halt_kind'] == 'missing-code':
        text = 'The last round found nothing new to add, yet the game still ends at code that is not translated: the translator could not add it.'
    else:
        text = 'The last round found no new code to translate, so the translation settled.'
    return text


def render_run(r):
    port, run = r['port'], r['run']
    out = ['### The game itself', '']
    if run.get('frames') is not None:
        out.append('- Frames run: **%d** (asked for %s).' % (run['frames'], r['settings']['frames']))
    elif run.get('last_shot_frame') is not None:
        out.append('- Frames run: not known (there is no stop message); the last screenshot is from frame %d.' % run['last_shot_frame'])
    else:
        out.append('- Frames run: not known (there is no stop message).')
    out.append('- Why it stopped: %s' % HALT_PLAIN.get(run['halt_kind'], ''))
    if run.get('halt'):
        out.append('  - The runner says: `%s`' % run['halt'])
    if run.get('stop'):
        out.append('  - The translator gave up at `%s`: %s' % (run['stop']['address'], run['stop']['what']))
    out.append("- The runner's exit code: %s." % exit_code_text(run))
    text = rounds_text(run, port)
    if text:
        out.append('- Exploring rounds: %d. %s' % (len(port['rounds']), text))
        out.append('  - Each round builds the runner, runs the game and looks for code that was missed: ' + ', '.join(
            '%d: %s%s' % (x['n'], x['ended'], ' (%s)' % nice_seconds(x['seconds']) if 'seconds' in x else '') for x in port['rounds']) + '.')
    if run.get('primitives') is not None:
        out.append('- The graphics chip drew %d shapes (triangles, sprites, lines) in the whole run.' % run['primitives'])
    return out


def render_steps(r):
    """Part 1, first half: the six port.sh steps, and what went wrong when one failed."""
    port = r['port']
    rows = []
    shown = {s['n']: s for s in port['steps']}
    for n in range(1, 7):
        s = shown.get(n)
        rows.append(('%d/6' % n, STEP_PLAIN[n], s['result'] if s else 'not reached', nice_seconds(s['seconds']) if s else ''))
    out = table(rows, ['Step', 'What it does', 'Result', 'Time'])
    out.append('')
    out.append('Whole port: %s, %s.' % (nice_seconds(port['seconds']), 'killed at the %s limit' % nice_seconds(r['settings']['timeout']) if port['timed_out']
                                       else 'exit code %s' % port['exit_code']))
    if r['image'].get('boots'):
        crc = ' (PCSX2 patch files for it end in _%s.pnach)' % r['image']['pcsx2_crc'] if r['image'].get('pcsx2_crc') else ''
        out.append('Disc: %s boots `%s`%s.' % (r['image']['file'], r['image']['boots'], crc))
    if port['units']:
        out.append('Translated into %d C++ units%s.' % (port['units'], '; vector units: ' + port['vu_line'] if port['vu_line'] else ''))
    if port['code_windows']:
        out.append('Code the game rewrites while it runs (translated separately): %s.' % port['code_windows'])
    if port['patch_notes']:
        out.append('Patches: %s.' % '; '.join(port['patch_notes']))
    if not port['completed']:
        failure = port['failure'] or {}
        out.append('')
        out.append('**Timed out.** port.sh was still working when the limit was reached.' if port['timed_out'] else '**What went wrong:**')
        if failure.get('line') and not port['timed_out']:
            out.append('')
            out.append('Last error line: `%s`' % failure['line'])
        if failure.get('tail'):
            out.append('')
            out.append('The last lines of the log:')
            out.append('')
            out.extend('    ' + line for line in failure['tail'])
        out.append('')
        out.append('Whole log: `%s`' % os.path.join(ROOT, port['log'].replace('/', os.sep)))
    return out


def render_pictures(pic, game):
    rows = [(e['frame'], e['size'], '%.1f %%' % e['lit_pct'], e['colors'], '' if 'change_pct' not in e else '%.1f %%' % e['change_pct']) for e in pic['shots']]
    out = table(rows, ['Frame', 'Size', 'Lit pixels', 'Colours', 'Changed since the shot before'])
    out.append('')
    out.append('The shots are every %d frames. Open them as pictures in `%s`.' % (SHOT_EVERY, os.path.join(game, 'report-pictures')))
    out.append('')
    out.append(JUDGING_RULE)
    return out


def render_missing(run):
    out = []
    total = 0
    for key, title, explain in MISSING_TITLES:
        rows = run['missing'].get(key, [])
        total += len(rows)
        if not rows:
            continue
        out.append('**%s.** %s' % (title, explain))
        out.append('')
        for e in rows[:10]:
            extra = ''
            called = re.search(r'from (0x[0-9A-Fa-f]+)', e['example'])
            if key in ('syscalls', 'iop_calls') and called:
                extra = ' (first call from %s)' % called.group(1)
            elif key not in ('syscalls', 'iop_calls') and e['example'] != e['what']:
                extra = ' (for example `%s`)' % e['example']
            out.append('- `%s`: %s%s' % (e['what'], times(e['count']), extra))
        if len(rows) > 10:
            out.append('- ...and %d more (all of them are in report.json)' % (len(rows) - 10))
        out.append('')
    if not total:
        out.append('Nothing was reported. The runner only prints what it knows it does not handle, so silence does not prove that everything works.')
    else:
        out.append('Most frequent first. Counts depend on how long the game ran.')
    return out


def render_changes(r):
    if r['changes'] is None:
        return ['This is the first report for this game, so there is nothing to compare with yet. Run the same command again later (for example '
                'after the project got a fix) and this part lists what got better or worse.']
    before = r['compared_with']
    out = ['Compared with the report of %s%s.' % (before['created'], ' (%s)' % before['label'] if before['label'] else ''), '']
    if not r['changes']:
        out.append('Nothing changed.')
    for title, kind in (('Better', 'better'), ('Worse', 'worse'), ('Different, neither better nor worse', 'note')):
        items = [c for c in r['changes'] if c['kind'] == kind]
        if items:
            out.append('**%s:**' % title)
            out.append('')
            out.extend('- %s' % c['text'] for c in items)
            out.append('')
    return out


def render(r, game):
    """report.md, in plain words, the most important thing first."""
    port, run, pic, verdict, s = r['port'], r['run'], r['picture'], r['verdict'], r['settings']
    out = ['# Compatibility report: %s' % r['name'], '', '**Result: %s**' % verdict['headline'], '']
    out.append('Made %s%s. This is a quick headless check of the first %s frames, not a play test: sound, the controller and whether '
               'the picture looks right are not checked.' % (r['created'], ' (%s)' % r['label'] if r['label'] else '', s['frames']))
    if r['changes'] is not None:
        out += ['', summary_of_changes(r)]
    out += ['', '## 1. How far it got', '']
    out += render_steps(r)
    out.append('')
    if run.get('ran'):
        out += render_run(r)
        out.append('')
    out += ['## 2. Does the picture move?', '', '**%s**' % pic['text'], '']
    if run.get('ran') and pic['shots']:
        out += render_pictures(pic, game)
        out.append('')
    out += ['## 3. What the runner says is missing', '']
    out += render_missing(run) if run.get('ran') else ['The game was not run, so there is nothing to list.']
    out += ['', '## 4. What changed since the last report', '']
    out += render_changes(r)
    out += ['', '## Earlier reports', '']
    now = dict(created=r['created'], label=r['label'], headline=verdict['headline'])
    out += table([(h['created'], h['label'], h['headline']) for h in [now] + list(reversed(r['history']))[:6]], ['When', 'Label', 'Result'])
    out += ['', '## Settings used', '']
    out.append('- Frames per run: %d. Rounds at most: %s. Time limit: %s.' % (s['frames'], s['rounds'] or "port.sh's default (8)", nice_seconds(s['timeout'])))
    out.append('- Buttons pressed: %s.' % press_text(s))
    out.append('- Patches: %s%s%s.' % (', '.join(s['pnach']) or 'none', '; sections: ' + ', '.join(s['pnach_sections']) if s['pnach_sections'] else '',
                                       '; widescreen on' if s['widescreen'] else ''))
    out.append('- Project version: %s.' % r['project'])
    out += ['', '## Where things are', '']
    out.append('- This report: `%s`; the same facts for scripts: `%s`' % (os.path.join(game, 'report.md'), os.path.join(game, 'report.json')))
    out.append('- Screenshots as PNG: `%s` (the raw ones are in `%s`)' % (os.path.join(game, 'report-pictures'), os.path.join(game, 'shots')))
    out.append("- The runner's whole log: `%s`; port.sh's output: `%s`" % (os.path.join(game, 'explore.log'), os.path.join(ROOT, port['log'].replace('/', os.sep))))
    if port['play']:
        out.append('- To play it (from the project folder): `%s`' % port['play'])
    out += ['', 'This report describes a game and stays in the `games` folder, which git ignores. Do not share it where game details must not go.']
    return '\n'.join(out) + '\n'


def summary_of_changes(r):
    counts = collections.Counter(c['kind'] for c in r['changes'])
    since = 'Since the last report (%s): ' % r['compared_with']['created']
    if not r['changes']:
        return since + 'nothing changed.'
    words = (('better', 'better'), ('worse', 'worse'), ('note', 'different'))
    return since + ', '.join('%d %s' % (counts[kind], word) for kind, word in words if counts[kind]) + '. See part 4.'


# ---------------------------------------------------------------- main


def compact(r):
    """One line of history for a report."""
    return dict(created=r['created'], label=r.get('label', ''), level=r['verdict']['level'], headline=r['verdict']['headline'], project=r.get('project'),
                frames=r['run'].get('frames'), picture=r['picture']['verdict'], step_reached=r['port']['step_reached'])


def load_previous(json_path):
    """The report written last time, or None (none yet, unreadable, or in another layout)."""
    if not os.path.exists(json_path):
        return None
    try:
        with open(json_path, encoding='utf-8') as f:
            prev = json.load(f)
    except (OSError, ValueError):
        say('the earlier report.json could not be read, so there is nothing to compare with')
        return None
    if not isinstance(prev, dict) or prev.get('format') != FORMAT:
        say('the earlier report.json has another layout, so there is nothing to compare with')
        return None
    return prev


def add_history(report, json_path, prev_path):
    """Fills in report['changes'], ['compared_with'] and ['history'] from the report written before, which is kept as report-prev.json."""
    prev = load_previous(json_path)
    report['history'], report['changes'], report['compared_with'] = [], None, None
    if prev is None:
        return
    try:
        report['changes'] = compare(prev, report)
        report['compared_with'] = dict(created=prev['created'], label=prev.get('label', ''))
        report['history'] = (list(prev.get('history', [])) + [compact(prev)])[-HISTORY:]
    except (KeyError, TypeError, AttributeError) as problem:  # a report that is not what this script wrote: start the comparison over
        say('the earlier report could not be compared (%s)' % problem)
        report['changes'], report['compared_with'], report['history'] = None, None, []
        return
    try:
        shutil.copyfile(json_path, prev_path)
    except OSError:
        pass


def clean_game(name):
    game = os.path.join(ROOT, 'games', name)
    for entry in PORT_FILES:
        path = os.path.join(game, entry)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path, ignore_errors=True)
        elif os.path.lexists(path):
            os.remove(path)
    for folder in (os.path.join(ROOT, 'generated', name), os.path.join(ROOT, 'build', 'native-' + name)):
        shutil.rmtree(folder, ignore_errors=True)


def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(
        prog='report.py', description='Port a PS2 disc image, run it headless and write a plain report of how far it got '
                                      '(games/<name>/report.md and report.json).',
        epilog='Exit code: 0 = ported and run, 1 = report written but the port failed or timed out, 2 = nothing was run (bad arguments, missing image).')
    parser.add_argument('name', help='a short name for the game; its files go to games/<name>/ (letters, digits, dot, dash, underscore)')
    parser.add_argument('image', help='the disc image: .iso, or a .zip / .7z holding one')
    parser.add_argument('--frames', type=int, default=DEFAULT_FRAMES, help='frames per exploring run (default %d)' % DEFAULT_FRAMES)
    parser.add_argument('--press', action='append', default=[], metavar='F:BUTTON', help='a button press for the runs, e.g. 900:cross; repeatable (default: start at 300, cross every 300)')
    parser.add_argument('--rounds', type=int, default=0, help='exploring runs at most (default: port.sh\'s 8)')
    parser.add_argument('--pnach', action='append', default=[], metavar='FILE', help='a PCSX2 patch file; repeatable')
    parser.add_argument('--pnach-section', action='append', default=[], metavar='NAME', help='also apply the patches under [NAME]; repeatable')
    parser.add_argument('--widescreen', action='store_true', help='tell the game the TV is 16:9')
    parser.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT, help='seconds the whole port.sh run may take before it is killed (default %d)' % DEFAULT_TIMEOUT)
    parser.add_argument('--jobs', type=int, default=0, help='limit the compiler jobs of the builds (PS2_JOBS)')
    parser.add_argument('--label', default='', help='a few words that name this run in the report, e.g. "with the 16:9 patch"')
    parser.add_argument('--fresh', action='store_true', help='forget the game\'s earlier work files and build from scratch (reports are kept)')
    return parser.parse_args(argv)


def usage_error(message):
    """Nothing was run: say why, plainly, and exit with 2 (1 is for a port that failed)."""
    print('report: ' + message, file=sys.stderr, flush=True)
    sys.exit(2)


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='replace')  # a path with letters the console cannot show must not stop the report
        except (AttributeError, ValueError):
            pass
    options = parse_arguments(argv)
    if not NAME_OK.match(options.name):
        usage_error('"%s" is not a usable game name: use letters, digits, dot, dash and underscore (no spaces), at most 61 characters.' % options.name)
    image = os.path.abspath(options.image)
    if not os.path.isfile(image):
        usage_error('the disc image %s does not exist (or is not a file). Give the path to the .iso, .zip or .7z.' % options.image)
    if options.frames < 1 or options.timeout < 1 or options.rounds < 0 or options.jobs < 0:
        usage_error('--frames and --timeout must be positive, --rounds and --jobs not negative')
    options.pnach = [os.path.abspath(p).replace('\\', '/') for p in options.pnach]
    for path in options.pnach:
        if not os.path.isfile(path):
            usage_error('the patch file %s does not exist' % path)
    for spec in options.press:
        if not re.fullmatch(r'\d+:[A-Za-z0-9_]+(\+[A-Za-z0-9_]+)*(:\d+)?', spec):
            usage_error('--press %s: expected FRAME:BUTTON, for example 900:cross (or 900:cross+start:10 to hold for 10 frames)' % spec)
    port_image = image.replace('\\', '/')

    game = os.path.join(ROOT, 'games', options.name)
    os.makedirs(game, exist_ok=True)
    iso = os.path.join(game, 'disc.iso')
    fresh = options.fresh
    if not fresh and os.path.exists(iso) and not image.lower().endswith(('.zip', '.7z')):
        try:
            same = os.path.samefile(iso, image) or os.path.getsize(iso) == os.path.getsize(image)
        except OSError:
            same = True
        if not same:
            say('games/%s holds a different disc image from the one you gave: starting from scratch (port.sh would keep the old one)' % options.name)
            fresh = True
    if fresh:
        say('clearing the earlier work files of %s' % options.name)
        clean_game(options.name)

    try:
        ensure_tools()
    except SystemExit as stop:  # the translator tools could not be built (ps2tools.die)
        print(stop.code, file=sys.stderr, flush=True)
        return 2
    args = [options.name, port_image, '--native-only', '--frames', options.frames]
    if options.rounds:
        args += ['--rounds', options.rounds]
    for spec in options.press:
        args += ['--press', spec]
    for path in options.pnach:
        args += ['--pnach', path]
    for section in options.pnach_section:
        args += ['--pnach-section', section]
    if options.widescreen:
        args.append('--widescreen')
    if options.jobs:
        os.environ['PS2_JOBS'] = str(options.jobs)

    log_path = os.path.join(game, 'report-port.log')
    rounds_at = []
    started = time.time()

    def on_line(raw):
        text = raw.decode('latin-1').rstrip()
        match = re.match(r'== \[(\d)/6\] (.*)', text)
        if match:
            say('  step %s of 6: %s' % (match.group(1), match.group(2)))
        match = ROUND.match(text)
        if match:
            rounds_at.append((int(match.group(1)), time.time() - started))
            say('    round %s: %s' % (match.group(1), text.split(': ', 1)[1]))

    say('%s: porting %s (log: %s)' % (options.name, os.path.basename(image), log_path))
    try:
        code, steps, seconds = ps2tools.run_port(args, log_path, options.timeout, None, on_line)
    except KeyboardInterrupt:
        say('interrupted: port.sh was stopped, no report was written')
        return 130
    report = build_report(options, image, started, code, steps, rounds_at, seconds, log_path)

    json_path = os.path.join(game, 'report.json')
    add_history(report, json_path, os.path.join(game, 'report-prev.json'))
    write_text(json_path, json.dumps(report, indent=1) + '\n')
    write_text(os.path.join(game, 'report.md'), render(report, game))

    say('')
    say('Result: ' + report['verdict']['headline'])
    if not report['port']['completed']:
        failure = report['port']['failure'] or {}
        if failure.get('line'):
            say('Last error line: ' + failure['line'])
        say('Whole log: ' + log_path)
    say('Report: ' + os.path.join(game, 'report.md'))
    return 0 if report['port']['completed'] else 1


if __name__ == '__main__':
    sys.exit(main())
