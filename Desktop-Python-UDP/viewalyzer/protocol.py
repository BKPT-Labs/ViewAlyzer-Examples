# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0

"""
viewalyzer.protocol - Core VA protocol constants and packet builders.

This module contains everything needed for **generic schema-powered tracing**:
trace values (int32 / float), string messages, toggles, and function spans.
No RTOS dependency.

For RTOS-specific events (TaskSwitch, ISR, Semaphore, Mutex, Queue, etc.),
see ``viewalyzer.protocol_rtos``.

Wire format (a SUBSET of ViewAlyzerRecorder/core/ViewAlyzer.c, the
authoritative emitter - this module builds the common event/setup packets a
synthetic sender needs, not every code the firmware can emit). There is
exactly one format: WIRE_VERSION 1 = 32-bit little-endian
timestamps (the low word of a cycle counter; the host reconstructs 64 bits
from packet ordering) plus a rolling 8-bit **sequence byte** at offset 1 of
every packet, used by the host for exact loss detection, plus a periodic
``seq_checkpoint`` event (0x16) carrying the 32-bit absolute counter.

Builders return **seq-free packet bodies**; pass every body through
:class:`SeqStamper` before sending - the same architecture as the firmware,
where builders lay out packets and one central point stamps the counter.

All packet builders return raw bytes (not COBS-encoded).  The sender
class or your own code should COBS-encode each packet before sending.
"""

import struct
from enum import IntEnum

# ── Sync marker (bytes 3 and 9 carry the wire version) ──────────────────────

#: The wire protocol id. Bumps only on a breaking framing change; additive
#: event/setup codes do not bump it.
WIRE_VERSION = 1

SYNC_MARKER = bytes([0x56, 0x41, 0x5A, WIRE_VERSION,
                     0x53, 0x59, 0x4E, 0x43,
                     0x30, 0x30 + WIRE_VERSION, 0xAA, 0x55])

# ── Core event type codes (lower 7 bits of type byte) ───────────────────────

EVT_USER_TRACE       = 0x04   # int32 value → widget
EVT_USER_TOGGLE      = 0x0A   # boolean state change
EVT_USER_FUNCTION    = 0x0B   # function/span entry/exit → timeline
EVT_STRING_EVENT     = 0x0D   # variable-length string → log
EVT_FLOAT_TRACE      = 0x0E   # IEEE 754 float value → widget
EVT_SEQ_CHECKPOINT   = 0x16   # absolute sequence checkpoint

FLAG_START = 0x80              # MSB: start / enter

# ── Core setup packet codes ──────────────────────────────────────────────────

SETUP_USER_TRACE        = 0x72
SETUP_USER_FUNCTION_MAP = 0x76
SETUP_CONFIG_FLAGS      = 0x77
SETUP_INFO              = 0x7F

# ── Trace visualisation type hints ───────────────────────────────────────────

class TraceType(IntEnum):
    """Display type for a user trace channel (byte 2 of UserTrace setup).
    Mirrors VA_UserTraceType_t in core/ViewAlyzer.h - the firmware enum is
    the authority; do not invent values here."""
    GRAPH     = 0
    BAR       = 1
    GAUGE     = 2
    COUNTER   = 3
    TABLE     = 4
    HISTOGRAM = 5
    TOGGLE    = 6
    TASK      = 7
    ISR       = 8


# ── Helpers ──────────────────────────────────────────────────────────────────

def _ts_mask(ts: int) -> int:
    """Wire timestamps are the low 32 bits of the cycle counter."""
    return ts & 0xFFFFFFFF


class SeqStamper:
    """Rolling sequence counter - the sender-side mirror of the
    firmware's ``_va_emit_packet`` stamping.

    Call :meth:`stamp` on every packet body (events and setup packets
    alike) exactly once, in send order. Sync markers are NOT packets:
    never stamp them, they consume no sequence number.
    """

    def __init__(self):
        self.count = 0            # absolute packets stamped since start

    def stamp(self, body: bytes) -> bytes:
        """Insert this packet's rolling seq byte at offset 1."""
        pkt = bytes([body[0], self.count & 0xFF]) + body[1:]
        self.count += 1
        return pkt

    def checkpoint(self, timestamp: int) -> bytes:
        """Build + stamp a seq_checkpoint whose payload is its own
        absolute sequence number (the host cross-checks the low byte
        against the packet's seq byte)."""
        body = struct.pack("<BBI", EVT_SEQ_CHECKPOINT, 0x00,
                           _ts_mask(timestamp)) + struct.pack("<I", self.count)
        return self.stamp(body)


# ── Setup packet builders ───────────────────────────────────────────────────

