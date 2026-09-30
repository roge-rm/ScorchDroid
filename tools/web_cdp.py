#!/usr/bin/env python3
"""Drives headless Chromium for checking the browser game without a person.

Opens a page, prints its console as it goes, and at set times clicks, drags,
scrolls, presses keys, runs JavaScript and takes screenshots. SwiftShader does
the rendering, so expect about 5 frames a second.

    python3 tools/web_cdp.py http://127.0.0.1:8940/ --wait 60 \
        --click 8:640,261 --click 10:640,300 --click 12:458,110 \
        --key 38:ArrowRight:1.0 --key 45:Space:0.1 --shots 50:/tmp/shot.png

Serve the page first (python3 -m http.server in
web/app/build/dist/wasmJs/productionExecutable). Needs chromium and the
websockets package: pip install --target <dir> websockets, then set
PYTHONPATH=<dir>. Times are seconds from opening the page.
"""
import asyncio, base64, json, os, subprocess, sys, time, urllib.request, argparse, tempfile
import websockets

ap = argparse.ArgumentParser()
ap.add_argument('url')
ap.add_argument('--wait', type=float, default=20)
ap.add_argument('--at', action='append', default=[], help='T:JS run at T seconds')
ap.add_argument('--shots', action='append', default=[], help='T:PATH screenshot at T seconds')
ap.add_argument('--click', action='append', default=[], help='T:X,Y left click at T seconds')
ap.add_argument('--drag', action='append', default=[], help='T:X1,Y1,X2,Y2 left drag')
ap.add_argument('--wheel', action='append', default=[], help='T:X,Y,DY')
ap.add_argument('--key', action='append', default=[], help='T:KEY:HOLD e.g. 30:ArrowRight:1.0 (KEY from KEYS)')
ap.add_argument('--size', default='1280,720')
ap.add_argument('--port', type=int, default=9333)
ap.add_argument('--quiet', action='store_true')
ap.add_argument('--gpu', action='store_true', help='use the real GPU instead of SwiftShader')
a = ap.parse_args()

prof = tempfile.mkdtemp(prefix='cdp-')
flags = ['--headless=new', f'--remote-debugging-port={a.port}', f'--user-data-dir={prof}',
         f'--window-size={a.size}', '--no-first-run', '--no-default-browser-check',
         '--autoplay-policy=no-user-gesture-required', '--ignore-certificate-errors']
if a.gpu:
    flags += ['--enable-gpu', '--ignore-gpu-blocklist', '--use-angle=vulkan', '--enable-features=Vulkan']
else:
    flags += ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']
