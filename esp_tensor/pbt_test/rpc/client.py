from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from enum import IntEnum
from typing import Callable


REQUEST_MAGIC = 0x31514352
RESPONSE_MAGIC = 0x31505352

DEFAULT_TIMEOUT = 20.0


class RpcStatus(IntEnum):
    OK = 0
    ERROR = 1
    BAD_FRAME = 2
    UNKNOWN_METHOD = 3


@dataclass
class RpcClient:
    serial: object
    timeout: float = DEFAULT_TIMEOUT
    reset_recovery_delay: float = 0.5

    def __enter__(self) -> RpcClient:
        self._release_boot_straps()
        self._killed = False
        try:
            self._killed = self.serial.stop_redirect_thread()
            self._release_boot_straps()
            self.wait_ready()
        except Exception:
            if self._killed:
                self.serial.start_redirect_thread()
            raise
        return self

    def __exit__(self, *args) -> None:
        self._release_boot_straps()
        if self._killed:
            self.serial.start_redirect_thread()

    def _release_boot_straps(self) -> None:
        self.serial.proc.dtr = False
        self.serial.proc.rts = False

    @property
    def port(self) -> str:
        return getattr(self.serial, "port", getattr(self.serial.proc, "port", "<unknown>"))

    def _with_serial_timeout(self, timeout: float, fn: Callable[[], bytes | None]) -> bytes | None:
        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = timeout
            return fn()
        finally:
            self.serial.proc.timeout = original_timeout

    def _drain_input(self, *, max_duration: float = 0.5) -> int:
        def drain() -> bytes:
            drained = bytearray()
            deadline = time.monotonic() + max_duration
            while time.monotonic() < deadline:
                chunk = self.serial.proc.read(256)
                if not chunk:
                    break
                drained.extend(chunk)
            return bytes(drained)

        return len(self._with_serial_timeout(0.05, drain) or b"")

    def _reset_buffers(self) -> None:
        for name in ("reset_input_buffer", "reset_output_buffer"):
            reset = getattr(self.serial.proc, name, None)
            if callable(reset):
                reset()

    def _hard_reset(self) -> bool:
        reset = getattr(self.serial, "hard_reset", None)
        if not callable(reset):
            return False
        reset()
        self._release_boot_straps()
        self._reset_buffers()
        time.sleep(self.reset_recovery_delay)
        return True

    def ping(self) -> bytes:
        return self.call("rpc.ping", b"")

    def wait_ready(self) -> None:
        deadline = time.monotonic() + self.timeout
        last_error: Exception | None = None
        attempts = 0
        reset_attempted = False
        reset_succeeded = False
        start = time.monotonic()

        while time.monotonic() < deadline:
            attempts += 1
            self._release_boot_straps()
            self._drain_input()
            try:
                if self.ping() == b"pong":
                    return
            except (TimeoutError, RuntimeError, ValueError) as exc:
                last_error = exc

                if isinstance(exc, TimeoutError) and not reset_attempted:
                    reset_attempted = True
                    try:
                        reset_succeeded = self._hard_reset()
                    except Exception as reset_error:
                        last_error = reset_error

                time.sleep(0.1)

        elapsed = time.monotonic() - start
        details = (
            f"port={self.port}, attempts={attempts}, "
            f"reset_attempted={reset_attempted}, reset_succeeded={reset_succeeded}, "
            f"elapsed={elapsed:.1f}s"
        )
        if last_error is not None:
            raise TimeoutError(f"RPC server did not become ready ({details}): {last_error}")
        raise TimeoutError(f"RPC server did not become ready ({details})")

    def _read_exact(self, size: int, *, timeout: float | None = None) -> bytes:
        if size == 0:
            return b""

        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = 0.05
            deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
            chunks = bytearray()
            while time.monotonic() < deadline:
                chunk = self.serial.proc.read(size - len(chunks))
                if chunk:
                    chunks.extend(chunk)
                    if len(chunks) >= size:
                        break
        finally:
            self.serial.proc.timeout = original_timeout
        if len(chunks) < size:
            raise TimeoutError(f"timeout reading {size} bytes, got {len(chunks)}")
        return bytes(chunks)

    def _read_response_header(self) -> bytes:
        magic_bytes = struct.pack("<I", RESPONSE_MAGIC)
        window = bytearray()
        deadline = time.monotonic() + self.timeout

        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = 0.05
            while time.monotonic() < deadline:
                byte = self.serial.proc.read(1)
                if not byte:
                    continue
                window.extend(byte)
                if len(window) > len(magic_bytes):
                    del window[: len(window) - len(magic_bytes)]
                if bytes(window) == magic_bytes:
                    return magic_bytes + self._read_exact(5, timeout=1.0)
        finally:
            self.serial.proc.timeout = original_timeout

        raise TimeoutError("timeout reading response magic")

    def call(self, method: str, payload: bytes) -> bytes:
        method_bytes = method.encode()
        header = struct.pack("<IHI", REQUEST_MAGIC, len(method_bytes), len(payload))

        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = 0.05
            self.serial.proc.write(header)
            self.serial.proc.write(method_bytes)
            self.serial.proc.write(payload)
            self.serial.proc.flush()

            resp_header = self._read_response_header()
            magic, status_value, payload_size = struct.unpack("<IBI", resp_header)
            if magic != RESPONSE_MAGIC:
                raise RuntimeError(f"bad response magic: 0x{magic:08X}")

            resp_payload = self._read_exact(payload_size)
            status = RpcStatus(status_value)
            if status is RpcStatus.OK:
                return resp_payload
            raise RuntimeError(f"{status.name}: {resp_payload.decode(errors='replace')}")
        finally:
            self.serial.proc.timeout = original_timeout