def build_sync() -> bytes:
    """12-byte sync marker - send once at the start of a session (and
    periodically to help re-sync)."""
    return SYNC_MARKER


def build_info_clk(cpu_freq_hz: int) -> bytes:
    """Info packet declaring the CPU clock frequency."""
    payload = f"CLK:{cpu_freq_hz}".encode("ascii")
    return struct.pack("BBB", SETUP_INFO, 0x00, len(payload)) + payload


def build_session_start() -> bytes:
    """Info packet marking a fresh session (``SES:START``). The host resets
    its sequence expectation on this packet, so a sender restart is a new
    epoch rather than phantom loss."""
    payload = b"SES:START"
    return struct.pack("BBB", SETUP_INFO, 0x00, len(payload)) + payload


def build_user_trace_setup(trace_id: int, name: str,
                           trace_type: int = TraceType.GRAPH) -> bytes:
    """Setup::UserTrace - declare a trace channel (id, type, name)."""
    nb = name.encode("ascii")
    return struct.pack("BBBB", SETUP_USER_TRACE, trace_id, int(trace_type), len(nb)) + nb


def build_user_function_map(func_id: int, name: str) -> bytes:
    """Setup::UserFunctionMap - map a function ID to a name."""
    nb = name.encode("ascii")
    return struct.pack("BBB", SETUP_USER_FUNCTION_MAP, func_id, len(nb)) + nb


# ── Config flags (0x77) ─────────────────────────────────────────────────────
# The one setup code that carries no name string:
#     [0x77] [seq?] [group(1)] [value(4, little-endian)]
# Groups and bit positions mirror ViewAlyzerConfig.h and are a host protocol
# contract - append only, never renumber.
FLAG_GROUP_CATEGORIES = 0x01   # value = enabled VA_TRACE_* categories
FLAG_GROUP_BUILD      = 0x02   # value = build flags below
FLAG_GROUP_VERSION    = 0x03   # value = (major << 16) | (minor << 8) | patch

BUILD_NO_RTOS     = 1 << 0
BUILD_BUFFERED    = 1 << 1
BUILD_SEQ_COUNTER = 1 << 2
BUILD_HOST_TS     = 1 << 3     # timestamps stamped by the host, not a target
BUILD_TIMER_TS    = 1 << 4     # timestamps from a user timer; CLK: is its tick rate, not HCLK
BUILD_NO_CS       = 1 << 5     # built with VA_ALLOWED_TO_DISABLE_INTERRUPTS=0


def build_config_flags(group: int, value: int) -> bytes:
    """Setup::ConfigFlags - report how the sender was configured."""
    return struct.pack("<BBI", SETUP_CONFIG_FLAGS, group & 0xFF, value & 0xFFFFFFFF)


# ── Core event packet builders (seq-free v2 bodies) ─────────────────────────

def build_user_trace_int(trace_id: int, timestamp: int, value: int) -> bytes:
    """UserTrace (0x04) - 10-byte body, signed int32 value.
    No FLAG_START: a trace point has no start/end semantics and the firmware
    (the authoritative emitter) sends the bare code - parsers reject the
    flagged form as corruption."""
    tb = EVT_USER_TRACE
    return struct.pack("<BBIi", tb, trace_id, _ts_mask(timestamp), value)


def build_float_trace(trace_id: int, timestamp: int, value: float) -> bytes:
    """FloatTrace (0x0E) - 10-byte body, IEEE 754 float value.
    No FLAG_START (see build_user_trace_int)."""
    tb = EVT_FLOAT_TRACE
    return struct.pack("<BBIf", tb, trace_id, _ts_mask(timestamp), value)


def build_user_toggle(toggle_id: int, timestamp: int, state: bool) -> bytes:
    """UserToggle (0x0A) - 7-byte body."""
    return struct.pack("<BBIB", EVT_USER_TOGGLE, toggle_id,
                       _ts_mask(timestamp), 1 if state else 0)


def build_user_function(func_id: int, is_entry: bool, timestamp: int) -> bytes:
    """UserFunction (0x0B) - 6-byte body.  Entry/exit of a profiled function or span."""
    tb = EVT_USER_FUNCTION | (FLAG_START if is_entry else 0)
    return struct.pack("<BBI", tb, func_id, _ts_mask(timestamp))


def build_string_event(msg_id: int, timestamp: int, message: str) -> bytes:
    """StringEvent (0x0D) - 8 + N byte body (max 200 chars)."""
    mb = message.encode("ascii")[:200]
    return struct.pack("<BBI", EVT_STRING_EVENT, msg_id,
                       _ts_mask(timestamp)) + struct.pack("<H", len(mb)) + mb
