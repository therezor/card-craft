#!/usr/bin/env python3
"""Generate src/sfxdata.h — the game's sound bank, rendered as 8-bit PCM.

The game used to synthesise on the device: every sound was a short table of
constant-pitch, constant-volume steps handed to Speaker.tone(), which loops a
16-sample wavetable at a frequency. That format has no envelope, so nothing
decays and every impact ends by being cut off; and its "noise" is a 16-sample
cycle played *at a pitch*, which is a buzz, not noise. An explosion built out of
it is five beeps in a row. No amount of retuning the frequency tables fixes
that, because the missing parts — decay, broadband noise, a continuous pitch
fall — are not expressible in the format.

So the waveforms are rendered here instead, once, offline, and the device just
plays them back through Speaker.playRaw(). That buys real envelopes, real noise
and real sweeps for about 85 KB of flash out of a 3.2 MB partition, and costs no
RAM at all: the arrays live in flash and the speaker task reads them in place.

    ./tools/make-sfx.py                # rewrite src/sfxdata.h + the wav previews
    afplay tools/sfx-preview/explode.wav

The previews are the point of the wav output. Judging a sound by reading its
recipe does not work; iterate here with afplay, and only flash once it is right.
tools/make-sprites.py and tools/make-font.py do the same trick for the art.

Everything is deterministic — one seeded Random, no wall clock — so
regenerating the header twice produces the same bytes and a diff means someone
changed a recipe.
"""

import argparse
import math
import random
import wave
from pathlib import Path

SR = 16000  # sample rate for the whole bank. Noise reads dull much below this,
            # and the flash cost is linear, so this is the knob to turn if the
            # bank ever needs to shrink or sparkle.

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "sfxdata.h"
PREVIEW = ROOT / "tools" / "sfx-preview"

rng = random.Random(1)


# ---- primitives -------------------------------------------------------------

def n(ms):
    return max(1, int(round(SR * ms / 1000.0)))


def buf(ms):
    return [0.0] * n(ms)


def add(dst, src, at_ms=0.0, gain=1.0):
    """Mixes src into dst at an offset, growing dst if it has to."""
    i = n(at_ms) if at_ms else 0
    if i + len(src) > len(dst):
        dst.extend([0.0] * (i + len(src) - len(dst)))
    for k, v in enumerate(src):
        dst[i + k] += v * gain
    return dst


SHAPE = {
    "sine":   lambda p: math.sin(2.0 * math.pi * p),
    "square": lambda p: 1.0 if p < 0.5 else -1.0,
    "saw":    lambda p: 2.0 * p - 1.0,
    "tri":    lambda p: 4.0 * abs(p - 0.5) - 1.0,
}


def osc(ms, f0, f1=None, wave="sine", vib_hz=0.0, vib=0.0):
    """One oscillator, optionally sweeping f0 -> f1 exponentially.

    Exponential rather than linear because pitch is heard in ratios: a linear
    300 -> 60 Hz fall spends most of its time already at the bottom.
    """
    N = n(ms)
    f1 = f0 if f1 is None else f1
    out = [0.0] * N
    ph = 0.0
    shape = SHAPE[wave]
    for i in range(N):
        u = i / (N - 1) if N > 1 else 0.0
        f = f0 * (f1 / f0) ** u
        if vib:
            f *= 1.0 + vib * math.sin(2.0 * math.pi * vib_hz * i / SR)
        ph = (ph + f / SR) % 1.0
        out[i] = shape(ph)
    return out


def noise(ms):
    return [rng.uniform(-1.0, 1.0) for _ in range(n(ms))]


# ---- envelopes --------------------------------------------------------------

def decay(sig, tau_ms, attack_ms=1.0):
    """Attack ramp then an exponential tail. The tail is the whole difference
    between a hit and a beep."""
    N = len(sig)
    a = max(1, n(attack_ms))
    tau = SR * tau_ms / 1000.0
    out = [0.0] * N
    for i in range(N):
        e = (i / a) if i < a else math.exp(-(i - a) / tau)
        out[i] = sig[i] * e
    return out


def adr(sig, attack_ms=6.0, hold_ms=40.0, tau_ms=60.0):
    """Attack, hold, exponential release — for anything meant to read as a note."""
    a, h = max(1, n(attack_ms)), n(hold_ms)
    tau = SR * tau_ms / 1000.0
    out = [0.0] * len(sig)
    for i, s in enumerate(sig):
        if i < a:
            e = i / a
        elif i < a + h:
            e = 1.0
        else:
            e = math.exp(-(i - a - h) / tau)
        out[i] = s * e
    return out


