#!/usr/bin/env node
// Drives the Electron renderer over CDP: opens the Live view, clicks Watch on the live stream,
// and samples hls.js playback state (latency, level, decoded frames) for a while.
// Usage: electron-cdp-live.mjs <debug-port> [seconds]
const port = process.argv[2] || '9333'
const seconds = Number(process.argv[3] || 20)

const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json()
const page = targets.find((t) => t.type === 'page' && t.url.includes('localhost:5173'))
if (!page) throw new Error('renderer page not found')
const ws = new WebSocket(page.webSocketDebuggerUrl)
await new Promise((r) => (ws.onopen = r))
let id = 0
const pending = new Map()
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id) } }
const send = (method, params = {}) => new Promise((resolve) => { const i = ++id; pending.set(i, resolve); ws.send(JSON.stringify({ id: i, method, params })) })
const ev = async (expression) => {
  const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
  if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || JSON.stringify(r.result.exceptionDetails))
  return r.result?.result?.value
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

await ev(`(() => { document.querySelector('.player .btn:last-child')?.click(); return true })()`)
await ev(`(() => { const b=[...document.querySelectorAll('.nav-item')].find(x=>x.textContent.includes('Live')); b.click(); return true })()`)
await sleep(800)
console.log('live view:', await ev(`[...document.querySelectorAll('.live-card')].map(c => c.querySelector('h3').innerText + ' [' + (c.querySelector('.live-tag') ? 'LIVE' : 'idle') + ']').join(', ')`))
console.log('stats row:', await ev(`document.querySelector('.live-card.is-live .live-stats')?.innerText.replace(/\\n/g,' | ')`))
const watched = await ev(`(() => { const b=[...document.querySelectorAll('.live-card.is-live button')].find(x=>x.textContent.trim()==='Watch'); if(!b) return false; b.click(); return true })()`)
if (!watched) { console.log('no live stream to watch'); process.exit(2) }

let ok = false, last
for (let i = 0; i < seconds; i++) {
  await sleep(1000)
  last = await ev(`(() => { const v=document.querySelector('.player video'); if(!v) return null; const q=v.getVideoPlaybackQuality();
     return { t:+v.currentTime.toFixed(1), paused:v.paused, ready:v.readyState, w:v.videoWidth, h:v.videoHeight, decoded:q.totalVideoFrames, dropped:q.droppedVideoFrames,
              err:v.error&&v.error.message, stats:(document.querySelector('.stats-overlay')||{}).innerText?.replace(/\\n/g,' | ') } })()`)
  if (i % 3 === 0 || (last && last.err)) console.log(JSON.stringify(last))
  if (last && last.decoded > 60 && !last.paused && last.w > 0 && !last.err) ok = true
  if (last && last.err) break
}
const m = last?.stats?.match(/latency: ([\d.]+) s/)
console.log(m ? `hls.js reported live latency: ${m[1]} s` : 'no latency reading')
console.log(ok ? 'LIVE PLAYBACK OK' : 'LIVE PLAYBACK FAILED')
ws.close()
process.exit(ok ? 0 : 2)
