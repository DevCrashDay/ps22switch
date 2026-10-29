#!/usr/bin/env python
"""Checks a sound recording (a 16-bit PCM WAV file) without listening to it.

    python scripts/wavcheck.py sound.wav [--frames N] [--fps 60] [--from S] [--to S] [--segment S] [--json]
    python scripts/wavcheck.py sound.wav --cut FROM TO out.wav

It prints: the format; the length, and with --frames the length the run should have made (N video frames at --fps);
level, peak, clipping and DC offset per channel; whether left and right both carry sound; silent stretches; gaps
(runs of exact zeros between sound) and clicks (single-sample jumps far above the sound around them); a rough
spectrum (energy per band, spectral flatness, strongest tones), the tuning of music (do the strongest tones sit on the
12-note scale with A at 440 Hz? a recording played too fast or too slow would be off by many cents) and a table per segment. --cut writes the part
from FROM to TO seconds into a new WAV file. Needs numpy.
"""

import argparse
import json
import sys
import wave

import numpy as np

BANDS = [(20, 60), (60, 250), (250, 2000), (2000, 6000), (6000, 20000)]


def load(path):
    with wave.open(path, 'rb') as w:
        info = {'rate': w.getframerate(), 'channels': w.getnchannels(), 'bits': w.getsampwidth() * 8, 'frames': w.getnframes()}
        raw = w.readframes(w.getnframes())
    if info['bits'] != 16:
        sys.exit(f'{path}: {info["bits"]}-bit samples, only 16-bit PCM is supported')
    data = np.frombuffer(raw, dtype='<i2')
    data = data[: len(data) // info['channels'] * info['channels']].reshape(-1, info['channels'])
    info['frames'] = len(data)
    return info, data


def db(x):
    return 20 * np.log10(max(float(x), 1e-12))


def runs(mask):
    """(start, length) of each run of True in a boolean array."""
    if not len(mask):
        return []
    edges = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    return list(zip(starts.tolist(), (ends - starts).tolist()))


def spectrum(mono, rate, size=4096):
    """Average power spectrum over windows that carry sound (Welch, Hann window)."""
    hops = range(0, len(mono) - size, size // 2)
    window = np.hanning(size)
    total, used = np.zeros(size // 2 + 1), 0
    for at in hops:
        chunk = mono[at:at + size]
        if np.sqrt(np.mean(chunk ** 2)) < 10 ** (-60 / 20):
            continue
        total += np.abs(np.fft.rfft(chunk * window)) ** 2
        used += 1
    return np.fft.rfftfreq(size, 1 / rate), total / max(used, 1), used


def tuning(mono, rate, size=16384):
    """How well the strongest tones in 100..2000 Hz sit on the equal-tempered scale with A = 440 Hz.

    Returns (mean offset in cents, concentration 0..1, median distance from the nearest note in cents, windows used) or None.
    Music written for that scale gives a mean near 0, a concentration around 0.4 or more and a median well below 25 cents (what
    unrelated tones would give); a wrong playback speed moves the mean by 17 cents per percent. Speech and effects have no scale,
    so a low concentration says nothing.
    """
    window = np.hanning(size)
    total, used = np.zeros(size // 2 + 1), 0
    for at in range(0, len(mono) - size, size // 2):
        chunk = mono[at:at + size]
        if np.sqrt(np.mean(chunk ** 2)) < 0.003:
            continue
        total += np.abs(np.fft.rfft(chunk * window)) ** 2
        used += 1
    if not used:
        return None
    freqs = np.fft.rfftfreq(size, 1 / rate)
    power = total / used
    ok = (power[1:-1] > power[:-2]) & (power[1:-1] >= power[2:]) & (freqs[1:-1] > 100) & (freqs[1:-1] < 2000)
    peaks = np.flatnonzero(ok) + 1
    peaks = peaks[np.argsort(-power[peaks])][:60]
    if len(peaks) < 10:
        return None
    cents, weights = [], []
    for i in peaks:
        a, b, c = np.log(power[i - 1] + 1e-30), np.log(power[i] + 1e-30), np.log(power[i + 1] + 1e-30)
        hz = (i + 0.5 * (a - c) / (a - 2 * b + c)) * rate / size
        semitones = 12 * np.log2(hz / 440.0)
        cents.append((semitones - np.round(semitones)) * 100)
        weights.append(power[i])
    cents, weights = np.array(cents), np.array(weights)
    mean = (weights * np.exp(2j * np.pi * cents / 100)).sum() / weights.sum()
    return float(np.angle(mean) / (2 * np.pi) * 100), float(abs(mean)), float(np.median(np.abs(cents))), used


def flatness(power):
    power = power[1:] + 1e-20
    return float(np.exp(np.mean(np.log(power))) / np.mean(power))


def describe(freqs, power):
    total = float(power[1:].sum()) or 1e-20
    bands = {f'{lo}-{hi} Hz': float(power[(freqs >= lo) & (freqs < hi)].sum() / total) for lo, hi in BANDS}
    peaks = []
    p = power.copy()
    for _ in range(5):
        i = int(np.argmax(p[1:]) + 1)
        peaks.append({'hz': round(float(freqs[i]), 1), 'share': float(power[max(i - 2, 0):i + 3].sum() / total)})
        p[max(i - 4, 0):i + 5] = 0
    centroid = float((freqs * power).sum() / (power.sum() or 1e-20))
    return {'bands': bands, 'flatness': flatness(power), 'centroid_hz': centroid, 'peaks': peaks}


def check(path, args):
    info, data = load(path)
    rate, frames = info['rate'], info['frames']
    lo = int((args.start or 0) * rate)
    hi = int(args.end * rate) if args.end is not None else frames
    data = data[lo:hi]
    x = data.astype(np.float64) / 32768.0
    seconds = len(x) / rate
    report = {'file': path, 'format': info, 'seconds': seconds, 'from': lo / rate}
    if args.frames:
        expected = args.frames / args.fps
        report['expected_seconds'] = expected
        report['seconds_per_expected'] = info['frames'] / rate / expected if expected else None

    ch = []
    for c in range(x.shape[1]):
        s = x[:, c]
        clipped = int(np.count_nonzero((data[:, c] >= 32767) | (data[:, c] <= -32768)))
        ch.append({'rms_dbfs': db(np.sqrt(np.mean(s ** 2))) if len(s) else None, 'peak_dbfs': db(np.max(np.abs(s))) if len(s) else None,
                   'dc': float(np.mean(s)) if len(s) else 0.0, 'clipped': clipped, 'clipped_share': clipped / max(len(s), 1)})
    report['channels'] = ch
    if x.shape[1] == 2 and len(x):
        l, r = x[:, 0], x[:, 1]
        report['stereo'] = {'identical': bool(np.array_equal(data[:, 0], data[:, 1])),
                            'correlation': float(np.corrcoef(l, r)[0, 1]) if np.std(l) > 0 and np.std(r) > 0 else None,
                            'side_to_mid_db': db(np.sqrt(np.mean((l - r) ** 2)) / max(np.sqrt(np.mean((l + r) ** 2)), 1e-12))}

    # Silence: 50 ms windows quieter than -60 dBFS on both channels.
    win = rate // 20
    n = len(x) // win
    level = np.sqrt(np.mean(x[: n * win].reshape(n, win, -1) ** 2, axis=(1, 2))) if n else np.zeros(0)
    quiet = level < 10 ** (-60 / 20)
    report['silent_share'] = float(np.mean(quiet)) if n else 1.0
    loud = np.flatnonzero(~quiet)
    report['sound_from_to'] = [float(loud[0] * win / rate), float((loud[-1] + 1) * win / rate)] if len(loud) else None
    report['silent_stretches'] = [{'at': round(s * win / rate + lo / rate, 3), 'seconds': round(k * win / rate, 3)}
                                  for s, k in runs(quiet) if k * win / rate >= args.min_silence]

    # Gaps: runs of exact zeros on all channels, at least 2 ms long, that cut into audible sound: the 3 ms just before or
    # just after the zeros are louder than -45 dBFS. A fade to silence and back is not a gap.
    zero = np.all(data == 0, axis=1)
    gaps = []
    edge = max(rate * 3 // 1000, 1)
    for s, k in runs(zero):
        if k < rate // 500 or s == 0 or s + k >= len(data):
            continue
        before, after = x[max(s - edge, 0):s], x[s + k:s + k + edge]
        if max(np.sqrt(np.mean(before ** 2)), np.sqrt(np.mean(after ** 2))) > 10 ** (-45 / 20):
            gaps.append({'at': round((s + lo) / rate, 4), 'ms': round(k * 1000 / rate, 2)})
    report['gaps'] = gaps

    # Clicks: a jump between two samples that is large (> 0.25 of full scale) and far above the typical jump around it.
    clicks = []
    for c in range(x.shape[1]):
        d = np.abs(np.diff(x[:, c]))
        if len(d) < 3:
            continue
        k = 256
        m = len(d) // k
        typical = np.repeat(np.median(d[: m * k].reshape(m, k), axis=1), k) if m else np.zeros(0)
        typical = np.concatenate((typical, np.full(len(d) - len(typical), np.median(d[m * k:]) if len(d) > m * k else 0)))
        hits = np.flatnonzero((d > 0.25) & (d > 20 * (typical + 1e-4)))
        for i in hits:
            if clicks and clicks[-1]['channel'] == c and (i + lo) / rate - clicks[-1]['at'] < 0.01:
                continue
            clicks.append({'channel': c, 'at': round((i + lo) / rate, 4), 'jump': round(float(d[i]), 3)})
    report['clicks'] = sorted(clicks, key=lambda e: e['at'])
    # Clicks that come back at the same spot of a regular block (the sound chip's streamed music arrives in blocks of 64 ms or half or a
    # quarter of that): the sign of a stream whose blocks do not join, not of a sound in the music. GTA III's film had it (2784 and 3040
    # samples into every 3072-sample block, about 30 % of the blocks) until the queued blocks were dropped when the game stops the DMA.
    report['click_repeat'] = None
    for period in (0.064, 0.032, 0.016):
        if len(clicks) < 8:
            break
        phase = np.array([c['at'] % period for c in clicks])
        for centre in phase:
            near = np.flatnonzero(np.minimum((phase - centre) % period, (centre - phase) % period) < 0.0006)
            if len(near) >= 8 and len(near) >= 0.3 * len(clicks) and (not report['click_repeat'] or len(near) > report['click_repeat']['count']):
                report['click_repeat'] = {'period_ms': round(period * 1000), 'at_ms': round(float(centre) * 1000, 1), 'count': int(len(near)), 'of': len(clicks)}
        if report['click_repeat']:
            break

    mono = x.mean(axis=1)
    freqs, power, used = spectrum(mono, rate)
    report['spectrum'] = describe(freqs, power) if used else None
    report['tuning'] = tuning(mono, rate)

    # One row per segment: level, flatness and centroid; music and speech change over time, a buzz or noise does not.
    seg = int(args.segment * rate)
    rows = []
    for at in range(0, len(mono), seg):
        part = mono[at:at + seg]
        if len(part) < seg // 2:
            break
        row = {'at': round((at + lo) / rate, 1), 'rms_dbfs': round(db(np.sqrt(np.mean(part ** 2))), 1), 'peak_dbfs': round(db(np.max(np.abs(part))), 1)}
        if row['rms_dbfs'] > -60 and len(part) > 8192:
            f, pw, u = spectrum(part, rate)
            if u:
                dsc = describe(f, pw)
                row.update(flatness=round(dsc['flatness'], 3), centroid_hz=round(dsc['centroid_hz']), top_hz=dsc['peaks'][0]['hz'],
                           top_share=round(dsc['peaks'][0]['share'], 2))
        rows.append(row)
    report['segments'] = rows
    return report


def verdicts(r):
    out = []
    f = r['format']
    out.append(('ok' if f['rate'] == 48000 and f['channels'] == 2 else 'CHECK', f'format {f["rate"]} Hz, {f["channels"]} channels, {f["bits"]} bit'))
    if 'expected_seconds' in r:
        ratio = r['seconds_per_expected']
        out.append(('ok' if ratio and 0.97 <= ratio <= 1.001 else 'CHECK',
                    f'{r["format"]["frames"] / f["rate"]:.2f} s of sound for {r["expected_seconds"]:.2f} s of emulated time ({ratio:.4f})'))
    for i, c in enumerate(r['channels']):
        name = 'left' if i == 0 else 'right' if i == 1 else f'channel {i}'
        out.append(('ok' if c['clipped_share'] < 1e-4 else 'CHECK',
                    f'{name}: level {c["rms_dbfs"]:.1f} dBFS, peak {c["peak_dbfs"]:.1f} dBFS, {c["clipped"]} clipped samples, DC {c["dc"]:+.5f}'))
        if abs(c['dc']) > 0.01:
            out.append(('CHECK', f'{name}: DC offset {c["dc"]:+.4f} of full scale'))
    st = r.get('stereo')
    if st:
        both = all(c['rms_dbfs'] is not None and c['rms_dbfs'] > -70 for c in r['channels'])
        out.append(('ok' if both and not st['identical'] else 'CHECK',
                    'left and right ' + ('are identical (mono)' if st['identical'] else f'differ (correlation {st["correlation"]:.2f}, side/mid {st["side_to_mid_db"]:.1f} dB)')
                    + ('' if both else ', one channel is silent')))
    out.append(('ok' if r['silent_share'] < 0.5 else 'CHECK', f'silent {100 * r["silent_share"]:.1f} % of the time'
                + (f', sound from {r["sound_from_to"][0]:.2f} s to {r["sound_from_to"][1]:.2f} s' if r['sound_from_to'] else ', no sound at all')))
    out.append(('ok' if not r['gaps'] else 'CHECK', f'{len(r["gaps"])} gaps (runs of zeros that cut into sound)'
                + ''.join(f'\n        at {g["at"]:.3f} s, {g["ms"]} ms' for g in r['gaps'][:10]) + (' ...' if len(r['gaps']) > 10 else '')))
    out.append(('ok' if len(r['clicks']) <= max(1, r['seconds'] / 10) else 'CHECK', f'{len(r["clicks"])} clicks (sudden jumps)'
                + ''.join(f'\n        at {c["at"]:.4f} s, channel {c["channel"]}, jump {c["jump"]}' for c in r['clicks'][:10]) + (' ...' if len(r['clicks']) > 10 else '')))
    if r['click_repeat']:
        q = r['click_repeat']
        out.append(('CHECK', f'{q["count"]} of the {q["of"]} clicks come back every {q["period_ms"]} ms at the same spot ({q["at_ms"]} ms into every {q["period_ms"]} ms stretch of the recording): '
                    'a streamed block that does not join its neighbours'))
    sp = r['spectrum']
    if sp:
        bands = ', '.join(f'{k} {100 * v:.0f} %' for k, v in sp['bands'].items())
        tonal = sp['peaks'][0]['share'] > 0.6
        noisy = sp['flatness'] > 0.5
        out.append(('ok' if not tonal and not noisy else 'CHECK', f'spectrum: flatness {sp["flatness"]:.3f} (noise is near 1, a pure tone near 0), '
                    f'centroid {sp["centroid_hz"]:.0f} Hz, strongest {sp["peaks"][0]["hz"]} Hz with {100 * sp["peaks"][0]["share"]:.0f} % of the energy'
                    f'\n        bands: {bands}'))
    tn = r.get('tuning')
    if tn:
        offset, concentration, median, _ = tn
        if concentration >= 0.4:
            out.append(('ok' if abs(offset) < 15 else 'CHECK', f'tuning: the strongest tones sit on the A=440 scale (offset {offset:+.1f} cents, {median:.0f} cents from the nearest note, '
                        f'agreement {concentration:.2f}); speed or pitch is right within about {abs(offset) / 17:.1f} %' if abs(offset) < 15 else
                        f'tuning: the tones sit on a scale that is {offset:+.1f} cents off A=440 (agreement {concentration:.2f}): playback speed may be wrong'))
        else:
            out.append(('info', f'tuning: no clear musical scale in the strongest tones (agreement {concentration:.2f}, {median:.0f} cents from the nearest note; '
                        'speech, effects and mixed passages give this), so it says nothing about speed'))
    return out


def cut(path, start, end, out):
    with wave.open(path, 'rb') as w:
        rate, width, channels = w.getframerate(), w.getsampwidth(), w.getnchannels()
        w.setpos(min(int(start * rate), w.getnframes()))
        raw = w.readframes(max(int((end - start) * rate), 0))
    with wave.open(out, 'wb') as o:
        o.setnchannels(channels)
        o.setsampwidth(width)
        o.setframerate(rate)
        o.writeframes(raw)
    print(f'{out}: {len(raw) // (width * channels) / rate:.2f} s from {start} s')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('wav')
    p.add_argument('--frames', type=int, help='video frames the run emulated, to compare the length with')
    p.add_argument('--fps', type=float, default=60.0, help='video frames per emulated second (60 NTSC, 50 PAL)')
    p.add_argument('--from', dest='start', type=float, help='check from this second on')
    p.add_argument('--to', dest='end', type=float, help='check up to this second')
    p.add_argument('--segment', type=float, default=5.0, help='seconds per row of the segment table')
    p.add_argument('--min-silence', type=float, default=1.0, help='list silent stretches at least this long (seconds)')
    p.add_argument('--json', action='store_true', help='print the whole result as JSON')
    p.add_argument('--cut', nargs=3, metavar=('FROM', 'TO', 'OUT'), help='write FROM..TO seconds into OUT and stop')
    args = p.parse_args()
    if args.cut:
        cut(args.wav, float(args.cut[0]), float(args.cut[1]), args.cut[2])
        return
    r = check(args.wav, args)
    if args.json:
        print(json.dumps(r, indent=1))
        return
    print(f'{r["file"]}: {r["seconds"]:.2f} s checked' + (f' from {r["from"]:.2f} s' if r['from'] else ''))
    for mark, text in verdicts(r):
        print(f'  [{mark:5}] {text}')
    if r['silent_stretches']:
        print('  silent stretches: ' + ', '.join(f'{s["at"]:.1f} s for {s["seconds"]:.1f} s' for s in r['silent_stretches'][:12])
              + (' ...' if len(r['silent_stretches']) > 12 else ''))
    print('  segments (time, level, peak, flatness, centroid, strongest tone and its share):')
    for row in r['segments']:
        extra = f'  flat {row["flatness"]:.3f}  centroid {row["centroid_hz"]:5d} Hz  top {row["top_hz"]:7.1f} Hz {row["top_share"]:.2f}' if 'flatness' in row else ''
        print(f'    {row["at"]:7.1f} s  {row["rms_dbfs"]:6.1f} dB  peak {row["peak_dbfs"]:6.1f} dB{extra}')


if __name__ == '__main__':
    main()
