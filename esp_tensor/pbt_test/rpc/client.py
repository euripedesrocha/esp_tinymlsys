from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from enum import IntEnum


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

    def __enter__(self) -> RpcClient:
        self._release_boot_straps()
        self._killed = self.serial.stop_redirect_thread()
        self._release_boot_straps()
        self.wait_ready()
        return self

    def __exit__(self, *args) -> None:
        self._release_boot_straps()
        if self._killed:
            self.serial.start_redirect_thread()

    def _release_boot_straps(self) -> None:
        self.serial.proc.dtr = False
        self.serial.proc.rts = False

    def synchronize(self) -> None:
        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = 0.05
            while self.serial.proc.read(256):
                pass
        finally:
            self.serial.proc.timeout = original_timeout

    def ping(self) -> bytes:
        return self.call("rpc.ping", b"")

    def wait_ready(self) -> None:
        deadline = time.monotonic() + self.timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            self.synchronize()
            try:
                if self.ping() == b"pong":
                    return
            except (TimeoutError, RuntimeError, ValueError) as exc:
                last_error = exc
        if last_error is not None:
            raise TimeoutError(f"RPC server did not become ready: {last_error}")
        raise TimeoutError("RPC server did not become ready")

    def _read_exact(self, size: int) -> bytes:
        if size == 0:
            return b""

        original_timeout = self.serial.proc.timeout
        try:
            self.serial.proc.timeout = 0.05
            deadline = time.monotonic() + self.timeout
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

            resp_header = self._read_exact(9)
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