def ramp(sig, lo, hi):
    N = len(sig)
    return [s * (lo + (hi - lo) * (i / (N - 1) if N > 1 else 0.0))
            for i, s in enumerate(sig)]


# ---- filters ----------------------------------------------------------------
#
# One-pole pairs rather than anything resonant, because a one-pole is
# unconditionally stable at any cutoff and the cutoffs here sweep across two
# decades. The one place a resonance is wanted — the bow twang — gets a state
# variable filter, at a frequency low enough for it to stay well inside its
# stable range.

def lowpass(sig, fc0, fc1=None, poles=1):
    """Cascaded one-poles. Two of them (12 dB/octave) rather than one is the
    difference between an explosion that rumbles and one that still hisses: a
    single pole leaves so much energy an octave above the cutoff that sweeping
    it down barely darkens the sound."""
    fc1 = fc0 if fc1 is None else fc1
    N = len(sig)
    out = [0.0] * N
    y = [0.0] * poles
    for i, x in enumerate(sig):
        u = i / (N - 1) if N > 1 else 0.0
        fc = min(fc0 * (fc1 / fc0) ** u, SR * 0.45)
        a = 1.0 - math.exp(-2.0 * math.pi * fc / SR)
        v = x
        for p in range(poles):
            y[p] += a * (v - y[p])
            v = y[p]
        out[i] = v
    return out


def highpass(sig, fc0, fc1=None):
    fc1 = fc0 if fc1 is None else fc1
    N = len(sig)
    out = [0.0] * N
    y = 0.0
    xp = 0.0
    for i, x in enumerate(sig):
        u = i / (N - 1) if N > 1 else 0.0
        fc = min(fc0 * (fc1 / fc0) ** u, SR * 0.45)
        a = math.exp(-2.0 * math.pi * fc / SR)
        y = a * (y + x - xp)
        xp = x
        out[i] = y
    return out


def band(sig, lp0, lp1=None, hp0=40.0, hp1=None, poles=2):
    """The workhorse for every noise-based sound. Two poles on the low side
    because that is the edge that carries the character; one on the high side is
    enough to lift the mud out."""
    return highpass(lowpass(sig, lp0, lp1, poles), hp0, hp1)


def resonator(sig, f0, f1=None, q=10.0):
    """Chamberlin SVF, bandpass tap. Only used well below SR/6, where it is
    stable; the clamp is a guard, not a working part."""
    f1 = f0 if f1 is None else f1
    N = len(sig)
    out = [0.0] * N
    low = bp = 0.0
    dq = 1.0 / q
    for i, x in enumerate(sig):
        u = i / (N - 1) if N > 1 else 0.0
        fc = f0 * (f1 / f0) ** u
        f = min(2.0 * math.sin(math.pi * fc / SR), 0.85)
        high = x - low - dq * bp
        bp += f * high
        low += f * bp
        out[i] = bp
    return out


# ---- output shaping ---------------------------------------------------------

