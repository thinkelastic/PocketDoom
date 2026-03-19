#!/usr/bin/env python3
"""
Reproduce PocketDoom's MUS→OPL register writes and dump to VGM file.
Compare the resulting audio to DOSBox to find driver bugs.

Usage: python3 tools/mus2vgm.py DOOM.WAD [D_E1M2] > output.vgm
  Then play with: vgmplay output.vgm  (or any VGM player)
"""
import struct, sys, os

# ---- GENMIDI structures (matches doom_music.c) ----
GENMIDI_NUM_INSTRS = 175
PERCUSSION_BASE = 128
PERCUSSION_CHAN = 15

def parse_genmidi_op(data, off):
    return {
        'tremolo': data[off], 'attack': data[off+1], 'sustain': data[off+2],
        'waveform': data[off+3], 'scale': data[off+4], 'level': data[off+5]
    }

def parse_genmidi_voice(data, off):
    mod = parse_genmidi_op(data, off)
    fb = data[off+6]
    car = parse_genmidi_op(data, off+7)
    unused = data[off+13]
    base_note_offset = struct.unpack_from('<h', data, off+14)[0]
    return {'modulator': mod, 'feedback': fb, 'carrier': car, 'base_note_offset': base_note_offset}

def parse_genmidi(data):
    if data[:8] != b'#OPL_II#':
        raise ValueError("Bad GENMIDI header")
    instrs = []
    for i in range(GENMIDI_NUM_INSTRS):
        off = 8 + i * 36
        flags = struct.unpack_from('<H', data, off)[0]
        fine_tune = data[off+2]
        fixed_note = data[off+3]
        v0 = parse_genmidi_voice(data, off+4)
        v1 = parse_genmidi_voice(data, off+20)
        instrs.append({'flags': flags, 'fine_tune': fine_tune, 'fixed_note': fixed_note, 'voice': [v0, v1]})
    return instrs

# ---- Volume curve (from Chocolate Doom) ----
volume_curve = [
      0,   1,   3,   5,   6,   8,  10,  11,  13,  14,  16,  17,  19,  20,  22,  23,
     25,  26,  27,  29,  30,  32,  33,  34,  36,  37,  39,  41,  43,  45,  47,  49,
     50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,  68,  69,  71,  72,
     73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,  85,  86,  87,  88,
     89,  90,  91,  92,  92,  93,  94,  95,  96,  96,  97,  98,  99,  99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106, 107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127
]

# ---- OPL3 register tables (18 channels) ----
op1_off = [0x000,0x001,0x002,0x008,0x009,0x00A,0x010,0x011,0x012,
           0x100,0x101,0x102,0x108,0x109,0x10A,0x110,0x111,0x112]
op2_off = [0x003,0x004,0x005,0x00B,0x00C,0x00D,0x013,0x014,0x015,
           0x103,0x104,0x105,0x10B,0x10C,0x10D,0x113,0x114,0x115]
ch_base = [0x000,0x001,0x002,0x003,0x004,0x005,0x006,0x007,0x008,
           0x100,0x101,0x102,0x103,0x104,0x105,0x106,0x107,0x108]

fnum_table = [0x157, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA,
              0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287]

NUM_OPL_VOICES = 18
MUS_CHANNELS = 16

# ---- VGM output ----
class VGMWriter:
    def __init__(self):
        self.cmds = bytearray()
        self.samples = 0  # total samples at 44100 Hz

    def opl3_write(self, reg, val):
        if reg & 0x100:
            self.cmds += bytes([0x5F, reg & 0xFF, val])  # OPL3 bank 1
        else:
            self.cmds += bytes([0x5E, reg & 0xFF, val])  # OPL3 bank 0

    def wait(self, samples_44100):
        self.samples += samples_44100
        while samples_44100 > 0:
            n = min(samples_44100, 65535)
            self.cmds += bytes([0x61, n & 0xFF, (n >> 8) & 0xFF])
            samples_44100 -= n

    def finish(self):
        self.cmds += bytes([0x66])  # end of data
        # Build VGM header (minimal)
        data_offset = 0x80 - 0x34  # data starts at 0x80
        hdr = bytearray(0x80)
        struct.pack_into('<4s', hdr, 0x00, b'Vgm ')
        struct.pack_into('<I', hdr, 0x04, len(hdr) + len(self.cmds) - 4)  # EOF offset
        struct.pack_into('<I', hdr, 0x08, 0x00000171)  # version 1.71
        struct.pack_into('<I', hdr, 0x18, self.samples)  # total samples
        struct.pack_into('<I', hdr, 0x34, data_offset)  # VGM data offset
        struct.pack_into('<I', hdr, 0x5C, 14318180)  # YMF262 clock
        return bytes(hdr) + bytes(self.cmds)

