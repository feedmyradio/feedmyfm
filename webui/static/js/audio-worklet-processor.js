// Runs on the dedicated audio render thread. Receives Float32 PCM chunks
// (already deinterleaved per channel) via postMessage from player.js and
// plays them back through a small per-channel ring buffer, absorbing
// normal network jitter without adding noticeable delay for live radio.
// On underrun (network stall), outputs silence rather than glitching.

class RingBuffer {
  constructor(capacitySamples) {
    this.capacity = capacitySamples;
    this.buffer = new Float32Array(capacitySamples);
    this.writeIndex = 0;
    this.readIndex = 0;
    this.available = 0;
  }

  write(samples) {
    for (let i = 0; i < samples.length; i++) {
      this.buffer[this.writeIndex] = samples[i];
      this.writeIndex = (this.writeIndex + 1) % this.capacity;
      if (this.available < this.capacity) {
        this.available++;
      } else {
        // buffer full -- drop the oldest sample to make room for the newest
        this.readIndex = (this.readIndex + 1) % this.capacity;
      }
    }
  }

  read(out) {
    for (let i = 0; i < out.length; i++) {
      if (this.available > 0) {
        out[i] = this.buffer[this.readIndex];
        this.readIndex = (this.readIndex + 1) % this.capacity;
        this.available--;
      } else {
        out[i] = 0;
      }
    }
  }
}

class FeedMyFMProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const opts = options.processorOptions || {};
    const channels = opts.channels || 1;
    const sampleRate = opts.sampleRate || 32000;
    const capacity = Math.round(sampleRate * 0.5); // ~0.5s per channel

    this.channelBuffers = Array.from({ length: channels }, () => new RingBuffer(capacity));

    this.port.onmessage = (event) => {
      const channelData = event.data; // array of Float32Array, one per channel
      for (let ch = 0; ch < this.channelBuffers.length; ch++) {
        if (channelData[ch]) {
          this.channelBuffers[ch].write(channelData[ch]);
        }
      }
    };
  }

  process(_inputs, outputs) {
    const output = outputs[0];
    for (let ch = 0; ch < output.length; ch++) {
      const buf = this.channelBuffers[ch] || this.channelBuffers[0];
      buf.read(output[ch]);
    }
    return true;
  }
}

registerProcessor("feedmyfm-processor", FeedMyFMProcessor);
