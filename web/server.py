#!/usr/bin/env python3
"""Administracion web del mapper (solo biblioteca estandar).

Guarda el estado en /var/lib/mapper/state.json y se lo pasa al renderer por el
socket datagrama /run/mapper/ctl.sock. La reproduccion en vivo (pausa, posicion)
va directo al socket IPC de mpv. Los videos subidos se optimizan a 720p30 H.264
con el codificador por hardware de la Pi (h264_v4l2m2m): es lo que la Pi 3
reproduce entero sin perder cuadros.
"""
import json
import os
import queue
import shutil
import socket
import struct
import subprocess
import threading
import time
import urllib.parse
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = '/var/lib/mapper/state.json'
MEDIA = '/var/lib/mapper/media'
ORIG = '/var/lib/mapper/originales'
THUMBS = '/var/lib/mapper/miniaturas'
RUN = '/run/mapper'
CTL = RUN + '/ctl.sock'
MPV = RUN + '/mpv.sock'
INFO = RUN + '/info.json'
SHOT = RUN + '/shot.ppm'
WEB = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get('MAPPER_PORT', '80'))
BOTON = 3   # GPIO3 = pin 5; el pulsador va entre el pin 5 y el pin 6 (GND)

VIDEO_EXT = {'.mp4', '.mov', '.mkv', '.avi', '.webm', '.m4v', '.mpg', '.mpeg', '.ts', '.wmv', '.flv'}
IMAGE_EXT = {'.jpg', '.jpeg', '.png', '.webp', '.bmp', '.gif'}
STATIC = {'/': 'index.html', '/app.js': 'app.js', '/app.css': 'app.css'}
CTYPE = {'.html': 'text/html; charset=utf-8', '.js': 'application/javascript; charset=utf-8',
         '.css': 'text/css; charset=utf-8', '.png': 'image/png', '.jpg': 'image/jpeg', '.json': 'application/json'}

DEFAULT_STATE = {
    'output': {'mode': '1280x720'},
    'show': {'selected': 0, 'point': -1, 'edit': 'corners', 'overlay': True, 'pattern': True, 'blackout': False},
    'surfaces': [{'name': 'Superficie 1'}],
    'media': {'playlist': [], 'loop': True, 'volume': 80, 'mute': False, 'image_secs': 10, 'audio': 'auto'},
}

lock = threading.Lock()
state = None
save_at = 0.0


def load_state():
    global state
    try:
        with open(STATE) as f:
            state = json.load(f)
        if not isinstance(state, dict):
            raise ValueError
    except Exception:
        state = json.loads(json.dumps(DEFAULT_STATE))


def write_state():
    tmp = STATE + '.tmp'
    with lock:
        data = json.dumps(state, separators=(',', ':'))
    with open(tmp, 'w') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, STATE)


def saver():
    """Escribe a la SD un segundo despues del ultimo cambio (no en cada arrastre)."""
    global save_at
    while True:
        time.sleep(0.25)
        if save_at and time.time() >= save_at:
            save_at = 0.0
            try:
                write_state()
            except OSError as e:
                print('no pude guardar el estado:', e, flush=True)


def send_ctl(obj):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(json.dumps(obj, separators=(',', ':')).encode(), CTL)
        return True
    except OSError:
        return False
    finally:
        s.close()


def push_state():
    global save_at
    with lock:
        snap = json.loads(json.dumps(state))
    send_ctl(snap)
    save_at = time.time() + 1.0


def set_blackout(v=None):
    """Apaga o prende la imagen (v=None: alterna). El renderer pausa el video al apagar."""
    with lock:
        show = state.setdefault('show', {})
        show['blackout'] = (not show.get('blackout')) if v is None else bool(v)
        r = show['blackout']
    push_state()
    print('imagen', 'apagada' if r else 'encendida', flush=True)
    return r


def boton():
    """Pulsador físico entre GPIO3 y GND: cada pulsación apaga o prende la imagen."""
    try:
        from datetime import timedelta

        import gpiod
        from gpiod.line import Bias, Direction, Edge, Value
    except ImportError:
        print('boton: falta python3-libgpiod', flush=True)
        return
    cfg = {BOTON: gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP, edge_detection=Edge.FALLING,
                                     debounce_period=timedelta(milliseconds=30))}
    while True:
        try:
            with gpiod.request_lines('/dev/gpiochip0', consumer='mapper-boton', config=cfg) as req:
                last = 0.0
                while True:
                    req.read_edge_events()
                    now = time.monotonic()
                    if now - last > 0.5 and req.get_value(BOTON) == Value.INACTIVE:
                        last = now
                        set_blackout()
        except OSError as e:
            print('boton:', e, flush=True)
            time.sleep(10)