# ---- OPL driver (matches doom_music.c logic) ----
class OPLDriver:
    def __init__(self, genmidi, vgm, music_volume=127):
        self.genmidi = genmidi
        self.vgm = vgm
        self.music_volume = music_volume
        self.voices = [{'active': False, 'mus_channel': 0, 'note': 0, 'instrument': 0,
                        'car_level': 0, 'mod_level': 0, 'car_scale': 0, 'mod_scale': 0,
                        'connection': 0} for _ in range(NUM_OPL_VOICES)]
        self.voice_reg_b0 = [0] * NUM_OPL_VOICES
        self.chan_instr = [0] * MUS_CHANNELS
        self.chan_volume = [100] * MUS_CHANNELS
        self.init_opl()

    def opl_write(self, reg, val):
        self.vgm.opl3_write(reg, val)

    def init_opl(self):
        for i in range(1, 0xF6):
            self.opl_write(i, 0)
        self.opl_write(0x105, 0x01)  # OPL3 mode
        for i in range(0x101, 0x1F6):
            self.opl_write(i, 0)
        self.opl_write(0x104, 0x00)  # no 4-op
        self.opl_write(0x01, 0x20)
        self.opl_write(0x101, 0x20)
        self.opl_write(0x08, 0x40)   # NOTE-SEL
        self.opl_write(0x108, 0x40)
        self.opl_write(0xBD, 0x00)

    def load_operator(self, ch, op, is_carrier):
        off = op2_off[ch] if is_carrier else op1_off[ch]
        self.opl_write(0x20 + off, op['tremolo'])
        self.opl_write(0x40 + off, op['scale'] | 0x3F)
        self.opl_write(0x60 + off, op['attack'])
        self.opl_write(0x80 + off, op['sustain'])
        self.opl_write(0xE0 + off, op['waveform'])

    def set_instrument(self, ch, v):
        self.load_operator(ch, v['modulator'], False)
        self.load_operator(ch, v['carrier'], True)
        self.opl_write(0xC0 + ch_base[ch], (v['feedback'] & 0x0F) | 0x30)
        self.voices[ch]['car_level'] = v['carrier']['level'] & 0x3F
        self.voices[ch]['mod_level'] = v['modulator']['level'] & 0x3F
        self.voices[ch]['car_scale'] = v['carrier']['scale'] & 0xC0
        self.voices[ch]['mod_scale'] = v['modulator']['scale'] & 0xC0
        self.voices[ch]['connection'] = v['feedback'] & 0x01

    def set_volume(self, ch, velocity):
        vel = velocity & 0x7F
        capped = min(vel, self.music_volume)
        midi_vol = 2 * (volume_curve[capped] + 1)
        full_vol = (volume_curve[vel] * midi_vol) >> 9
        if full_vol > 0x3F: full_vol = 0x3F
        car_tl = 0x3F - full_vol
        self.opl_write(0x40 + op2_off[ch], self.voices[ch]['car_scale'] | (car_tl & 0x3F))
        if self.voices[ch]['connection']:
            mod_tl = self.voices[ch]['mod_level']
            if mod_tl < car_tl:
                mod_tl = car_tl
            self.opl_write(0x40 + op1_off[ch], self.voices[ch]['mod_scale'] | (mod_tl & 0x3F))
        else:
            self.opl_write(0x40 + op1_off[ch], self.voices[ch]['mod_scale'] | (self.voices[ch]['mod_level'] & 0x3F))

    def set_frequency(self, ch, note, key_on):
        semi = note % 12
        block = (note // 12) - 1
        fnum = fnum_table[semi]
        if block < 0:
            fnum >>= (-block)
            block = 0
        if block > 7:
            block = 7
        b0 = (0x20 if key_on else 0x00) | ((block & 7) << 2) | ((fnum >> 8) & 0x03)
        self.opl_write(0xA0 + ch_base[ch], fnum & 0xFF)
        self.opl_write(0xB0 + ch_base[ch], b0)
        self.voice_reg_b0[ch] = b0

    def voice_key_off(self, ch):
        self.opl_write(0xB0 + ch_base[ch], self.voice_reg_b0[ch] & ~0x20)
        self.voice_reg_b0[ch] &= ~0x20

    def alloc_voice(self, mus_channel):
        for i in range(NUM_OPL_VOICES):
            if not self.voices[i]['active']:
                return i
        for i in range(NUM_OPL_VOICES):
            if self.voices[i]['mus_channel'] == mus_channel:
                return i
        return 0

    def note_on(self, mus_channel, note, velocity):
        if mus_channel == PERCUSSION_CHAN:
            instr_idx = note - 35 + PERCUSSION_BASE
            if instr_idx < PERCUSSION_BASE or instr_idx >= GENMIDI_NUM_INSTRS:
                return
            play_note = note
        else:
            instr_idx = self.chan_instr[mus_channel]
            if instr_idx < 0 or instr_idx >= 128:
                return
            play_note = note

        instr = self.genmidi[instr_idx]
        if instr['flags'] & 1:  # FIXED
            play_note = instr['fixed_note']
        play_note += instr['voice'][0]['base_note_offset']
        play_note = max(0, min(127, play_note))

        idx = self.alloc_voice(mus_channel)
        if self.voices[idx]['active']:
            self.voice_key_off(idx)

        self.set_instrument(idx, instr['voice'][0])
        self.set_volume(idx, velocity)
        self.set_frequency(idx, play_note, True)

        self.voices[idx]['active'] = True
        self.voices[idx]['mus_channel'] = mus_channel
        self.voices[idx]['note'] = note
        self.voices[idx]['instrument'] = instr_idx

    def note_off(self, mus_channel, note):
        for i in range(NUM_OPL_VOICES):
            v = self.voices[i]
            if v['active'] and v['mus_channel'] == mus_channel and v['note'] == note:
                self.voice_key_off(i)
                v['active'] = False
                break

# ---- MUS parser ----
def play_mus(mus_data, driver, max_seconds=30):
    if mus_data[:4] != b'MUS\x1a':
        raise ValueError("Bad MUS header")
    score_len = struct.unpack_from('<H', mus_data, 4)[0]
    score_start = struct.unpack_from('<H', mus_data, 6)[0]

    pos = score_start
    total_ticks = 0
    samples_per_tick = 44100 / 140  # MUS tick rate = 140 Hz, VGM uses 44100 Hz
    max_ticks = max_seconds * 140

    while pos < len(mus_data) and total_ticks < max_ticks:
        header = mus_data[pos]; pos += 1
        last = header & 0x80
        etype = (header >> 4) & 0x07
        channel = header & 0x0F

        if etype == 0:  # Release
            b1 = mus_data[pos]; pos += 1
            driver.note_off(channel, b1 & 0x7F)
        elif etype == 1:  # Play note
            b1 = mus_data[pos]; pos += 1
            if b1 & 0x80:
                b2 = mus_data[pos]; pos += 1
                driver.chan_volume[channel] = b2 & 0x7F
            driver.note_on(channel, b1 & 0x7F, driver.chan_volume[channel])
        elif etype == 2:  # Pitch bend
            pos += 1
        elif etype == 3:  # System event
            pos += 1
        elif etype == 4:  # Controller
            b1 = mus_data[pos]; pos += 1
            b2 = mus_data[pos]; pos += 1
            if b1 == 0:
                driver.chan_instr[channel] = b2
            elif b1 == 3:
                driver.chan_volume[channel] = b2
        elif etype == 6:  # Score end
            break

        if last:
            delay = 0
            while True:
                b = mus_data[pos]; pos += 1
                delay = (delay << 7) | (b & 0x7F)
                if not (b & 0x80):
                    break
            total_ticks += delay
            driver.vgm.wait(int(delay * samples_per_tick))

# ---- Main ----
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} DOOM.WAD [lump_name]", file=sys.stderr)
        sys.exit(1)

    wad_path = sys.argv[1]
    lump_name = sys.argv[2] if len(sys.argv) > 2 else 'D_E1M2'

    with open(wad_path, 'rb') as f:
        magic, numlumps, diroff = struct.unpack('<4sII', f.read(12))
        f.seek(diroff)
        lumps = {}
        for _ in range(numlumps):
            pos, size, name = struct.unpack('<II8s', f.read(16))
            name = name.rstrip(b'\x00').decode()
            lumps[name] = (pos, size)

        # Load GENMIDI
        gm_pos, gm_size = lumps['GENMIDI']
        f.seek(gm_pos)
        genmidi = parse_genmidi(f.read(gm_size))

        # Load MUS
        mus_pos, mus_size = lumps[lump_name]
        f.seek(mus_pos)
        mus_data = f.read(mus_size)

    print(f"Loaded {len(genmidi)} instruments, MUS '{lump_name}' ({len(mus_data)} bytes)", file=sys.stderr)

    vgm = VGMWriter()
    driver = OPLDriver(genmidi, vgm)
    play_mus(mus_data, driver, max_seconds=60)

    out = vgm.finish()
    sys.stdout.buffer.write(out)
    print(f"Wrote {len(out)} bytes VGM ({vgm.samples} samples)", file=sys.stderr)

if __name__ == '__main__':
    main()
