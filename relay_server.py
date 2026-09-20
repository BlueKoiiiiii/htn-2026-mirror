#!/usr/bin/env python3
"""Run locally on the laptop; close the window to stop the bidirectional relay.
Python 3.10+, standard library + Tkinter. Two explicitly bound TCP listeners:
mirror/Tailscale :5050, watch/trusted LAN or hotspot :5051. NO application TLS.
Existing server-config.json can be imported without changing the mirror token.
"""
from __future__ import annotations
import asyncio
from collections import OrderedDict
import contextlib
from dataclasses import dataclass, field
import hmac
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import secrets
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox
from tkinter.scrolledtext import ScrolledText
import uuid

BASE = Path(__file__).resolve().parent
MAX_FRAME = 4096
MAX_TEXT = 500
AUTH_TIMEOUT = 5
READ_TIMEOUT = 35
ACK_TIMEOUT = 8
DEVICE_IDS = ('mirror-01', 'watch-01')
ID_RE = re.compile(r'[A-Za-z0-9_-]{1,100}\Z')
TOKEN_RE = re.compile(r'[0-9a-f]{64}\Z')


def encode(message: dict) -> bytes:
    raw = (json.dumps(message, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')
    if len(raw) > MAX_FRAME:
        raise ValueError('Frame too large')
    return raw


async def read_frame(reader, timeout=READ_TIMEOUT):
    raw = await asyncio.wait_for(reader.readline(), timeout)
    if not raw:
        raise ConnectionError('Disconnected')
    if len(raw) > MAX_FRAME or not raw.endswith(b'\n'):
        raise ValueError('Invalid frame length')
    message = json.loads(raw.decode('utf-8'))
    if not isinstance(message, dict) or type(message.get('v')) is not int or message['v'] != 1:
        raise ValueError('Invalid protocol version')
    return message


def valid_text(text):
    if not isinstance(text, str) or not 1 <= len(text.strip()) <= MAX_TEXT or '\0' in text:
        return False
    try:
        text.encode('utf-8')
    except UnicodeError:
        return False
    return True


@dataclass(eq=False)
class Peer:
    device: str
    writer: asyncio.StreamWriter
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    requests: OrderedDict = field(default_factory=OrderedDict)
    last_send: float = -1e9

    async def send(self, message):
        data = encode(message)
        async with self.lock:
            if self.writer.is_closing():
                raise ConnectionError('Disconnected')
            self.writer.write(data)
            await asyncio.wait_for(self.writer.drain(), 2)


class Relay:
    """A single event-loop owns BOTH listeners and all routing/session state."""
    def __init__(self, config, log=print):
        self.config = config
        self.log = log
        self.peers: dict[str, Peer] = {}
        self.writers = set()
        self.handlers = set()
        self.routes = set()
        self.pending = {}  # relay ID -> (recipient connection, Future)
        self.listeners = []
        self.closing = False

    async def start(self):
        try:
            for device in DEVICE_IDS:
                setting = self.config['devices'][device]
                async def accept(reader, writer, expected=device):
                    await self.handle_client(expected, reader, writer)
                listener = await asyncio.start_server(accept, setting['host'], setting['port'], limit=MAX_FRAME)
                self.listeners.append(listener)
        except BaseException:
            await self.close()
            raise
        for device in DEVICE_IDS:
            setting = self.config['devices'][device]
            self.log(f"Listening for {device} on {setting['host']}:{setting['port']}")

    def connected(self, device):
        peer = self.peers.get(device)
        return peer is not None and not peer.writer.is_closing()

    def fail_pending(self, peer):
        for recipient, future in list(self.pending.values()):
            if recipient is peer and not future.done():
                future.set_result(False)

    def route_task(self, coroutine):
        task = asyncio.create_task(coroutine)
        self.routes.add(task)
        def done(t):
            self.routes.discard(t)
            if not t.cancelled() and t.exception() is not None:
                self.log(f'Route error: {type(t.exception()).__name__}')
        task.add_done_callback(done)
        return task

    async def handle_client(self, expected, reader, writer):
        task = asyncio.current_task()
        self.handlers.add(task)
        peer = Peer(expected, writer)
        try:
            if self.closing or len(self.writers) >= 8:
                return
            self.writers.add(writer)
            hello = await read_frame(reader, AUTH_TIMEOUT)
            token = hello.get('token')
            if (hello.get('type') != 'hello' or hello.get('device_id') != expected or
                not isinstance(token, str) or not TOKEN_RE.fullmatch(token) or
                not hmac.compare_digest(token, self.config['devices'][expected]['token'])):
                await peer.send({'v': 1, 'type': 'error', 'message': 'Authentication failed'})
                self.log(f'Rejected authentication on {expected} listener')
                return
            # Each listener accepts only its designated device identity/token.
            await peer.send({'v': 1, 'type': 'welcome', 'device_id': expected})
            old = self.peers.get(expected)
            self.peers[expected] = peer
            if old:
                self.fail_pending(old)
                old.writer.close()
            self.log(f'{expected} connected')
            while self.peers.get(expected) is peer:
                message = await read_frame(reader)
                kind = message.get('type')
                if kind == 'ping':
                    await peer.send({'v': 1, 'type': 'pong'})
                elif kind == 'ack':
                    relay_id = message.get('ack_for')
                    if not isinstance(relay_id, str):
                        raise ValueError('Invalid acknowledgment')
                    pending = self.pending.get(relay_id)
                    if (pending and pending[0] is peer and message.get('status') == 'ui_received'
                            and not pending[1].done()):
                        pending[1].set_result(True)
                elif kind == 'send':
                    request_id = message.get('id')
                    target = 'watch-01' if expected == 'mirror-01' else 'mirror-01'
                    if (not isinstance(request_id, str) or not ID_RE.fullmatch(request_id)
                        or message.get('to') != target or not valid_text(message.get('text'))):
                        raise ValueError('Invalid send request')
                    if request_id in peer.requests:
                        previous = peer.requests[request_id]
                        if previous is not None:
                            await peer.send(previous)
                        continue  # Do not display duplicate requests twice.
                    now = time.monotonic()
                    if now - peer.last_send < 0.4 or len(self.routes) >= 8:
                        result = self.result(request_id, target, 'busy')
                        await peer.send(result)
                        self.remember(peer, request_id, result)
                        continue
                    peer.last_send = now
                    self.remember(peer, request_id, None)
                    self.route_task(self.deliver(peer, request_id, target, message['text'].strip()))
                else:
                    raise ValueError('Unsupported frame')
        except (OSError, ValueError, UnicodeError, asyncio.TimeoutError) as error:
            # Do NOT log packets or tokens.
            self.log(f'{expected} connection ended ({type(error).__name__})')
        finally:
            if self.peers.get(expected) is peer:
                self.peers.pop(expected, None)
                self.fail_pending(peer)
                self.log(f'{expected} disconnected')
            self.writers.discard(writer)
            writer.close()
            with contextlib.suppress(OSError, asyncio.TimeoutError):
                await asyncio.wait_for(writer.wait_closed(), 1)
            self.handlers.discard(task)

    @staticmethod
    def result(request, target, status):
        return {'v': 1, 'type': 'result', 'request_id': request, 'to': target, 'status': status}

    @staticmethod
    def remember(peer, request, result):
        peer.requests[request] = result
        while len(peer.requests) > 128:
            peer.requests.popitem(last=False)

    async def deliver(self, source, request, target, text):
        sender = source.device if source else 'laptop'
        status = 'offline'
        recipient = self.peers.get(target)
        relay_id = str(uuid.uuid4())
        if recipient and not recipient.writer.is_closing():
            future = asyncio.get_running_loop().create_future()
            self.pending[relay_id] = (recipient, future)
            try:
                self.log(f'{sender} -> {target}: sending {relay_id[:8]}')
                # Sender identity is assigned here, never trusted from user input.
                await recipient.send({'v': 1, 'type': 'notify', 'id': relay_id,
                                      'from': sender, 'to': target, 'text': text})
                accepted = await asyncio.wait_for(future, ACK_TIMEOUT)
                status = 'delivered' if accepted else 'unconfirmed'
            except (OSError, ValueError, asyncio.TimeoutError):
                status = 'unconfirmed'
            finally:
                self.pending.pop(relay_id, None)
        self.log(f'{sender} -> {target}: {status} ({relay_id[:8]})')
        result = self.result(request, target, status)
        if source:
            self.remember(source, request, result)
            # Never deliver a result to a different/reconnected sender session.
            if self.peers.get(source.device) is source:
                with contextlib.suppress(OSError, ValueError, asyncio.TimeoutError):
                    await source.send(result)
        return status

    def send_test(self, target, text):
        if target not in DEVICE_IDS or not valid_text(text):
            raise ValueError('Enter 1-500 characters and choose a device')
        return self.route_task(self.deliver(None, str(uuid.uuid4()), target, text.strip()))

    async def close(self):
        self.closing = True
        for listener in self.listeners:
            listener.close()
        for writer in list(self.writers):
            writer.close()
        for peer in list(self.peers.values()):
            self.fail_pending(peer)
        tasks = list(self.routes | self.handlers)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        for listener in self.listeners:
            await listener.wait_closed()
        self.listeners.clear()
        self.peers.clear()


def check_endpoint(endpoint, device):
    host = ipaddress.IPv4Address(endpoint.get('host', ''))
    if host.is_unspecified or host.is_multicast or str(host) == '255.255.255.255':
        raise ValueError('Use an actual laptop interface IPv4, not 0.0.0.0')
    port = endpoint.get('port')
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('Ports must be 1024-65535')
    token = endpoint.get('token')
    if not isinstance(token, str) or not TOKEN_RE.fullmatch(token):
        raise ValueError(f'Invalid {device} token; restore the matching settings')
    return {'host': str(host), 'port': port, 'token': token}


def validate(config):
    if not isinstance(config, dict) or config.get('version') != 1:
        raise ValueError('Invalid relay configuration')
    endpoints = {d: check_endpoint(config['devices'][d], d) for d in DEVICE_IDS}
    if tuple(endpoints['mirror-01'][k] for k in ('host', 'port')) == tuple(endpoints['watch-01'][k] for k in ('host', 'port')):
        raise ValueError('The two listeners must have different addresses or ports')
    if hmac.compare_digest(endpoints['mirror-01']['token'], endpoints['watch-01']['token']):
        raise ValueError('Use separate mirror and watch tokens')
    return {'version': 1, 'devices': endpoints}


def save_private(path, text):
    temporary = path.with_name('.' + path.name + '.' + secrets.token_hex(6) + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8', newline='\n') as f:
            if os.name != 'nt':
                os.chmod(temporary, 0o600)
            f.write(text)
        os.replace(temporary, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()


def load_settings(base):
    relay_file = base / 'relay-config.json'
    if relay_file.exists():
        return validate(json.loads(relay_file.read_text(encoding='utf-8-sig'))), True
    # Migrate old settings in memory only. Do not overwrite the old server config.
    old_file = base / 'server-config.json'
    mirror = {'host': '', 'port': 5050, 'token': secrets.token_hex(32)}
    if old_file.exists():
        old = json.loads(old_file.read_text(encoding='utf-8-sig'))
        if old.get('device_id') != 'mirror-01':
            raise ValueError('Old server-config.json is not for mirror-01')
        mirror = check_endpoint(old, 'mirror-01')
    return {'version': 1, 'devices': {
        'mirror-01': mirror,
        'watch-01': {'host': '', 'port': 5051, 'token': secrets.token_hex(32)}
    }}, False


def save_settings(base, config):
    config = validate(config)
    save_private(base / 'relay-config.json', json.dumps(config, indent=2) + '\n')
    mirror = dict(config['devices']['mirror-01'], device_id='mirror-01')
    save_private(base / 'mirror-connection.json', json.dumps(mirror, indent=2) + '\n')
    watch = config['devices']['watch-01']
    header = f'''#pragma once
// Generated export. Copy to your WATCH project as include/WatchSecrets.h.
// Edit Wi-Fi credentials in that copy, not in this regenerated export.
#define WATCH_WIFI_SSID "YOUR_WIFI_NAME"
#define WATCH_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define WATCH_LAPTOP_HOST "{watch['host']}"
#define WATCH_LAPTOP_PORT {watch['port']}
#define WATCH_DEVICE_ID "watch-01"
#define WATCH_TOKEN "{watch['token']}"
#define WATCH_ALLOW_AUTO_DEEP_SLEEP false
'''
    save_private(base / 'WatchSecrets.generated.h', header)


class Worker(threading.Thread):
    def __init__(self, config, events):
        super().__init__(daemon=True, name='laptop-relay')
        self.config = config
        self.events = events
        self.stop_requested = threading.Event()
        self.commands = queue.Queue(maxsize=1)

    def emit(self, kind, value=None):
        with contextlib.suppress(queue.Full):
            self.events.put_nowait((kind, value))

    def run(self):
        try:
            asyncio.run(self.serve())
        except Exception as e:
            self.emit('error', f'{type(e).__name__}: {e}\nCheck both LAPTOP addresses and stop the old server.')

    async def serve(self):
        relay = Relay(self.config, lambda text: self.emit('log', text))
        try:
            await relay.start()
            self.emit('started')
            last = None
            while not self.stop_requested.is_set():
                connected = tuple(relay.connected(d) for d in DEVICE_IDS)
                if connected != last:
                    self.emit('peers', connected)
                    last = connected
                try:
                    target, text = self.commands.get_nowait()
                except queue.Empty:
                    pass
                else:
                    if len(relay.routes) < 8:
                        relay.send_test(target, text)
                    else:
                        self.emit('log', 'Busy: test message not sent')
                await asyncio.sleep(0.05)
        finally:
            await relay.close()
            self.emit('log', 'Relay stopped. Both listening ports are closed.')


class RelayWindow:
    def __init__(self, root, base=BASE):
        self.root, self.base = root, base
        self.events = queue.Queue(maxsize=2000)
        self.worker = None
        self.closing = False
        self.connected = (False, False)
        self.running = False
        self.failed = False
        self.saved = False
        root.title('Mirror ↔ Watch — Laptop Relay')
        root.geometry('860x610')
        root.minsize(780, 570)
        root.protocol('WM_DELETE_WINDOW', self.close)
        outer = ttk.Frame(root, padding=18)
        outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='Mirror ↔ Laptop ↔ Watch', font=('TkDefaultFont', 18, 'bold')).pack(anchor='w')
        ttk.Label(outer, text='Leave this app running. Device buttons send through it automatically.').pack(anchor='w', pady=(4, 14))
        settings = ttk.Frame(outer)
        settings.pack(fill='x')
        self.hosts, self.ports, self.entries = {}, {}, []
        for i, (device, label) in enumerate(zip(DEVICE_IDS, ('Laptop Tailscale IPv4 (mirror)', 'Laptop LAN/hotspot IPv4 (watch)'))):
            ttk.Label(settings, text=label).grid(row=i, column=0, sticky='w', pady=5)
            self.hosts[device] = tk.StringVar(root)
            self.ports[device] = tk.StringVar(root)
            host = ttk.Entry(settings, textvariable=self.hosts[device], width=21)
            host.grid(row=i, column=1, padx=12)
            ttk.Label(settings, text='Port').grid(row=i, column=2)
            port = ttk.Entry(settings, textvariable=self.ports[device], width=7)
            port.grid(row=i, column=3, padx=8)
            self.entries += [host, port]
        controls = ttk.Frame(outer)
        controls.pack(fill='x', pady=12)
        self.start_button = ttk.Button(controls, text='Start relay', command=self.start)
        self.start_button.pack(side='left')
        self.stop_button = ttk.Button(controls, text='Stop relay', command=self.stop, state='disabled')
        self.stop_button.pack(side='left', padx=8)
        self.status = tk.StringVar(root, value='Stopped')
        ttk.Label(outer, textvariable=self.status).pack(anchor='w', pady=(2, 12))
        compose = ttk.LabelFrame(outer, text='Optional laptop test — not needed for device-to-device buttons', padding=8)
        compose.pack(fill='x')
        self.target = tk.StringVar(root, value='watch-01')
        ttk.Combobox(compose, textvariable=self.target, values=DEVICE_IDS, state='readonly', width=11).pack(side='left')
        self.text = tk.StringVar(root, value='Test notification')
        entry = ttk.Entry(compose, textvariable=self.text)
        entry.pack(side='left', fill='x', expand=True, padx=8)
        entry.bind('<Return>', lambda event: self.send_test())
        self.send_button = ttk.Button(compose, text='Send test', command=self.send_test, state='disabled')
        self.send_button.pack(side='left')
        ttk.Label(outer, text='Watch link: trusted private Wi-Fi/hotspot only; no TLS. Never port-forward these ports.\nDelivered = receiving UI accepted the notification, not proof it was read.', wraplength=780).pack(side='bottom', anchor='w', pady=(10, 0))
        self.log_box = ScrolledText(outer, height=14, state='disabled', wrap='word')
        self.log_box.pack(fill='both', expand=True, pady=(12, 0))
        try:
            self.config, self.saved = load_settings(base)
            for d in DEVICE_IDS:
                self.hosts[d].set(self.config['devices'][d]['host'])
                self.ports[d].set(str(self.config['devices'][d]['port']))
            if self.saved:
                root.after(200, self.start)
            else:
                self.log('First run: enter both LAPTOP addresses. Existing mirror token is reused when server-config.json is present.')
        except (OSError, ValueError, TypeError, KeyError) as e:
            self.failed = True
            self.start_button.configure(state='disabled')
            self.log(f'Cannot load settings: {e}. Existing files were not changed.')
        root.after(100, self.poll)

    def log(self, text):
        self.log_box.configure(state='normal')
        self.log_box.insert('end', f"[{time.strftime('%H:%M:%S')}] {text}\n")
        lines = int(self.log_box.index('end-1c').split('.')[0])
        if lines > 450:
            self.log_box.delete('1.0', f'{lines-350}.0')
        self.log_box.see('end')
        self.log_box.configure(state='disabled')

    def start(self):
        if self.failed or self.closing or (self.worker and self.worker.is_alive()):
            return
        try:
            config = {'version': 1, 'devices': {d: {
                'host': self.hosts[d].get().strip(), 'port': int(self.ports[d].get()),
                'token': self.config['devices'][d]['token']
            } for d in DEVICE_IDS}}
            config = validate(config)
            changed = config != self.config
            if not self.saved and not messagebox.askokcancel('Save relay settings?',
                'Save relay-config.json and export matching device settings?\n\nAlready configured the mirror? The old server-config.json must be beside this script to preserve its token.', parent=self.root):
                return
            save_settings(self.base, config)
            self.config, self.saved = config, True
            self.log('Settings saved. WatchSecrets.generated.h is the WATCH export; keep it private.')
            if changed:
                self.log('Addresses changed: update the affected device settings before testing.')
        except (OSError, ValueError, KeyError, TypeError) as e:
            messagebox.showerror('Settings', str(e), parent=self.root)
            return
        self.running = False
        self.connected = (False, False)
        self.start_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        for entry in self.entries:
            entry.configure(state='disabled')
        self.worker = Worker(config, self.events)
        self.worker.start()
        self.status.set('Starting both listeners...')

    def stop(self):
        self.running = False
        self.send_button.configure(state='disabled')
        self.stop_button.configure(state='disabled')
        if self.worker:
            self.worker.stop_requested.set()
        self.status.set('Stopping...')

    def close(self):
        self.closing = True
        self.stop()

    def send_test(self):
        if not self.running or not self.worker or self.worker.stop_requested.is_set():
            return
        if not valid_text(self.text.get()):
            messagebox.showwarning('Message', 'Enter 1-500 characters', parent=self.root)
            return
        try:
            self.worker.commands.put_nowait((self.target.get(), self.text.get().strip()))
        except queue.Full:
            self.log('Busy: wait before sending another test')

    def poll(self):
        for _ in range(200):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'log':
                self.log(value)
            elif kind == 'started':
                self.running = not self.closing and not self.worker.stop_requested.is_set()
            elif kind == 'peers':
                self.connected = value
            elif kind == 'error':
                self.log(value)
                if not self.closing:
                    messagebox.showerror('Server error', value, parent=self.root)
        if self.worker and not self.worker.is_alive():
            self.worker.join(timeout=0)
            self.worker = None
            self.running = False
            self.connected = (False, False)
            for entry in self.entries:
                entry.configure(state='normal')
            self.start_button.configure(state='disabled' if self.closing or self.failed else 'normal')
            self.stop_button.configure(state='disabled')
        if self.running:
            names = ['connected' if c else 'offline' for c in self.connected]
            self.status.set(f'Relay running     |     Mirror: {names[0]}     |     Watch: {names[1]}')
        elif self.worker is None:
            self.status.set('Relay stopped')
        self.send_button.configure(state='normal' if self.running else 'disabled')
        if self.closing and self.worker is None:
            self.root.destroy()
            return
        self.root.after(100, self.poll)


def main():
    root = tk.Tk()
    app = RelayWindow(root)
    try:
        root.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        if app.worker:
            app.worker.stop_requested.set()
            app.worker.join(timeout=5)
        with contextlib.suppress(tk.TclError):
            root.destroy()


if __name__ == '__main__':
    main()