def mpv_cmd(*cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(MPV)
        s.sendall(json.dumps({'command': list(cmd), 'request_id': 7}).encode() + b'\n')
        buf = b''
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return None
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                msg = json.loads(line)
                if msg.get('request_id') == 7:
                    return msg.get('data') if msg.get('error') == 'success' else None
    except (OSError, ValueError):
        return None
    finally:
        s.close()


def ppm_to_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    row = w * 3
    raw = b''.join(b'\x00' + px[y * row:(y + 1) * row] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))


def safe_name(name):
    name = os.path.basename(urllib.parse.unquote(name or '')).strip()
    name = ''.join(c if c.isalnum() or c in '._- ()' else '_' for c in name)
    return name.lstrip('.') or None


def probe(path):
    try:
        out = subprocess.run(['ffprobe', '-v', 'error', '-print_format', 'json', '-show_streams',
                              '-show_format', path], capture_output=True, timeout=30).stdout
        return json.loads(out)
    except Exception:
        return {}


def media_info(name):
    p = os.path.join(MEDIA, name)
    st = os.stat(p)
    ext = os.path.splitext(name)[1].lower()
    info = {'name': name, 'size': st.st_size, 'mtime': int(st.st_mtime),
            'kind': 'image' if ext in IMAGE_EXT else 'video',
            'thumb': os.path.exists(os.path.join(THUMBS, name + '.jpg'))}
    cache = os.path.join(THUMBS, name + '.json')
    try:
        with open(cache) as f:
            info.update(json.load(f))
    except Exception:
        pass
    return info


def describe(path):
    """Resumen corto del archivo (resolucion, fps, codec, duracion) para la lista."""
    pr = probe(path)
    v = next((s for s in pr.get('streams', []) if s.get('codec_type') == 'video'), None)
    a = next((s for s in pr.get('streams', []) if s.get('codec_type') == 'audio'), None)
    d = {'audio': bool(a)}
    if v:
        try:
            n, dd = v.get('avg_frame_rate', '0/1').split('/')
            fps = float(n) / float(dd) if float(dd) else 0
        except ValueError:
            fps = 0
        d.update({'w': v.get('width'), 'h': v.get('height'), 'codec': v.get('codec_name'),
                  'fps': round(fps, 2), 'pix': v.get('pix_fmt')})
    try:
        d['dur'] = round(float(pr.get('format', {}).get('duration', 0)), 2)
    except ValueError:
        pass
    return d, v


def needs_transcode(d, v):
    if not v:
        return False
    return (d.get('codec') != 'h264' or (d.get('h') or 0) > 720 or (d.get('w') or 0) > 1280 or
            d.get('fps', 0) > 31 or d.get('pix') not in ('yuv420p', 'yuvj420p'))


jobs = {}
job_q = queue.Queue()


def worker():
    while True:
        name = job_q.get()
        try:
            process(name)
        except Exception as e:
            jobs[name] = {'state': 'error', 'msg': str(e)}
            print('error procesando', name, e, flush=True)


def process(name):
    """Optimiza un archivo recien subido: video a 720p30 H.264 por hardware, imagen a 1280x720 max."""
    src = os.path.join(ORIG, name)
    base, ext = os.path.splitext(name)
    ext = ext.lower()
    if ext in IMAGE_EXT:
        out = os.path.join(MEDIA, base + ('.png' if ext == '.png' else '.jpg'))
        jobs[name] = {'state': 'run', 'pct': 0}
        subprocess.run(['nice', '-n', '10', 'ffmpeg', '-v', 'error', '-y', '-i', src, '-frames:v', '1', '-vf',
                        "scale='min(1280,iw)':'min(720,ih)':force_original_aspect_ratio=decrease", out], check=True)
    else:
        d, v = describe(src)
        out = os.path.join(MEDIA, base + '.mp4')
        if not needs_transcode(d, v):
            # ya es reproducible: solo se remultiplexa a mp4 (sin recodificar)
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-map', '0:v:0', '-map', '0:a:0?', '-c', 'copy',
                            '-movflags', '+faststart', '-f', 'mp4', out + '.tmp'], check=True)
            os.replace(out + '.tmp', out)
        else:
            dur = d.get('dur') or 0
            jobs[name] = {'state': 'run', 'pct': 0}
            vf = ("scale=w='min(1280,iw)':h='min(720,ih)':force_original_aspect_ratio=decrease:"
                  "force_divisible_by=2," + ("fps=30," if d.get('fps', 0) > 31 else '') + 'format=yuv420p')
            dec = ['-c:v', 'h264_v4l2m2m'] if d.get('codec') == 'h264' and (d.get('h') or 0) <= 1088 else ['-threads', '3']
            cmd = ['nice', '-n', '10', 'ffmpeg', '-v', 'error', '-y', '-progress', 'pipe:1', '-nostats',
                   *dec, '-i', src, '-map', '0:v:0', '-map', '0:a:0?', '-vf', vf,
                   '-c:v', 'h264_v4l2m2m', '-b:v', '5M', '-g', '30',
                   '-c:a', 'aac', '-b:a', '160k', '-ac', '2', '-ar', '48000', '-movflags', '+faststart',
                   '-f', 'mp4', out + '.tmp']
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for line in p.stdout:
                if line.startswith('out_time_us=') and dur:
                    try:
                        jobs[name]['pct'] = min(99, int(int(line.split('=')[1]) / 1e6 / dur * 100))
                    except ValueError:
                        pass
            err = p.stderr.read()
            if p.wait() != 0:
                raise RuntimeError(err.strip()[-300:] or 'ffmpeg fallo')
            os.replace(out + '.tmp', out)
    final = os.path.basename(out)
    d, _ = describe(out)
    with open(os.path.join(THUMBS, final + '.json'), 'w') as f:
        json.dump(d, f)
    t = min(1.0, (d.get('dur') or 0) / 3)
    subprocess.run(['nice', '-n', '10', 'ffmpeg', '-v', 'error', '-y', '-ss', str(t), '-i', out, '-frames:v', '1',
                    '-vf', 'scale=240:-2', os.path.join(THUMBS, final + '.jpg')], check=False)
    jobs[name] = {'state': 'ok', 'out': final}


