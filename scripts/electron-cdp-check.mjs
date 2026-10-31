#!/usr/bin/env node
// Drives the running Electron renderer over CDP (no dependencies) to verify real hls.js playback.
// Usage: electron-cdp-check.mjs <debug-port> [video title substring]
const port = process.argv[2] || '9333'
const title = process.argv[3] || ''

const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json()
const page = targets.find((t) => t.type === 'page' && t.url.includes('localhost:5173'))
if (!page) {
  console.error('renderer page not found; targets:', targets.map((t) => t.url))
  process.exit(1)
}
const ws = new WebSocket(page.webSocketDebuggerUrl)
await new Promise((r) => (ws.onopen = r))
let id = 0
const pending = new Map()
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data)
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg)
    pending.delete(msg.id)
  }
}
const send = (method, params = {}) =>
  new Promise((resolve) => {
    const i = ++id
    pending.set(i, resolve)
    ws.send(JSON.stringify({ id: i, method, params }))
  })
const evaluate = async (expression) => {
  const res = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
  if (res.result?.exceptionDetails) throw new Error(JSON.stringify(res.result.exceptionDetails))
  return res.result?.result?.value
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

await evaluate(`(async () => {
  const cards = [...document.querySelectorAll('.card')];
  const card = cards.find(c => c.innerText.includes(${JSON.stringify(title)})) || cards[0];
  if (!card) throw new Error('no video cards');
  const btn = [...card.querySelectorAll('button')].find(b => b.textContent.trim() === 'Play');
  if (!btn) throw new Error('no Play button on card: ' + card.innerText.slice(0, 80));
  btn.click();
})()`)

const samples = []
for (let i = 0; i < 12; i++) {
  await sleep(1000)
  samples.push(
    await evaluate(`(() => {
      const v = document.querySelector('.player video');
      if (!v) return null;
      const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
      return { t: +v.currentTime.toFixed(2), paused: v.paused, ready: v.readyState, w: v.videoWidth, h: v.videoHeight,
               err: v.error ? v.error.message : null, decoded: q.totalVideoFrames, dropped: q.droppedVideoFrames,
               stats: (document.querySelector('.stats-overlay')||{}).innerText, perr: (document.querySelector('.player-error')||{}).innerText };
    })()`)
  )
}
for (const s of samples) console.log(JSON.stringify(s))
const last = samples.at(-1)
const ok = last && !last.err && last.t > 5 && last.decoded > 100 && last.w > 0
console.log(ok ? 'PLAYBACK OK' : 'PLAYBACK FAILED')
ws.close()
process.exit(ok ? 0 : 2)
