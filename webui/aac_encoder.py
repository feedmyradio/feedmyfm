"""AAC-LC (ADTS) encoder for one station's PCM stream.

Only used when config ``listener.codec: aac``. webui builds one
:class:`StationEncoder` per station *port* on the first AAC listener and
drops it when the last one leaves; a ``pcm`` deployment never constructs
one.

The receiver's UDP output is unchanged raw s16le either way -- this turns
that byte stream into a sequence of self-syncing ADTS frames a browser
``<audio>`` element can play, which is what unlocks mobile background /
lock-screen playback.
"""

from __future__ import annotations

import logging
from fractions import Fraction

import av

logger = logging.getLogger(__name__)

# AAC-LC codes one 1024-sample frame at a time (32 ms at 32 kHz).
AAC_FRAME_SAMPLES = 1024

# AAC-LC runs at 32 kHz natively, which is plenty for de-emphasised,
# <=15 kHz FM audio. When the receiver's audio.rate is already 32 kHz the
# resampler below is only an s16 -> planar-float format conversion (what
# the ffmpeg `aac` encoder wants), not a rate change.
AAC_RATE = 32_000

# ADTS sampling_frequency_index (ISO/IEC 14496-3 table 1.16).
_SFI = {
    96000: 0,
    88200: 1,
    64000: 2,
    48000: 3,
    44100: 4,
    32000: 5,
    24000: 6,
    22050: 7,
    16000: 8,
    12000: 9,
    11025: 10,
    8000: 11,
}


class StationEncoder:
    """Stateful s16le -> ADTS/AAC-LC encoder for one station.

    Not thread-safe: :meth:`feed` mutates the resampler/FIFO/codec, so it
    must run on one thread at a time (webui drives it from a single
    thread-pool worker per encoder).
    """

    def __init__(self, *, stereo: bool, bitrate: int, in_rate: int = AAC_RATE):
        self.stereo = stereo
        self.channels = 2 if stereo else 1
        self._layout = "stereo" if stereo else "mono"
        self._frame_bytes = 2 * self.channels
        self._in_rate = in_rate or AAC_RATE
        self._sfi = _SFI[AAC_RATE]

        self._resampler = av.AudioResampler(
            format="fltp", layout=self._layout, rate=AAC_RATE
        )
        self._cc = av.CodecContext.create("aac", "w")
        self._cc.sample_rate = AAC_RATE
        self._cc.format = "fltp"
        self._cc.layout = self._layout
        self._cc.bit_rate = bitrate
        self._cc.time_base = Fraction(1, AAC_RATE)
        self._fifo = av.AudioFifo()

    def _adts(self, payload_len: int) -> bytes:
        """7-byte ADTS header (protection_absent = 1, so no CRC) for a
        payload of ``payload_len`` bytes -- makes each frame an
        independent, self-syncing unit."""
        n = 7 + payload_len
        c = self.channels
        return bytes(
            (
                0xFF,
                0xF1,  # syncword tail + MPEG-4 + layer 0 + protection absent
                ((1 & 3) << 6) | ((self._sfi & 0xF) << 2) | ((c >> 2) & 1),
                ((c & 3) << 6) | ((n >> 11) & 3),
                (n >> 3) & 0xFF,
                ((n & 7) << 5) | 0x1F,
                0xFC,
            )
        )

    def _drain_fifo(self) -> list[bytes]:
        out: list[bytes] = []
        while self._fifo.samples >= AAC_FRAME_SAMPLES:
            chunk = self._fifo.read(AAC_FRAME_SAMPLES)
            if chunk is None:  # guarded by the loop condition; keeps mypy happy
                break
            chunk.pts = None
            for pkt in self._cc.encode(chunk):
                payload = bytes(pkt)
                out.append(self._adts(len(payload)) + payload)
        return out

    def feed(self, pcm: bytes) -> list[bytes]:
        """Raw interleaved s16le -> zero or more complete ADTS frames.

        A datagram is rarely a whole number of 1024-sample frames; the
        remainder stays in the FIFO for the next call and is never
        dropped.
        """
        nsamp = len(pcm) // self._frame_bytes
        if nsamp:
            frame = av.AudioFrame(format="s16", layout=self._layout, samples=nsamp)
            frame.planes[0].update(pcm[: nsamp * self._frame_bytes])
            frame.sample_rate = self._in_rate
            frame.pts = None
            for rf in self._resampler.resample(frame):
                self._fifo.write(rf)
        return self._drain_fifo()

    def close(self) -> list[bytes]:
        """Flush the encoder; returns any trailing ADTS frames."""
        try:
            out: list[bytes] = []
            for pkt in self._cc.encode(None):
                payload = bytes(pkt)
                out.append(self._adts(len(payload)) + payload)
            return out
        except Exception:  # already flushed / EOF -- nothing left to emit
            return []
