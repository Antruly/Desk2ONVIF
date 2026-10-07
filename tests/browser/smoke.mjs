import { chromium } from 'playwright-core';
const b = await chromium.launch({ channel: 'chrome', headless: true });
const p = await b.newPage();
await p.setContent('<h1 id=x>ok</h1>');
console.log('title-check:', await p.textContent('#x'));
console.log('version:', b.version());
// H.264 支持探测：MSE 能不能加 avc1 的 SourceBuffer
const ms = await p.evaluate(async () => {
  const supported = (t) => window.MediaSource?.isTypeSupported?.(t) ?? null;
  return {
    hasMSE: typeof window.MediaSource !== 'undefined',
    baseline: supported('video/mp4; codecs="avc1.42E01E"'),
    main:     supported('video/mp4; codecs="avc1.4D401E"'),
    high:     supported('video/mp4; codecs="avc1.64001F"'),
    decoders: 'MediaSource' in window,
  };
});
console.log('mse:', JSON.stringify(ms, null, 2));
await b.close();