proc = subprocess.Popen(['chromium', *flags, 'about:blank'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

async def main():
    for _ in range(100):
        try:
            tabs = json.load(urllib.request.urlopen(f'http://127.0.0.1:{a.port}/json'))
            page = next(t for t in tabs if t['type'] == 'page')
            break
        except Exception:
            await asyncio.sleep(0.1)
    async with websockets.connect(page['webSocketDebuggerUrl'], max_size=None) as ws:
        seq = 0
        pending = {}
        async def call(method, params=None):
            nonlocal seq
            seq += 1
            fut = asyncio.get_event_loop().create_future()
            pending[seq] = fut
            await ws.send(json.dumps({'id': seq, 'method': method, 'params': params or {}}))
            return await fut
        async def reader():
            async for raw in ws:
                m = json.loads(raw)
                if 'id' in m and m['id'] in pending:
                    pending.pop(m['id']).set_result(m.get('result', m.get('error')))
                elif m.get('method') == 'Runtime.consoleAPICalled':
                    args = ' '.join(str(x.get('value', x.get('description', ''))) for x in m['params']['args'])
                    if not a.quiet: print(f'[{time.time()-t0:6.1f}] console.{m["params"]["type"]}: {args}', flush=True)
                elif m.get('method') == 'Runtime.exceptionThrown':
                    d = m['params']['exceptionDetails']
                    print(f'[{time.time()-t0:6.1f}] EXCEPTION: {d.get("exception", {}).get("description", d.get("text"))}', flush=True)
        t0 = time.time()
        asyncio.get_event_loop().create_task(reader())
        await call('Runtime.enable')
        await call('Page.enable')
        await call('Page.navigate', {'url': a.url})
        events = []
        for s in a.at:
            t, js = s.split(':', 1); events.append((float(t), 'js', js))
        for s in a.shots:
            t, p = s.split(':', 1); events.append((float(t), 'shot', p))
        for s_ in a.click:
            t, xy = s_.split(':', 1); events.append((float(t), 'click', xy))
        for s_ in a.drag:
            t, v = s_.split(':', 1); events.append((float(t), 'drag', v))
        for s_ in a.wheel:
            t, v = s_.split(':', 1); events.append((float(t), 'wheel', v))
        for s_ in a.key:
            t, v = s_.split(':', 1); events.append((float(t), 'key', v))
        events.sort(key=lambda e: e[0])
        for t, kind, arg in events:
            delay = t - (time.time() - t0)
            if delay > 0: await asyncio.sleep(delay)
            if kind == 'js':
                r = await call('Runtime.evaluate', {'expression': arg, 'awaitPromise': True, 'returnByValue': True})
                print(f'[{time.time()-t0:6.1f}] eval {arg[:60]!r} -> {json.dumps(r.get("result", r))[:2000]}', flush=True)
            elif kind == 'drag':
                x1, y1, x2, y2 = [float(v) for v in arg.split(',')]
                await call('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': x1, 'y': y1})
                await call('Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': x1, 'y': y1, 'button': 'left', 'buttons': 1, 'clickCount': 1})
                for i in range(1, 16):
                    await asyncio.sleep(0.03)
                    await call('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': x1 + (x2-x1)*i/15, 'y': y1 + (y2-y1)*i/15, 'button': 'left', 'buttons': 1})
                await call('Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': x2, 'y': y2, 'button': 'left', 'buttons': 0, 'clickCount': 1})
                print(f'[{time.time()-t0:6.1f}] drag {arg}', flush=True)
            elif kind == 'wheel':
                x, y, dy = [float(v) for v in arg.split(',')]
                await call('Input.dispatchMouseEvent', {'type': 'mouseWheel', 'x': x, 'y': y, 'deltaX': 0, 'deltaY': dy})
                print(f'[{time.time()-t0:6.1f}] wheel {arg}', flush=True)
            elif kind == 'key':
                KEYS = {'ArrowRight': ('ArrowRight', 39, ''), 'ArrowLeft': ('ArrowLeft', 37, ''), 'ArrowUp': ('ArrowUp', 38, ''),
                        'ArrowDown': ('ArrowDown', 40, ''), 'Equal': ('=', 187, '='), 'Space': (' ', 32, ' '), 'KeyS': ('s', 83, 's'),
                        'Tab': ('Tab', 9, ''), 'Digit1': ('1', 49, '1'), 'Escape': ('Escape', 27, '')}
                name, hold = arg.split(':')
                k, code, text = KEYS[name]
                base = {'key': k, 'code': name, 'windowsVirtualKeyCode': code, 'nativeVirtualKeyCode': code}
                await call('Input.dispatchKeyEvent', {'type': 'keyDown' if text else 'rawKeyDown', **base, **({'text': text} if text else {})})
                await asyncio.sleep(float(hold))
                await call('Input.dispatchKeyEvent', {'type': 'keyUp', **base})
                print(f'[{time.time()-t0:6.1f}] key {arg}', flush=True)
            elif kind == 'click':
                x, y = [float(v) for v in arg.split(',')]
                await call('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': x, 'y': y})
                await call('Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': x, 'y': y, 'button': 'left', 'clickCount': 1})
                await asyncio.sleep(0.08)
                await call('Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': x, 'y': y, 'button': 'left', 'clickCount': 1})
                print(f'[{time.time()-t0:6.1f}] click {x},{y}', flush=True)
            else:
                r = await call('Page.captureScreenshot', {'format': 'png'})
                open(arg, 'wb').write(base64.b64decode(r['data']))
                print(f'[{time.time()-t0:6.1f}] screenshot {arg}', flush=True)
        rest = a.wait - (time.time() - t0)
        if rest > 0: await asyncio.sleep(rest)

try:
    asyncio.run(main())
finally:
    proc.terminate()
    try: proc.wait(5)
    except Exception: proc.kill()
