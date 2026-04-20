#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["msgpack", "click"]
# ///
"""
Async RPC client for yetty terminal.

Protocol: msgpack-rpc over TCP
- Notification: [2, channel, method, params]
- Request:      [0, msgid, channel, method, params]
- Response:     [1, msgid, error, result]

Channels:
- 0: EventLoop (keyboard, mouse, window events)
- 1: CardStream (card buffer/texture streaming)
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass
from enum import IntEnum
from typing import Any

import msgpack


class Channel(IntEnum):
    EventLoop = 0
    CardStream = 1


class MessageType(IntEnum):
    Request = 0
    Response = 1
    Notification = 2


@dataclass
class RpcResponse:
    msgid: int
    error: Any
    result: Any


class RpcClient:
    """Async RPC client for yetty terminal."""

    def __init__(self, host: str = "127.0.0.1", port: int = 9999):
        self.host = host
        self.port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._msgid = 0
        self._pending: dict[int, asyncio.Future[RpcResponse]] = {}
        self._recv_task: asyncio.Task | None = None
        self._unpacker = msgpack.Unpacker(raw=False)
        self._connected = False

    async def connect(self) -> None:
        """Connect to the yetty RPC server."""
        self._reader, self._writer = await asyncio.open_connection(self.host, self.port)
        self._connected = True
        self._recv_task = asyncio.create_task(self._recv_loop())

    async def disconnect(self) -> None:
        """Disconnect from the server."""
        if self._recv_task:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except asyncio.CancelledError:
                pass
            self._recv_task = None
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()
            self._writer = None
            self._reader = None
        self._connected = False

    async def __aenter__(self) -> RpcClient:
        await self.connect()
        return self

    async def __aexit__(self, *args) -> None:
        await self.disconnect()

    def _next_msgid(self) -> int:
        self._msgid += 1
        return self._msgid

    async def _recv_loop(self) -> None:
        """Background task to receive and dispatch responses."""
        assert self._reader is not None
        try:
            while True:
                data = await self._reader.read(65536)
                if not data:
                    break
                self._unpacker.feed(data)
                for msg in self._unpacker:
                    self._handle_message(msg)
        except asyncio.CancelledError:
            pass
        except Exception as e:
            for fut in self._pending.values():
                if not fut.done():
                    fut.set_exception(e)
            self._pending.clear()

    def _handle_message(self, msg: list) -> None:
        """Handle an incoming message."""
        if not isinstance(msg, list) or len(msg) < 3:
            return
        msg_type = msg[0]
        if msg_type == MessageType.Response:
            if len(msg) < 4:
                return
            msgid = msg[1]
            error = msg[2]
            result = msg[3]
            fut = self._pending.pop(msgid, None)
            if fut and not fut.done():
                fut.set_result(RpcResponse(msgid, error, result))

    async def notify(self, channel: Channel, method: str, params: dict) -> None:
        """Send a notification (no response expected)."""
        if not self._connected:
            raise ConnectionError("Not connected")
        msg = [MessageType.Notification, int(channel), method, params]
        data = msgpack.packb(msg, use_bin_type=True)
        assert self._writer is not None
        self._writer.write(data)
        await self._writer.drain()

    async def request(self, channel: Channel, method: str, params: dict) -> Any:
        """Send a request and wait for response."""
        if not self._connected:
            raise ConnectionError("Not connected")
        msgid = self._next_msgid()
        msg = [MessageType.Request, msgid, int(channel), method, params]
        data = msgpack.packb(msg, use_bin_type=True)

        fut: asyncio.Future[RpcResponse] = asyncio.get_event_loop().create_future()
        self._pending[msgid] = fut

        assert self._writer is not None
        self._writer.write(data)
        await self._writer.drain()

        response = await fut
        if response.error is not None:
            raise RuntimeError(f"RPC error: {response.error}")
        return response.result

    # --- EventLoop Channel Methods ---

    async def key_down(self, key: int, mods: int = 0, scancode: int = 0) -> None:
        """Send key down event."""
        await self.notify(Channel.EventLoop, "key_down", {
            "key": key, "mods": mods, "scancode": scancode
        })

    async def key_up(self, key: int, mods: int = 0, scancode: int = 0) -> None:
        """Send key up event."""
        await self.notify(Channel.EventLoop, "key_up", {
            "key": key, "mods": mods, "scancode": scancode
        })

    async def char_input(self, codepoint: int, mods: int = 0) -> None:
        """Send character input event."""
        await self.notify(Channel.EventLoop, "char", {
            "codepoint": codepoint, "mods": mods
        })

    async def mouse_down(self, x: float, y: float, button: int) -> None:
        """Send mouse button down event."""
        await self.notify(Channel.EventLoop, "mouse_down", {
            "x": x, "y": y, "button": button
        })

    async def mouse_up(self, x: float, y: float, button: int) -> None:
        """Send mouse button up event."""
        await self.notify(Channel.EventLoop, "mouse_up", {
            "x": x, "y": y, "button": button
        })

    async def mouse_move(self, x: float, y: float) -> None:
        """Send mouse move event."""
        await self.notify(Channel.EventLoop, "mouse_move", {"x": x, "y": y})

    async def scroll(self, x: float, y: float, dx: float, dy: float, mods: int = 0) -> None:
        """Send scroll event."""
        await self.notify(Channel.EventLoop, "scroll", {
            "x": x, "y": y, "dx": dx, "dy": dy, "mods": mods
        })

    async def resize(self, width: float, height: float) -> None:
        """Resize the window."""
        await self.notify(Channel.EventLoop, "resize", {
            "width": width, "height": height
        })

    async def ui_tree(self) -> str:
        """Get UI tree (request, returns string)."""
        return await self.request(Channel.EventLoop, "ui_tree", {})

    # --- High-Level Commands ---

    async def type_text(self, text: str, delay: float = 0.01) -> None:
        """Type text by sending character events."""
        for char in text:
            await self.char_input(ord(char))
            if delay > 0:
                await asyncio.sleep(delay)

    async def run_command(self, command: str, delay: float = 0.01) -> None:
        """Type a command and press Enter."""
        await self.type_text(command, delay)
        await self.char_input(ord('\n'))

    async def press_key(self, key: int, mods: int = 0) -> None:
        """Press and release a key."""
        await self.key_down(key, mods)
        await self.key_up(key, mods)


# --- GLFW Key Codes ---

class Keys:
    """GLFW key codes."""
    SPACE = 32
    ENTER = 257
    TAB = 258
    BACKSPACE = 259
    ESCAPE = 256
    DELETE = 261
    RIGHT = 262
    LEFT = 263
    DOWN = 264
    UP = 265
    PAGE_UP = 266
    PAGE_DOWN = 267
    HOME = 268
    END = 269
    F1 = 290
    F12 = 301


class Mods:
    """GLFW modifier flags."""
    SHIFT = 0x0001
    CONTROL = 0x0002
    ALT = 0x0004
    SUPER = 0x0008


# --- CLI ---

def run_async(coro):
    """Run an async coroutine."""
    return asyncio.run(coro)


try:
    import click
except ImportError:
    click = None  # type: ignore

if click:
    @click.group()
    @click.option("--host", "-h", default="127.0.0.1", help="Server host")
    @click.option("--port", "-p", default=9999, type=int, help="Server port")
    @click.pass_context
    def cli(ctx, host, port):
        """Yetty RPC client."""
        ctx.ensure_object(dict)
        ctx.obj["host"] = host
        ctx.obj["port"] = port

    @cli.command()
    @click.argument("key", type=int)
    @click.option("--mods", "-m", default=0, type=int)
    @click.pass_context
    def key_down(ctx, key, mods):
        """Send key down event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.key_down(key, mods)
        run_async(_run())

    @cli.command()
    @click.argument("key", type=int)
    @click.option("--mods", "-m", default=0, type=int)
    @click.pass_context
    def key_up(ctx, key, mods):
        """Send key up event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.key_up(key, mods)
        run_async(_run())

    @cli.command()
    @click.argument("key", type=int)
    @click.option("--mods", "-m", default=0, type=int)
    @click.pass_context
    def press(ctx, key, mods):
        """Press and release a key."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.press_key(key, mods)
        run_async(_run())

    @cli.command("char")
    @click.argument("codepoint", type=int)
    @click.option("--mods", "-m", default=0, type=int)
    @click.pass_context
    def char_cmd(ctx, codepoint, mods):
        """Send character input."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.char_input(codepoint, mods)
        run_async(_run())

    @cli.command()
    @click.argument("x", type=float)
    @click.argument("y", type=float)
    @click.argument("button", type=int, default=0)
    @click.pass_context
    def mouse_down(ctx, x, y, button):
        """Send mouse down event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.mouse_down(x, y, button)
        run_async(_run())

    @cli.command()
    @click.argument("x", type=float)
    @click.argument("y", type=float)
    @click.argument("button", type=int, default=0)
    @click.pass_context
    def mouse_up(ctx, x, y, button):
        """Send mouse up event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.mouse_up(x, y, button)
        run_async(_run())

    @cli.command()
    @click.argument("x", type=float)
    @click.argument("y", type=float)
    @click.pass_context
    def mouse_move(ctx, x, y):
        """Send mouse move event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.mouse_move(x, y)
        run_async(_run())

    @cli.command()
    @click.argument("x", type=float)
    @click.argument("y", type=float)
    @click.argument("dx", type=float)
    @click.argument("dy", type=float)
    @click.option("--mods", "-m", default=0, type=int)
    @click.pass_context
    def scroll(ctx, x, y, dx, dy, mods):
        """Send scroll event."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.scroll(x, y, dx, dy, mods)
        run_async(_run())

    @cli.command()
    @click.argument("width", type=float)
    @click.argument("height", type=float)
    @click.pass_context
    def resize(ctx, width, height):
        """Resize window."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.resize(width, height)
        run_async(_run())

    @cli.command()
    @click.pass_context
    def ui_tree(ctx):
        """Get UI tree."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                tree = await client.ui_tree()
                click.echo(tree)
        run_async(_run())

    @cli.command("type")
    @click.argument("text")
    @click.option("--delay", "-d", default=0.01, type=float)
    @click.pass_context
    def type_cmd(ctx, text, delay):
        """Type text."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.type_text(text, delay)
        run_async(_run())

    @cli.command()
    @click.argument("command")
    @click.option("--delay", "-d", default=0.01, type=float)
    @click.pass_context
    def run(ctx, command, delay):
        """Run a command (type + Enter)."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.run_command(command, delay)
        run_async(_run())

    @cli.command()
    @click.pass_context
    def enter(ctx):
        """Press Enter."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.char_input(ord('\n'))
        run_async(_run())

    @cli.command()
    @click.pass_context
    def tab(ctx):
        """Press Tab."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.press_key(Keys.TAB)
        run_async(_run())

    @cli.command()
    @click.pass_context
    def escape(ctx):
        """Press Escape."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.press_key(Keys.ESCAPE)
        run_async(_run())

    @cli.command("ctrl-c")
    @click.pass_context
    def ctrl_c(ctx):
        """Send Ctrl+C."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.char_input(3, Mods.CONTROL)
        run_async(_run())

    @cli.command("ctrl-d")
    @click.pass_context
    def ctrl_d(ctx):
        """Send Ctrl+D."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                await client.char_input(4, Mods.CONTROL)
        run_async(_run())

    @cli.command()
    @click.pass_context
    def interactive(ctx):
        """Interactive mode."""
        async def _run():
            async with RpcClient(ctx.obj["host"], ctx.obj["port"]) as client:
                click.echo("Interactive mode. Ctrl+D to exit.")
                while True:
                    try:
                        line = input("> ")
                        await client.run_command(line, delay=0.005)
                    except EOFError:
                        click.echo("\nExiting.")
                        break
                    except KeyboardInterrupt:
                        await client.char_input(3, Mods.CONTROL)
                        click.echo("^C")
        run_async(_run())

    @cli.command()
    def keys():
        """Show key codes."""
        click.echo("GLFW Key Codes:")
        click.echo(f"  ENTER={Keys.ENTER} TAB={Keys.TAB} ESCAPE={Keys.ESCAPE}")
        click.echo(f"  BACKSPACE={Keys.BACKSPACE} DELETE={Keys.DELETE}")
        click.echo(f"  LEFT={Keys.LEFT} RIGHT={Keys.RIGHT} UP={Keys.UP} DOWN={Keys.DOWN}")
        click.echo(f"  HOME={Keys.HOME} END={Keys.END}")
        click.echo(f"  PAGE_UP={Keys.PAGE_UP} PAGE_DOWN={Keys.PAGE_DOWN}")
        click.echo("Mods: SHIFT=1 CONTROL=2 ALT=4 SUPER=8")

    def main():
        cli()

else:
    def main():
        print("Click not installed. Use: uv run yrpc.py")


if __name__ == "__main__":
    main()