def sysinfo():
    def rd(p, d=''):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            return d
    try:
        with open(INFO) as f:
            info = json.load(f)
        info['renderer'] = os.path.exists('/proc/%d' % info.get('pid', 0))
    except Exception:
        info = {'renderer': False}
    mem = {}
    for line in rd('/proc/meminfo').splitlines():
        k, v = line.split(':', 1)
        mem[k] = int(v.split()[0])
    du = shutil.disk_usage(MEDIA)
    try:
        thr = subprocess.run(['vcgencmd', 'get_throttled'], capture_output=True, text=True, timeout=2).stdout.strip()
    except Exception:
        thr = ''
    info.update({
        'temp': round(int(rd('/sys/class/thermal/thermal_zone0/temp', '0')) / 1000, 1),
        'load': rd('/proc/loadavg').split()[:3],
        'mem_used_mb': (mem.get('MemTotal', 0) - mem.get('MemAvailable', 0)) // 1024,
        'mem_total_mb': mem.get('MemTotal', 0) // 1024,
        'disk_free_gb': round(du.free / 1e9, 1),
        'uptime_s': int(float(rd('/proc/uptime', '0').split()[0])),
        'throttled': thr.split('=')[-1] if thr else '',
        'hostname': socket.gethostname(),
    })
    return info


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'mapper'

    def log_message(self, fmt, *args):
        pass

    def send(self, code, body=b'', ctype='application/json', extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def body(self, limit=4 << 20):
        n = int(self.headers.get('Content-Length') or 0)
        if n > limit:
            raise ValueError('cuerpo demasiado grande')
        return self.rfile.read(n) if n else b''

    def jbody(self):
        return json.loads(self.body() or b'{}')

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        path = u.path
        if path in STATIC:
            f = os.path.join(WEB, STATIC[path])
            with open(f, 'rb') as fh:
                return self.send(200, fh.read(), CTYPE[os.path.splitext(f)[1]])
        if path == '/api/state':
            with lock:
                return self.send(200, state)
        if path == '/api/info':
            return self.send(200, sysinfo())
        if path == '/api/media':
            files = []
            for n in sorted(os.listdir(MEDIA), key=str.lower):
                if not n.startswith('.') and not n.endswith('.tmp'):
                    files.append(media_info(n))
            return self.send(200, {'files': files, 'jobs': jobs})
        if path.startswith('/api/thumb/'):
            n = safe_name(path[len('/api/thumb/'):])
            p = os.path.join(THUMBS, (n or '') + '.jpg')
            if n and os.path.exists(p):
                with open(p, 'rb') as fh:
                    return self.send(200, fh.read(), 'image/jpeg', {'Cache-Control': 'max-age=3600'})
            return self.send(404, {'error': 'sin miniatura'})
        if path == '/api/blackout':
            with lock:
                bo = bool(state.get('show', {}).get('blackout'))
            return self.send(200, {'blackout': bo})
        if path == '/api/player':
            props = {}
            for k in ('filename', 'time-pos', 'duration', 'pause', 'playlist-pos', 'playlist-count'):
                props[k] = mpv_cmd('get_property', k)
            return self.send(200, props)
        if path == '/api/shot.png':
            before = os.stat(SHOT).st_mtime if os.path.exists(SHOT) else 0
            if not send_ctl({'cmd': 'shot'}):
                return self.send(503, {'error': 'el renderer no responde'})
            t0 = time.time()
            while time.time() - t0 < 2.0:
                if os.path.exists(SHOT) and os.stat(SHOT).st_mtime != before:
                    break
                time.sleep(0.05)
            else:
                return self.send(504, {'error': 'sin captura'})
            return self.send(200, ppm_to_png(SHOT), 'image/png')
        return self.send(404, {'error': 'no existe'})

    def do_HEAD(self):
        self.do_GET()

    def do_PUT(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/api/state':
            global state
            new = self.jbody()
            if not isinstance(new, dict) or not isinstance(new.get('surfaces'), list):
                return self.send(400, {'error': 'estado invalido'})
            if not isinstance(new.get('show'), dict):
                new['show'] = {}
            with lock:
                # el apagado tiene su propia ruta: una pestaña vieja no lo pisa
                new['show']['blackout'] = bool(state.get('show', {}).get('blackout'))
                state = new
            push_state()
            return self.send(200, {'ok': True})
        if u.path == '/api/media':
            q = urllib.parse.parse_qs(u.query)
            name = safe_name((q.get('name') or [''])[0])
            ext = os.path.splitext(name or '')[1].lower()
            if not name or ext not in VIDEO_EXT | IMAGE_EXT:
                return self.send(400, {'error': 'formato no soportado'})
            n = int(self.headers.get('Content-Length') or 0)
            if shutil.disk_usage(MEDIA).free < n * 2 + (500 << 20):
                return self.send(507, {'error': 'no hay espacio'})
            dst = os.path.join(ORIG, name)
            with open(dst + '.part', 'wb') as f:
                left = n
                while left > 0:
                    chunk = self.rfile.read(min(1 << 20, left))
                    if not chunk:
                        break
                    f.write(chunk)
                    left -= len(chunk)
            if left:
                os.unlink(dst + '.part')
                return self.send(400, {'error': 'subida incompleta'})
            os.replace(dst + '.part', dst)
            jobs[name] = {'state': 'queued'}
            job_q.put(name)
            return self.send(200, {'ok': True, 'name': name})
        return self.send(404, {'error': 'no existe'})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/api/blackout':
            return self.send(200, {'blackout': set_blackout(self.jbody().get('value'))})
        if u.path == '/api/player':
            b = self.jbody()
            a = b.get('action')
            if a == 'pause':
                mpv_cmd('cycle', 'pause')
            elif a == 'next':
                mpv_cmd('playlist-next', 'force')
            elif a == 'prev':
                mpv_cmd('playlist-prev', 'force')
            elif a == 'restart':
                mpv_cmd('seek', 0, 'absolute')
            elif a == 'seek':
                mpv_cmd('seek', float(b.get('value', 0)), 'absolute')
            elif a == 'goto':
                mpv_cmd('set_property', 'playlist-pos', int(b.get('value', 0)))
            else:
                return self.send(400, {'error': 'accion desconocida'})
            return self.send(200, {'ok': True})
        if u.path == '/api/system':
            a = self.jbody().get('action')
            if a == 'restart':
                send_ctl({'cmd': 'restart'})
                return self.send(200, {'ok': True})
            if a in ('reboot', 'poweroff'):
                write_state()
                self.send(200, {'ok': True})
                subprocess.Popen(['sudo', '-n', 'systemctl', a])
                return None
            return self.send(400, {'error': 'accion desconocida'})
        return self.send(404, {'error': 'no existe'})

    def do_DELETE(self):
        u = urllib.parse.urlparse(self.path)
        if u.path.startswith('/api/media/'):
            n = safe_name(u.path[len('/api/media/'):])
            p = os.path.join(MEDIA, n or '')
            if not n or not os.path.exists(p):
                return self.send(404, {'error': 'no existe'})
            os.unlink(p)
            for extra in (os.path.join(THUMBS, n + '.jpg'), os.path.join(THUMBS, n + '.json')):
                if os.path.exists(extra):
                    os.unlink(extra)
            with lock:
                pl = state.get('media', {}).get('playlist', [])
                if n in pl:
                    state['media']['playlist'] = [x for x in pl if x != n]
            push_state()
            return self.send(200, {'ok': True})
        return self.send(404, {'error': 'no existe'})


def main():
    for d in (MEDIA, ORIG, THUMBS):
        os.makedirs(d, exist_ok=True)
    load_state()
    threading.Thread(target=saver, daemon=True).start()
    threading.Thread(target=worker, daemon=True).start()
    threading.Thread(target=boton, daemon=True).start()
    # el renderer puede haber arrancado antes con un estado viejo
    push_state()
    srv = ThreadingHTTPServer(('', PORT), H)
    srv.daemon_threads = True
    print('mapper-web en el puerto', PORT, flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
