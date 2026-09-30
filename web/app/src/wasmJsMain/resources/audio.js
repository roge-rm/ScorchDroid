// The browser's sound: what SoundPlayer, MusicPlayer and AmbientPlayer do on a
// phone, on Web Audio. The Kotlin side (WebAudio.kt) decides what plays and
// when; this plays it. Files come from the engine's file system (globalThis.sd),
// where the game data is preloaded, and are decoded once and kept.
//
// A page may only start sound after the player has touched it, so the context
// is made (or resumed) on the first pointer or key press.
globalThis.sdAudio = (() => {
  let ctx = null;
  let effects = null; // the effects bus, under the effects volume
  let effectsOn = true;
  const buffers = new Map();  // path -> AudioBuffer, or null when it can't be decoded
  const pending = new Map();  // path -> Promise of the above
  const loops = new Map();    // key -> the loop's nodes
  const tracks = new Map();   // id -> a music or ambient track's nodes
  let nextTrack = 1;

  function context() {
    if (!ctx) {
      ctx = new AudioContext();
      effects = ctx.createGain();
      effects.connect(ctx.destination);
    }
    return ctx;
  }

  function wake() {
    const c = context();
    if (c.state === 'suspended') c.resume();
  }
  addEventListener('pointerdown', wake, true);
  addEventListener('keydown', wake, true);

  function buffer(path) {
    if (buffers.has(path)) return Promise.resolve(buffers.get(path));
    if (pending.has(path)) return pending.get(path);
    let bytes;
    try {
      bytes = globalThis.sd.FS.readFile(path);
    } catch (e) {
      console.warn('ScorchDroidSound: no file ' + path);
      buffers.set(path, null);
      return Promise.resolve(null);
    }
    const p = context().decodeAudioData(bytes.slice().buffer).then(
      (b) => { buffers.set(path, b); pending.delete(path); return b; },
      () => { console.warn('ScorchDroidSound: can\'t decode ' + path); buffers.set(path, null); pending.delete(path); return null; },
    );
    pending.set(path, p);
    return p;
  }

  function voice(b, gain, pan, rate, loop, out) {
    const c = context();
    const source = c.createBufferSource();
    source.buffer = b;
    source.loop = loop;
    source.playbackRate.value = rate;
    const g = c.createGain();
    g.gain.value = gain;
    const p = c.createStereoPanner();
    p.pan.value = Math.max(-1, Math.min(1, pan));
    source.connect(g).connect(p).connect(out);
    source.start();
    return { source, gain: g, pan: p };
  }

  return {
    setEffects(on, volume) {
      effectsOn = on;
      context();
      effects.gain.value = on ? volume : 0;
      if (!on) this.stopAllLoops();
    },
    preload(path) { buffer(path); },
    play(path, gain, pan) {
      if (!effectsOn) return;
      buffer(path).then((b) => { if (b) voice(b, gain, pan, 1, false, effects); });
    },
    startLoop(key, path, gain, pan, rate) {
      this.stopLoop(key);
      if (!effectsOn) return;
      const entry = { stopped: false };
      loops.set(key, entry);
      buffer(path).then((b) => {
        if (!b || entry.stopped) return;
        Object.assign(entry, voice(b, gain, pan, rate, true, effects));
      });
    },
    updateLoop(key, gain, pan, rate) {
      const e = loops.get(key);
      if (!e || !e.source) return;
      e.gain.gain.value = gain;
      e.pan.pan.value = Math.max(-1, Math.min(1, pan));
      e.source.playbackRate.value = rate;
    },
    stopLoop(key) {
      const e = loops.get(key);
      if (!e) return;
      e.stopped = true;
      if (e.source) { try { e.source.stop(); } catch (x) {} }
      loops.delete(key);
    },
    stopAllLoops() {
      for (const key of [...loops.keys()]) this.stopLoop(key);
    },

    // Music and ambient tracks: long loops with their own level, faded in
    // and out. Returns the track's id.
    startTrack(path, level, fadeSeconds, loop) {
      const id = nextTrack++;
      const entry = { stopped: false, level };
      tracks.set(id, entry);
      buffer(path).then((b) => {
        if (!b || entry.stopped) return;
        const c = context();
        const v = voice(b, 0, 0, 1, loop, c.destination);
        v.gain.gain.setValueAtTime(0, c.currentTime);
        v.gain.gain.linearRampToValueAtTime(entry.level, c.currentTime + fadeSeconds);
        Object.assign(entry, v);
        if (!loop) v.source.onended = () => tracks.delete(id);
      });
      return id;
    },
    setTrackLevel(id, level) {
      const e = tracks.get(id);
      if (!e) return;
      e.level = level;
      if (e.gain) e.gain.gain.setValueAtTime(level, context().currentTime);
    },
    stopTrack(id, fadeSeconds) {
      const e = tracks.get(id);
      if (!e) return;
      e.stopped = true;
      tracks.delete(id);
      if (!e.source) return;
      const c = context();
      e.gain.gain.cancelScheduledValues(c.currentTime);
      e.gain.gain.setValueAtTime(e.gain.gain.value, c.currentTime);
      e.gain.gain.linearRampToValueAtTime(0, c.currentTime + fadeSeconds);
      try { e.source.stop(c.currentTime + fadeSeconds); } catch (x) {}
    },
    exists(path) {
      try { return globalThis.sd.FS.analyzePath(path).exists; } catch (e) { return false; }
    },
  };
})();