def fade(sig, ms=1.5):
    """Ramps both ends to zero. A buffer that starts or stops on a non-zero
    sample clicks, and a click is the cheapest-sounding artefact there is."""
    k = min(n(ms), len(sig) // 2)
    if k < 1:
        return sig
    for i in range(k):
        w = i / k
        sig[i] *= w
        sig[-1 - i] *= w
    return sig


def finish(sig, peak):
    """Normalises to a chosen peak, then fades the edges.

    The peak is per-cue and deliberate: it is what keeps the mix ordered the way
    sfx.cpp describes it — urgent things (hurt, explosion, death) loud and low,
    routine things (mining, placing, menus) quiet and high — regardless of how
    hot the recipe happened to come out.
    """
    m = max(abs(s) for s in sig) or 1.0
    g = peak / m
    return fade([s * g for s in sig])


def note(freq, ms, wave="tri", attack=6.0, hold=None, tau=None, harm=0.0):
    hold = ms * 0.30 if hold is None else hold
    tau = ms * 0.35 if tau is None else tau
    s = osc(ms, freq, freq, wave)
    if harm:
        s = [a + harm * b for a, b in zip(s, osc(ms, freq * 2.0, freq * 2.0, "sine"))]
    return adr(s, attack, hold, tau)


def melody(notes, peak, wave="tri", harm=0.0):
    """notes: [(freq, ms, at_ms), ...]. Overlapping tails on purpose — two notes
    that butt up against each other with hard edges read as two beeps."""
    out = buf(1)
    for freq, ms, at in notes:
        add(out, note(freq, ms, wave=wave, harm=harm), at)
    return finish(out, peak)


# ---- the bank ---------------------------------------------------------------
#
# Each function returns floats in -1..1. The comments say what the sound is
# meant to be, because the numbers cannot.

def s_explode():
    """Noise falling from a hiss to a rumble, over a sub that drops with it, and
    a second thump behind it. The fall is the sound: five discrete tones read as
    a chiptune, a continuous sweep reads as a blast."""
    d = 800
    out = buf(d)
    add(out, decay(noise(40), 8, 0.3), 0, 0.9)                       # the crack
    add(out, decay(band(noise(d), 3000, 200, 50, 40), 230, 2), 0, 1.0)
    add(out, decay(osc(d, 120, 45, "sine"), 260, 4), 0, 0.9)         # the sub
    add(out, decay(osc(300, 70, 40, "sine"), 110, 8), 90, 0.35)      # ground shock
    return finish(out, 1.0)


def s_died():
    """A long saw falling away with a wobble in it. The one sound allowed to
    take most of a second."""
    d = 700
    out = buf(d)
    add(out, decay(lowpass(osc(d, 300, 100, "saw", vib_hz=5.5, vib=0.03),
                           2500, 500), 330, 20), 0, 1.0)
    add(out, decay(osc(d, 150, 50, "tri"), 300, 20), 0, 0.55)
    return finish(out, 0.95)


def s_hurt():
    """A noise transient over a square dropping through the bottom of the mix.
    Has to cut through mining and a wave at once, so it is the loudest thing
    that is not an explosion."""
    d = 220
    out = buf(d)
    add(out, decay(highpass(noise(30), 900), 10, 0.3), 0, 0.8)
    add(out, decay(lowpass(osc(d, 200, 90, "square"), 1600, 700), 90, 2), 0, 1.0)
    return finish(out, 0.95)


def s_mob_hit():
    """Meat and bone: a 10 ms crack sitting on an 80 ms thump."""
    d = 170
    out = buf(d)
    add(out, decay(band(noise(30), 5000, 2500, 600), 6, 0.2), 0, 1.0)
    add(out, decay(lowpass(osc(120, 180, 110, "square"), 1400, 600), 45, 1), 0, 0.9)
    add(out, decay(osc(d, 95, 70, "tri"), 70, 3), 0, 0.6)
    return finish(out, 0.9)


def s_mob_died():
    """A saw sagging out from under itself. Lower and slower than a hit, so the
    two never read as the same event."""
    d = 400
    out = buf(d)
    add(out, decay(lowpass(osc(d, 440, 150, "saw", vib_hz=9.0, vib=0.04),
                           3000, 700), 160, 8), 0, 1.0)
    add(out, decay(osc(d, 220, 90, "tri"), 150, 8), 0, 0.4)
    return finish(out, 0.85)


def s_block_broke():
    """The crack, then two pieces landing. The debris is what says a block came
    apart rather than that something was struck."""
    d = 280
    out = buf(d)
    add(out, decay(band(noise(60), 4500, 1500, 900), 14, 0.3), 0, 1.0)
    add(out, decay(band(noise(70), 900, 500, 180), 26, 1), 70, 0.55)
    add(out, decay(osc(70, 190, 140, "tri"), 26, 1), 70, 0.4)
    add(out, decay(band(noise(60), 700, 400, 150), 22, 1), 150, 0.4)
    add(out, decay(osc(60, 150, 110, "tri"), 22, 1), 150, 0.3)
    return finish(out, 0.85)


def s_mine_tick():
    """A pick hitting rock. Fires several times a second while mining, so it has
    to be short, dull and quiet — anything bright here becomes a buzz within a
    second. sfx.cpp jitters its playback rate so repeats are not identical."""
    d = 75
    out = buf(d)
    add(out, decay(band(noise(30), 1400, 700, 260), 12, 0.3), 0, 1.0)
    add(out, decay(osc(60, 170, 130, "tri"), 20, 1), 0, 0.7)
    return finish(out, 0.55)


def s_place():
    """A block set down: a soft knock with a little rise on the end, so placing
    and breaking are opposites rather than variations."""
    d = 130
    out = buf(d)
    add(out, decay(band(noise(30), 2000, 900, 400), 12, 0.3), 0, 0.7)
    add(out, decay(osc(110, 300, 430, "tri"), 45, 2), 8, 1.0)
    return finish(out, 0.6)


def s_no_blocks():
    """Two dull low knocks. A refusal, not an alarm."""
    d = 190
    out = buf(d)
    add(out, decay(lowpass(osc(60, 150, 130, "square"), 900), 26, 2), 0, 1.0)
    add(out, decay(lowpass(osc(60, 130, 110, "square"), 800), 26, 2), 95, 0.8)
    return finish(out, 0.5)


def s_swing():
    """Air moving past. Bandpassed noise swept down — a swing is a whoosh, and a
    whoosh is a moving filter, not a pitch."""
    d = 130
    out = buf(d)
    add(out, adr(band(noise(d), 2400, 700, 500, 250), 18, 10, 45), 0, 1.0)
    return finish(out, 0.45)


def s_whiff():
    """The same gesture into empty space: wider, quieter, longer. It exists so
    that swinging at nothing is still something you can hear."""
    d = 180
    out = buf(d)
    add(out, adr(band(noise(d), 3000, 500, 400, 180), 25, 10, 70), 0, 1.0)
    return finish(out, 0.32)


def s_telegraph():
    """A mob committing. Low, short, and the same shape as the fuse that may
    follow it, so the pair reads as one escalating idea."""
    d = 160
    out = buf(d)
    add(out, decay(lowpass(osc(d, 165, 120, "square"), 1100, 600), 60, 3), 0, 1.0)
    return finish(out, 0.55)


def s_hiss():
    """The fuse. Rising in brightness and in volume at once — the climb is the
    whole warning, and it is the only one the player gets."""
    d = 340
    out = buf(d)
    s = band(noise(d), 1000, 2400, 700, 1600)
    add(out, ramp(s, 0.25, 1.0), 0, 1.0)
    return finish(out, 0.9)


def s_arrow_fire():
    """A bow: a damped string over the air of the release. Short, and pitched
    where nothing else in the mix lives, because it is a warning that arrives
    while the player is busy."""
    d = 160
    out = buf(d)
    add(out, decay(resonator(noise(d), 330, 300, q=14.0), 45, 1), 0, 1.0)
    add(out, decay(band(noise(90), 2600, 900, 800), 35, 2), 0, 0.5)
    return finish(out, 0.6)


def s_arrow_hit():
    """A thock into whatever it found."""
    d = 130
    out = buf(d)
    add(out, decay(band(noise(40), 2600, 1000, 500), 14, 0.3), 0, 1.0)
    add(out, decay(osc(90, 150, 110, "tri"), 35, 1), 0, 0.8)
    return finish(out, 0.75)


def s_dusk():
    """Two notes falling. A single beep says something happened; a falling pair
    says which thing."""
    return melody([(330, 150, 0), (247, 230, 140)], 0.65)


def s_dawn():
    return melody([(392, 130, 0), (523, 130, 115), (659, 230, 230)], 0.65)


def s_menu_move():
    """A cursor tick. Thirty milliseconds, and quiet: it plays on every keypress
    in every card, so it is the one sound in the bank that must never be
    noticed."""
    out = buf(45)
    add(out, adr(osc(45, 700, 660, "tri"), 2, 6, 14), 0, 1.0)
    return finish(out, 0.3)


def s_buy():
    return melody([(523, 90, 0), (784, 140, 80)], 0.6, harm=0.25)


def s_craft():
    return melody([(440, 80, 0), (587, 80, 65), (784, 150, 130)], 0.6, harm=0.2)


def s_craft_fail():
    """Down instead of up, and dull instead of bright. Nothing else needs to be
    said about a recipe you cannot afford."""
    d = 160
    out = buf(d)
    add(out, decay(lowpass(osc(d, 210, 140, "square"), 1200, 500), 70, 4), 0, 1.0)
    return finish(out, 0.5)


# Order here is the order in the header, and it is grouped the way sfx.cpp
# groups its cues rather than alphabetically.
BANK = [
    ("mine_tick",  s_mine_tick),
    ("block_broke", s_block_broke),
    ("place",      s_place),
    ("no_blocks",  s_no_blocks),
    ("swing",      s_swing),
    ("whiff",      s_whiff),
    ("mob_hit",    s_mob_hit),
    ("mob_died",   s_mob_died),
    ("hurt",       s_hurt),
    ("died",       s_died),
    ("telegraph",  s_telegraph),
    ("hiss",       s_hiss),
    ("explode",    s_explode),
    ("arrow_fire", s_arrow_fire),
    ("arrow_hit",  s_arrow_hit),
    ("dusk",       s_dusk),
    ("dawn",       s_dawn),
    ("menu_move",  s_menu_move),
    ("buy",        s_buy),
    ("craft",      s_craft),
    ("craft_fail", s_craft_fail),
]


# ---- emit -------------------------------------------------------------------

def quantise(sig):
    return [max(-127, min(127, int(round(s * 127.0)))) for s in sig]


def camel(name):
    return "k" + "".join(p.capitalize() for p in name.split("_"))


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(SR)
        # wav's 8-bit format is unsigned; the header's is signed, which is what
        # playRaw takes. Converting here rather than storing unsigned keeps the
        # one format the firmware sees unambiguous.
        w.writeframes(bytes((s + 128) & 0xFF for s in pcm))


def emit_header(bank):
    L = []
    L.append("// " + "=" * 77)
    L.append("//  sfxdata.h — GENERATED by tools/make-sfx.py. Do not edit by hand.")
    L.append("//")
    L.append("//  The game's sound bank: 8-bit signed PCM, mono, one array per cue.")
    L.append("//  Signed because that is the overload Speaker::playRaw takes — its tone()")
    L.append("//  overload takes unsigned, and mixing the two up is a silent distortion bug")
    L.append("//  rather than an error.")
    L.append("//")
    L.append("//  These live in flash and are played in place: the speaker task reads")
    L.append("//  straight out of them, so the bank costs no RAM at all. That matters here —")
    L.append("//  the two 64.8 KB DMA framebuffers are already the tightest allocation in")
    L.append("//  the program.")
    L.append("//")
    L.append("//  Why rendered offline rather than synthesised on the device: see the")
    L.append("//  docstring of tools/make-sfx.py. Short version — the device's tone()")
    L.append("//  cannot produce an envelope, real noise, or a pitch sweep, and every sound")
    L.append("//  worth having needs at least one of the three.")
    L.append("// " + "=" * 77)
    L.append("#pragma once")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("namespace sfxdata {")
    L.append("")
    L.append("// Every asset shares one rate, so sfx.cpp can name it rather than carry it")
    L.append("// per cue. It is also the base that the per-play pitch jitter scales.")
    L.append("constexpr uint32_t RATE = %d;" % SR)
    L.append("")
    total = 0
    for name, pcm in bank:
        total += len(pcm)
        ms = int(round(len(pcm) * 1000.0 / SR))
        L.append("// %s — %d ms, %d bytes" % (name, ms, len(pcm)))
        L.append("static const int8_t %s[%d] = {" % (camel(name), len(pcm)))
        for i in range(0, len(pcm), 16):
            row = ",".join("%4d" % v for v in pcm[i:i + 16])
            L.append("  " + row + ",")
        L.append("};")
        L.append("")
    L.append("// bank total: %d bytes (%.1f KB)" % (total, total / 1024.0))
    L.append("")
    L.append("}  // namespace sfxdata")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-wav", action="store_true",
                    help="skip the preview wavs (the header is the only build input)")
    args = ap.parse_args()

    if not args.no_wav:
        PREVIEW.mkdir(parents=True, exist_ok=True)

    bank = []
    total = 0
    for name, fn in BANK:
        pcm = quantise(fn())
        bank.append((name, pcm))
        total += len(pcm)
        ms = len(pcm) * 1000.0 / SR
        print("  %-11s %6.0f ms  %6d bytes  peak %d"
              % (name, ms, len(pcm), max(abs(v) for v in pcm)))
        if not args.no_wav:
            write_wav(PREVIEW / (name + ".wav"), pcm)

    if not args.no_wav:
        # One file with the whole bank in it, half a second apart. Auditioning
        # twenty-one sounds one afplay at a time is how a bad one gets missed.
        sheet = []
        for _, pcm in bank:
            sheet.extend(pcm)
            sheet.extend([0] * (SR // 2))
        write_wav(PREVIEW / "_all.wav", sheet)

    HEADER.write_text(emit_header(bank))
    print("\n%s  %d cues, %d bytes (%.1f KB)"
          % (HEADER.relative_to(ROOT), len(bank), total, total / 1024.0))
    if not args.no_wav:
        print("%s/  previews — audition with afplay before flashing"
              % PREVIEW.relative_to(ROOT))


if __name__ == "__main__":
    main()
