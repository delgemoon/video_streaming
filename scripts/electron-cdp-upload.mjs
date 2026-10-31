#!/usr/bin/env node
// Drives the Electron renderer's Upload wizard over CDP: pick file, probe, capture poster, upload,
// then plays the video while it is still transcoding (LL-HLS live preview) and after it is ready.
// Usage: electron-cdp-upload.mjs <debug-port> <absolute video path>
const port = process.argv[2] || '9333'
const file = process.argv[3]
if (!file) throw new Error('usage: electron-cdp-upload.mjs <port> <file>')

const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json()
const page = targets.find((t) => t.type === 'page' && t.url.includes('localhost:5173'))
const ws = new WebSocket(page.webSocketDebuggerUrl)
await new Promise((r) => (ws.onopen = r))
let id = 0
const pending = new Map()
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data)
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id) }
}
const send = (method, params = {}) => new Promise((resolve) => { const i = ++id; pending.set(i, resolve); ws.send(JSON.stringify({ id: i, method, params })) })
const ev = async (expression) => {
  const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
  if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || JSON.stringify(r.result.exceptionDetails))
  return r.result?.result?.value
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const clickText = (sel, text) => ev(`(() => { const b=[...document.querySelectorAll(${JSON.stringify(sel)})].find(x=>x.textContent.trim().includes(${JSON.stringify(text)})); if(!b) throw new Error('no element '+${JSON.stringify(text)}); b.click(); return true })()`)

await ev(`(() => { document.querySelector('.player .btn:last-child')?.click(); return true })()`)
await clickText('.nav-item', 'Upload')
await sleep(500)

// Set the hidden <input type=file> through CDP
await send('DOM.enable')
const doc = await send('DOM.getDocument', { depth: 1 })
const q = await send('DOM.querySelector', { nodeId: doc.result.root.nodeId, selector: 'input[type=file]' })
await send('DOM.setFileInputFiles', { nodeId: q.result.nodeId, files: [file] })
await sleep(2500)
console.log('probe:', await ev(`document.querySelector('.wizard-preview .kv')?.innerText.replace(/\\n/g,' | ')`))

await ev(`(() => { const v=document.querySelector('.preview-box video'); v.currentTime = 12; return true })()`)
await sleep(800)
await clickText('button', 'Capture current frame')
await sleep(800)
console.log('poster:', await ev(`document.querySelector('.poster-preview span')?.innerText`))
await ev(`(() => { const i=[...document.querySelectorAll('.trim-row input')]; const set=(el,v)=>{const s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set; s.call(el,v); el.dispatchEvent(new Event('input',{bubbles:true}))}; set(i[0],'2000'); set(i[1],'62000'); return true })()`)
await sleep(300)
console.log('trim:', await ev(`document.querySelector('.range-track')?.nextElementSibling?.innerText.replace(/\\n/g,' ')`))
await clickText('.chip', '240p')   // drop the 240p rendition
console.log('renditions on:', await ev(`[...document.querySelectorAll('.chip.on')].map(c=>c.textContent.trim().split(' ')[0]).join(',')`))

await clickText('button', 'Upload & transcode')
for (let i = 0; i < 30; i++) {
  await sleep(500)
  const s = await ev(`document.querySelector('.progress.big .progress-label')?.innerText || document.querySelector('.ok-panel')?.innerText || document.querySelector('.banner.error')?.innerText || ''`)
  if (s) console.log('upload:', s)
  if (/Uploaded|error/i.test(s)) break
}
await sleep(1500)

// Library: wait for LIVE PREVIEW, play while transcoding
let live = false
for (let i = 0; i < 40; i++) {
  await sleep(500)
  const st = await ev(`(() => { const c=document.querySelector('.card'); return c ? { live: !!c.querySelector('.live-tag'), status: c.querySelector('.badge')?.innerText, prog: c.querySelector('.progress-label')?.innerText, thumb: !!c.querySelector('img') } : null })()`)
  if (st?.live) { console.log('card:', JSON.stringify(st)); live = true; break }
}
if (!live) console.log('WARN: live preview never appeared')
await ev(`(() => { const c=document.querySelector('.card'); const b=[...c.querySelectorAll('button')].find(b=>b.textContent.trim()==='Play'); if(!b) throw new Error('no play'); b.click(); return true })()`)
let liveOk = false, vodOk = false
for (let i = 0; i < 90; i++) {
  await sleep(1000)
  const s = await ev(`(() => { const v=document.querySelector('.player video'); if(!v) return null; const q=v.getVideoPlaybackQuality(); return { t:+v.currentTime.toFixed(1), ready:v.readyState, w:v.videoWidth, decoded:q.totalVideoFrames, err:v.error&&v.error.message, stats:(document.querySelector('.stats-overlay')||{}).innerText?.replace(/\\n/g,' | '), hdr:document.querySelector('.player-header .small')?.innerText } })()`)
  if (i % 5 === 0 || (s && s.err)) console.log(JSON.stringify(s))
  if (s && s.stats?.includes('EVENT') && s.decoded > 30 && s.w > 0) liveOk = true
  if (s && s.stats?.includes('VOD') && s.decoded > 30 && s.w > 0 && s.hdr && !s.hdr.includes('LIVE')) { vodOk = true; console.log(JSON.stringify(s)); break }
  if (s && s.err) break
}
console.log(liveOk ? 'LIVE PREVIEW PLAYBACK OK' : 'live preview playback not observed')
console.log(vodOk ? 'FINAL VOD PLAYBACK OK' : 'FINAL VOD PLAYBACK FAILED')
ws.close()
process.exit(vodOk ? 0 : 2)
