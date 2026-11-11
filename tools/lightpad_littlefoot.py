# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
LittleFoot for the Lightpad: Himmel's tiny LED renderer program
================================================================

A Lightpad draws its LEDs with a small bytecode program ("LittleFoot") that
runs on the block. To show our own pictures we upload one: it reads a 15 x 15
RGB565 bitmap from its heap and calls fillPixel for every LED, the same idea as
ROLI's BitmapLEDProgram. The host then only writes pixel bytes into the heap.

We do not compile LittleFoot source. This module assembles the two functions by
hand, from the program format and opcodes in ROLI's reference VM
(roli_blocks_basics: littlefoot/roli_LittleFootRunner.h, ISC license; the
opcode numbers were cross-checked with blocksd's opcodes.py, ISC). No code was
copied.

Program layout (little endian):
    checksum u16 | size u16 | num functions u16 | num globals u16 | heap bytes u16
    then per function: id u16 | code offset u16
    then the code. Jumps are absolute offsets from the program start.
    checksum: n = size; for every byte from offset 2: n = n + n * 2 + byte (16 bit).
The heap starts right after the program, so pixel byte k lives at block
offset (program size + k).

The program lives in the block's RAM only. Himmel never sends
saveProgramAsDefault, so a power cycle brings the factory program back.

MiniVM runs the program in Python with the same stack rules as the reference
VM, so the simulator shows exactly what the uploaded bytecode draws.

Self-test:  python tools/lightpad_littlefoot.py
"""

import struct
import sys

GRID = 15
BUTTON_EVENT = 1     # first int of our sendMessageToHost(1, button index, down) messages
PIXEL_BYTES = GRID * GRID * 2

OPS = {name: i for i, name in enumerate((
    "halt jump jumpIfTrue jumpIfFalse call retVoid retValue callNative drop dropMultiple "
    "pushMultiple0 push0 push1 push8 push16 push32 dup dupOffset_01 dupOffset_02 dupOffset_03 "
    "dupOffset_04 dupOffset_05 dupOffset_06 dupOffset_07 dupOffset dupOffset16 dropToStack "
    "dropToStack16 dupFromGlobal dropToGlobal int32ToFloat floatToInt32 add_int32 add_float "
    "mul_int32 mul_float sub_int32 sub_float div_int32 div_float mod_int32 bitwiseOr bitwiseAnd "
    "bitwiseXor bitwiseNot bitShiftLeft bitShiftRight logicalOr logicalAnd logicalNot "
    "testZE_int32 testNZ_int32 testGT_int32 testGE_int32 testLT_int32 testLE_int32 "
    "testZE_float testNZ_float testGT_float testGE_float testLT_float testLE_float "
    "getHeapByte getHeapInt getHeapBits setHeapByte setHeapInt").split())}
ARG8 = {"retVoid", "retValue", "dropMultiple", "pushMultiple0", "push8", "dupOffset", "dropToStack"}
ARG16 = {"jump", "jumpIfTrue", "jumpIfFalse", "call", "callNative", "push16", "dupOffset16",
         "dropToStack16", "dupFromGlobal", "dropToGlobal"}
ARG32 = {"push32"}
NAMES = {v: k for k, v in OPS.items()}


def function_id(signature):
    """NativeFunction::createID: 31-hash of name + argument types (the return type is skipped),
    plus the length of the whole signature, as int16."""
    slash = signature.index("/")
    h = 0
    for i, ch in enumerate(signature):
        if i != slash + 1:
            h = (h * 31 + ord(ch)) & 0xFFFFFFFF
    v = (h + len(signature)) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def checksum(prog):
    n = len(prog) & 0xFFFF
    for b in prog[2:]:
        n = (n + n * 2 + b) & 0xFFFF
    return n


class Asm:
    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.fixups = []

    def op(self, name, arg=None):
        self.code.append(OPS[name])
        if name in ARG8:
            self.code += struct.pack("<b", arg)
        elif name in ARG16:
            if isinstance(arg, str):
                self.fixups.append((len(self.code), arg))
                arg = 0
            self.code += struct.pack("<h", arg)
        elif name in ARG32:
            self.code += struct.pack("<i", arg)

    def label(self, name):
        self.labels[name] = len(self.code)

    def push(self, v):
        if v == 0:
            self.op("push0")
        elif v == 1:
            self.op("push1")
        elif -128 <= v < 128:
            self.op("push8", v)
        elif -32768 <= v < 32768:
            self.op("push16", v)
        else:
            self.op("push32", v)

    def dup_depth(self, d):
        """Copy the stack item at depth d (0 = top) to the top."""
        if d == 0:
            self.op("dup")
        elif d <= 7:
            self.op("dupOffset_%02d" % d)
        else:
            self.op("dupOffset", d)


def build_key_program(keys=24):
    """The same renderer for a LUMI Keys: one RGB565 pixel per key, drawn with fillPixel(colour, key, 0).
    initialise() also keeps the block's own musical key handling (notes, pressure, glide as MIDI) and
    hands only the lighting to us: setUseDefaultKeyHandler(true, false), as blocksd documents for
    LUMI firmware 1.3.0 or newer."""
    return build_bitmap_program(heap_bytes=keys * 2, width=keys, height=1, keep_key_handler=True)


def build_bitmap_program(heap_bytes=PIXEL_BYTES, width=GRID, height=GRID, keep_key_handler=False):
    """initialise(): status overlay off.  repaint(): width x height x fillPixel(makeARGB(255, r, g, b), x, y)
    with r, g, b read from the RGB565 heap (bit = i * 16: r 5 bits, g 6 bits, b 5 bits).
    The Lightpad uses the defaults (15 x 15)."""
    funcs = []
    # --- initialise() ---
    a = Asm()
    a.push(0)
    a.op("callNative", function_id("setStatusOverlayActive/vb"))
    if keep_key_handler:                  # setUseDefaultKeyHandler(musical = true, lighting = false)
        a.push(0)                         # second argument first
        a.push(1)
        a.op("callNative", function_id("setUseDefaultKeyHandler/vbb"))
    a.op("retVoid", 0)
    funcs.append(("initialise/v", a))
    # --- handleButtonDown(index) / handleButtonUp(index) ---
    # With our program on the block, the firmware no longer sends button messages to the host
    # (seen on a Lightpad M), so the program forwards them: sendMessageToHost(1, index, down).
    for sig, down in (("handleButtonDown/vi", 1), ("handleButtonUp/vi", 0)):
        a = Asm()                             # entry: [index, ret]
        a.push(down)                          # third argument (pushed first)
        a.dup_depth(2)                        # second: index
        a.push(BUTTON_EVENT)                  # first: what this message is
        a.op("callNative", function_id("sendMessageToHost/viii"))
        a.op("retVoid", 1)
        funcs.append((sig, a))
    # --- repaint() ---  local i is the only stack slot under the work items
    a = Asm()
    a.op("pushMultiple0", 1)              # i = 0             [ret, i]
    a.label("loop")
    a.dup_depth(0)                        # y = i / width     [i, i]
    a.push(width)
    a.op("div_int32")                     #                   [i, y]
    a.dup_depth(1)                        # x = i % width
    a.push(width)
    a.op("mod_int32")                     #                   [i, y, x]
    for shift_bits, bit_off, n, depth in ((3, 11, 5, 3), (2, 5, 6, 4), (3, 0, 5, 5)):  # b, g, r
        a.push(n)                         # numBits (getHeapBits pops start first, then numBits)
        a.dup_depth(depth)                # i
        a.push(16)
        a.op("mul_int32")
        if bit_off:
            a.push(bit_off)
            a.op("add_int32")
        a.op("getHeapBits")
        a.push(shift_bits)
        a.op("bitShiftLeft")
    a.push(255)                           # alpha: first argument, so pushed last
    a.op("callNative", function_id("makeARGB/iiiii"))   # [i, y, x, colour]
    a.op("callNative", function_id("fillPixel/viii"))   # fillPixel(colour, x, y) -> [i]
    a.push(1)
    a.op("add_int32")                     # i += 1
    a.dup_depth(0)
    a.push(width * height)
    a.op("sub_int32")
    a.op("testLT_int32")
    a.op("jumpIfTrue", "loop")
    a.op("drop")                          # back to the return address
    a.op("retVoid", 0)
    funcs.append(("repaint/v", a))

    header = 10 + 4 * len(funcs)
    code = bytearray()
    table = bytearray()
    for sig, asm in funcs:
        base = header + len(code)
        for pos, lab in asm.fixups:
            struct.pack_into("<h", asm.code, pos, base + asm.labels[lab])
        table += struct.pack("<hH", function_id(sig), base)
        code += asm.code
    prog = bytearray(struct.pack("<HHHHH", 0, header + len(code), len(funcs), 0, heap_bytes)) + table + code
    struct.pack_into("<H", prog, 0, checksum(prog))
    return bytes(prog)


def parse_program(block):
    """(size, heap_size, {function id: offset}) if `block` starts with a valid program, else None."""
    if len(block) < 10:
        return None
    _, size, nfun, _, heap = struct.unpack_from("<HHHHH", block, 0)
    if size < 10 or size > len(block) or 10 + 4 * nfun > size:
        return None
    if checksum(bytes(block[:size])) != struct.unpack_from("<H", block, 0)[0]:
        return None
    funcs = {}
    for k in range(nfun):
        fid, off = struct.unpack_from("<hH", block, 10 + 4 * k)
        funcs[fid] = off
    return size, heap, funcs


class MiniVM:
    """Just enough of the reference VM to run our renderer (and fail loudly on anything else)."""

    MAX_OPS = 200000

    def __init__(self, block, width=GRID, height=GRID):
        p = parse_program(block)
        if p is None:
            raise ValueError("no valid program")
        self.size, self.heap_size, self.funcs = p
        self.prog = bytes(block[:self.size])
        self.heap = bytes(block[self.size:self.size + self.heap_size])
        self.width, self.height = width, height
        self.pixels = [(0, 0, 0)] * (width * height)
        self.key_handler = None   # (musical, lighting) after setUseDefaultKeyHandler
        self.overlay = True
        self.to_host = []        # sendMessageToHost calls: (a, b, c)

    def heap_bits(self, start, n):
        if start + n > 8 * len(self.heap):
            return 0
        v = int.from_bytes(self.heap, "little")
        return (v >> start) & ((1 << n) - 1)

    def native(self, fid, args):
        if fid == function_id("makeARGB/iiiii"):
            a, r, g, b = args
            return ((a & 255) << 24) | ((r & 255) << 16) | ((g & 255) << 8) | (b & 255), 4, False
        if fid == function_id("fillPixel/viii"):
            c, x, y = args[:3]
            if 0 <= x < self.width and 0 <= y < self.height:
                self.pixels[y * self.width + x] = ((c >> 16) & 255, (c >> 8) & 255, c & 255)
            return 0, 3, True
        if fid == function_id("setUseDefaultKeyHandler/vbb"):
            self.key_handler = (bool(args[0]), bool(args[1]))
            return 0, 2, True
        if fid == function_id("sendMessageToHost/viii"):
            self.to_host.append(tuple(args[:3]))
            return 0, 3, True
        if fid == function_id("setStatusOverlayActive/vb"):
            self.overlay = bool(args[0])
            return 0, 1, True
        raise ValueError("unknown native function %d" % fid)

    def call(self, signature, *args):
        fid = function_id(signature)
        if fid not in self.funcs:
            return False
        pc = self.funcs[fid]
        # list end = top (the reference VM keeps tos in a register, same thing). Like
        # FunctionExecutionContext: 0, then the arguments (first argument nearest the top), then the
        # return address 0.
        stack = [0] + list(reversed(args)) + [0]
        prog = self.prog
        for _ in range(self.MAX_OPS):
            if pc >= self.size:
                raise ValueError("ran off the program")
            name = NAMES.get(prog[pc])
            pc += 1
            arg = None
            if name in ARG8:
                arg = struct.unpack_from("<b", prog, pc)[0]
                pc += 1
            elif name in ARG16:
                arg = struct.unpack_from("<h", prog, pc)[0]
                pc += 2
            elif name in ARG32:
                arg = struct.unpack_from("<i", prog, pc)[0]
                pc += 4
            if name == "retVoid":
                if stack[-1] == 0:
                    return True
                raise ValueError("nested calls not supported")
            elif name == "pushMultiple0":
                stack += [0] * arg
            elif name in ("push0", "push1", "push8", "push16", "push32"):
                stack.append({"push0": 0, "push1": 1}.get(name, arg))
            elif name == "dup":
                stack.append(stack[-1])
            elif name.startswith("dupOffset"):
                d = arg if name in ("dupOffset", "dupOffset16") else int(name[-2:])
                stack.append(stack[-1 - d])
            elif name == "drop":
                stack.pop()
            elif name in ("add_int32", "sub_int32", "mul_int32", "div_int32", "mod_int32", "bitShiftLeft"):
                b, a = stack.pop(), stack.pop()
                if name in ("div_int32", "mod_int32") and b == 0:
                    raise ValueError("division by zero")
                stack.append({"add_int32": a + b, "sub_int32": a - b, "mul_int32": a * b,
                              "div_int32": int(a / b), "mod_int32": a - b * int(a / b),
                              "bitShiftLeft": a << b}[name])
            elif name == "testLT_int32":
                stack.append(1 if stack.pop() < 0 else 0)
            elif name == "jumpIfTrue":
                if stack.pop():
                    pc = arg
            elif name == "jump":
                pc = arg
            elif name == "getHeapBits":
                start = stack.pop()
                n = stack.pop()
                stack.append(self.heap_bits(start, n))
            elif name == "callNative":
                n_args = {function_id("makeARGB/iiiii"): 4, function_id("fillPixel/viii"): 3,
                          function_id("setStatusOverlayActive/vb"): 1,
                          function_id("sendMessageToHost/viii"): 3,
                          function_id("setUseDefaultKeyHandler/vbb"): 2}.get(arg, 0)
                args = [stack[-1 - k] for k in range(n_args)]   # first argument on top
                res, n, void = self.native(arg, args)
                del stack[len(stack) - n:]
                if not void:
                    stack.append(res)
            else:
                raise ValueError("opcode %s not in MiniVM" % name)
        raise ValueError("timed out")


def selftest():
    prog = build_bitmap_program()
    ok = parse_program(prog) is not None
    print(("ok   " if ok else "FAIL ") + "program %d bytes, checksum valid" % len(prog))
    # a heap with a known pattern: every pixel a different colour
    import lightpad_sim as L
    want = [((x * 17) & 0xF8, (y * 17) & 0xFC, ((x + y) * 9) & 0xF8) for y in range(GRID) for x in range(GRID)]
    block = bytearray(prog) + L.pixels_to_bytes(want)
    vm = MiniVM(block)
    vm.call("initialise/v")
    vm.call("repaint/v")
    good = vm.pixels == want and not vm.overlay
    print(("ok   " if good else "FAIL ") + "MiniVM draws the heap exactly (225 pixels), overlay off")
    vm.call("handleButtonDown/vi", 0)
    vm.call("handleButtonUp/vi", 0)
    b = vm.to_host == [(BUTTON_EVENT, 0, 1), (BUTTON_EVENT, 0, 0)]
    print(("ok   " if b else "FAIL ") + "Mode button forwarded to the host: %s" % vm.to_host)
    good = good and b
    kp = build_key_program()
    want_k = [((k * 10) & 0xF8, (255 - k * 10) & 0xFC, (k * 5) & 0xF8) for k in range(24)]
    vm = MiniVM(bytearray(kp) + L.pixels_to_bytes(want_k), width=24, height=1)
    vm.call("initialise/v")
    vm.call("repaint/v")
    k_ok = vm.pixels == want_k and vm.key_handler == (True, False) and not vm.overlay
    print(("ok   " if k_ok else "FAIL ") + "LUMI key renderer: %d bytes, 24 keys drawn, musical handler kept, lighting ours" % len(kp))
    good = good and k_ok
    print("function ids: repaint %d, fillPixel %d, makeARGB %d" % (
        function_id("repaint/v"), function_id("fillPixel/viii"), function_id("makeARGB/iiiii")))
    print("program:", prog.hex(" "))
    return ok and good


if __name__ == "__main__":
    sys.exit(0 if selftest() else 1)
