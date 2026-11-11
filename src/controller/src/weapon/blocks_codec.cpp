// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// BLOCKS codec, LittleFoot renderer and HeapSync (see blocks.h). Line-by-line port
// of tools/lightpad_sim.py and tools/lightpad_littlefoot.py; weapon_tests.cpp checks
// the bytes against the Python output.
#include "himmel/weapon/blocks.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace himmel::weapon::blocks {

// ---------------------------------------------------------------- bits
BitWriter& BitWriter::put(uint32_t value, int bits) {
    for (int i = 0; i < bits; ++i) {
        const size_t byte = nbits_ / 7;
        if (byte >= b_.size()) return *this;  // never with our packet sizes
        if ((value >> i) & 1u) b_[byte] = static_cast<uint8_t>(b_[byte] | (1u << (nbits_ % 7)));
        ++nbits_;
    }
    return *this;
}

uint32_t BitReader::get(int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; ++i) {
        if (pos_ < total_ && ((d_[pos_ / 7] & 0x7F) >> (pos_ % 7)) & 1u) v |= 1u << i;
        ++pos_;
    }
    return v;
}

uint8_t checksum(const uint8_t* body, size_t n) {
    uint32_t c = static_cast<uint32_t>(n) & 0xFF;
    for (size_t i = 0; i < n; ++i) c = (c + c * 2 + body[i]) & 0xFF;
    return static_cast<uint8_t>(c & 0x7F);
}

MidiMsg frame(uint8_t index, const BitWriter& w) {
    MidiMsg m;
    std::memcpy(m.b, kHeader, 5);
    m.b[5] = index & 0x7F;
    const size_t n = w.nbytes();
    std::memcpy(m.b + 6, w.bytes(), n);
    m.b[6 + n] = checksum(w.bytes(), n);
    m.b[7 + n] = 0xF7;
    m.len = static_cast<uint16_t>(8 + n);
    return m;
}

bool unframe(const uint8_t* msg, size_t n, uint8_t& index, const uint8_t*& body, size_t& body_len) {
    if (n < 9 || std::memcmp(msg, kHeader, 5) != 0 || msg[n - 1] != 0xF7) return false;
    body = msg + 6;
    body_len = n - 8;
    if (checksum(body, body_len) != msg[n - 2]) return false;
    index = msg[5];
    return true;
}

MidiMsg host_command(uint32_t cmd, uint8_t index) {
    BitWriter w;
    w.put(kHostCommand, 7).put(cmd, 9);
    return frame(index, w);
}

MidiMsg host_config_set(uint32_t item, int32_t value, uint8_t index) {
    BitWriter w;
    w.put(kHostConfig, 7).put(0, 4).put(item, 8).put(static_cast<uint32_t>(value), 32);
    return frame(index, w);
}

MidiMsg host_config_request(uint32_t item, uint8_t index) {
    // command 1, then 32 bits of 0, then the item (as ROLI's addRequestMessage; checked on a Lightpad M)
    BitWriter w;
    w.put(kHostConfig, 7).put(1, 4).put(0, 32).put(item, 8);
    return frame(index, w);
}

MidiMsg serial_request() {
    MidiMsg m;
    std::memcpy(m.b, kSerialRequest, sizeof(kSerialRequest));
    m.len = sizeof(kSerialRequest);
    return m;
}

// ---------------------------------------------------------------- sharedDataChange
namespace {
void skip(BitWriter& w, size_t count) {
    while (count > 0) {
        const size_t k = std::min<size_t>(count, 255);
        if (k > 15) w.put(3, 3).put(static_cast<uint32_t>(k), 8);  // skipBytesMany
        else w.put(2, 3).put(static_cast<uint32_t>(k), 4);         // skipBytesFew
        count -= k;
    }
}
}  // namespace

void host_data_changes(uint16_t packet_index, const uint8_t* old_data, const uint8_t* new_data, size_t n,
                       std::vector<DataPacket>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < n && old_data[pos] == new_data[pos]) ++pos;
    if (pos >= n) return;
    while (pos < n) {
        packet_index &= 0x3FF;
        BitWriter w;
        w.put(kHostData, 7).put(packet_index, 16);
        skip(w, pos);
        uint8_t last = 0;
        while (pos < n && w.bits() < kMaxPacketBits) {
            if (old_data[pos] == new_data[pos]) {
                size_t k = pos;
                while (k < n && old_data[k] == new_data[k]) ++k;
                if (k >= n) {
                    pos = n;
                    break;
                }
                skip(w, k - pos);
                pos = k;
                continue;
            }
            size_t run = 1;
            while (pos + run < n && new_data[pos + run] == new_data[pos] && run < 255) ++run;
            if (run >= 3) {
                const uint8_t v = new_data[pos];
                if (run > 15) w.put(7, 3).put(static_cast<uint32_t>(run), 8).put(v, 8);  // setManyBytesWithValue
                else if (v == last) w.put(6, 3).put(static_cast<uint32_t>(run), 4);       // setFewBytesWithLastValue
                else w.put(5, 3).put(static_cast<uint32_t>(run), 4).put(v, 8);           // setFewBytesWithValue
                last = v;
                pos += run;
                continue;
            }
            // a mixed sequence up to the next unchanged byte or the next run of 3
            uint8_t seq[32];
            size_t len = 0;
            while (pos < n && old_data[pos] != new_data[pos] && len < 32) {
                if (pos + 2 < n && new_data[pos] == new_data[pos + 1] && new_data[pos] == new_data[pos + 2] && len) break;
                seq[len++] = new_data[pos++];
            }
            w.put(4, 3);  // setSequenceOfBytes
            for (size_t i = 0; i < len; ++i) w.put(seq[i], 8).put(i + 1 < len ? 1 : 0, 1);
            last = seq[len - 1];
        }
        const bool done = pos >= n;
        w.put(done ? 1 : 0, 3);  // endOfChanges / endOfPacket
        out.push_back({static_cast<uint16_t>(packet_index & 0x3FF), frame(0, w)});
        ++packet_index;
    }
}

bool apply_data_change(BitReader& r, uint8_t* d, size_t n, uint16_t& packet) {
    if (r.left() < 16) return false;
    packet = static_cast<uint16_t>(r.get(16) & 0x3FF);
    size_t pos = 0;
    uint8_t last = 0;
    auto fill = [&](size_t k, uint8_t v) {
        for (size_t i = 0; i < k && pos + i < n; ++i) d[pos + i] = v;
        pos += k;
    };
    while (r.left() >= 3) {
        const uint32_t c = r.get(3);
        if (c == 0 || c == 1) break;
        if (c == 2) pos += r.get(4);
        else if (c == 3) pos += r.get(8);
        else if (c == 4) {
            for (;;) {
                const auto v = static_cast<uint8_t>(r.get(8));
                const uint32_t more = r.get(1);
                if (pos < n) d[pos] = v;
                ++pos;
                last = v;
                if (!more || r.left() < 9) break;
            }
        } else if (c == 5) {
            const uint32_t k = r.get(4);
            const auto v = static_cast<uint8_t>(r.get(8));
            fill(k, v);
            last = v;
        } else if (c == 6) {
            fill(r.get(4), last);
        } else {
            const uint32_t k = r.get(8);
            const auto v = static_cast<uint8_t>(r.get(8));
            fill(k, v);
            last = v;
        }
    }
    return true;
}

// ---------------------------------------------------------------- decoding
namespace {
int32_t s32(uint32_t v) { return static_cast<int32_t>(v); }
}  // namespace

int decode_device(const uint8_t* msg, size_t n, DeviceEvent* out, int cap) {
    if (cap <= 0) return 0;
    if (n >= 6 && std::memcmp(msg, kSerialHeader, 5) == 0 && std::memcmp(msg, kHeader, 5) != 0) {
        const std::string s(reinterpret_cast<const char*>(msg), n);
        const size_t i = s.find("48:B6:20:");
        DeviceEvent e{};
        e.kind = DeviceEvent::Serial;
        const std::string serial = i != std::string::npos && i + 17 < s.size() ? s.substr(i + 17, 16) : "?";
        std::snprintf(e.text, sizeof(e.text), "%s", serial.c_str());
        for (char& c : e.text)
            if (c && (c < 32 || c > 126)) c = 0;
        out[0] = e;
        return 1;
    }
    uint8_t index = 0;
    const uint8_t* body = nullptr;
    size_t blen = 0;
    if (!unframe(msg, n, index, body, blen)) return 0;
    BitReader r(body, blen);
    if (r.left() < 32) return 0;
    const uint32_t ts = r.get(32);
    int count = 0;
    while (r.left() >= 7 && count < cap) {
        const uint32_t t = r.get(7);
        if (t == 0) break;
        DeviceEvent e{};
        e.kind = DeviceEvent::Unknown;
        e.t = ts;
        if (t >= kTouchStart && t <= kTouchEndV) {
            const uint32_t off = r.get(5);
            e.kind = DeviceEvent::Touch;
            e.t = ts + off;
            e.index = static_cast<uint8_t>(r.get(5));
            e.x = static_cast<uint16_t>(r.get(12));
            e.y = static_cast<uint16_t>(r.get(12));
            e.z = static_cast<uint8_t>(r.get(8));
            e.phase = static_cast<uint8_t>((t - kTouchStart) % 3);
            if (t >= kTouchStartV) {
                e.has_velocity = true;
                e.vx = static_cast<int8_t>(r.get(8));
                e.vy = static_cast<int8_t>(r.get(8));
                e.vz = static_cast<int8_t>(r.get(8));
            }
        } else if (t == kButtonDown || t == kButtonUp) {
            const uint32_t off = r.get(5);
            e.kind = DeviceEvent::Button;
            e.t = ts + off;
            e.id = r.get(12);
            e.down = t == kButtonDown;
        } else if (t == kDevAck) {
            e.kind = DeviceEvent::Ack;
            e.id = r.get(10);
        } else if (t == kDevTopology) {
            e.kind = DeviceEvent::Topology;
            r.get(8);  // version
            const uint32_t nd = r.get(7), nc = r.get(8);
            for (uint32_t k = 0; k < nd; ++k) {
                char serial[17] = {};
                for (int c = 0; c < 16; ++c) serial[c] = static_cast<char>(r.get(7));
                r.get(7);  // topology index
                const uint32_t battery = r.get(5), charging = r.get(1);
                if (k == 0) {
                    std::snprintf(e.text, sizeof(e.text), "%s", serial);
                    e.battery = static_cast<uint8_t>(battery);
                    e.charging = charging != 0;
                }
            }
            for (uint32_t k = 0; k < nc; ++k) r.get(7 + 5 + 7 + 5);
        } else if (t == kDevProgramEvent) {
            const int32_t a = s32(r.get(32)), b = s32(r.get(32)), c = s32(r.get(32));
            if (a == kButtonEvent) {  // our renderer forwards the buttons this way
                e.kind = DeviceEvent::Button;
                e.id = static_cast<uint32_t>(b);
                e.down = c != 0;
                e.via_program = true;
            } else {
                e.kind = DeviceEvent::Program;
                e.value = a, e.min = b, e.max = c;
            }
        } else if (t == kDevConfig) {
            const uint32_t cmd = r.get(4);
            e.kind = DeviceEvent::Config;
            if ((cmd == 4 || cmd == 5) && r.left() >= 8 + 96) {
                e.id = r.get(8);
                e.value = s32(r.get(32));
                e.min = s32(r.get(32));
                e.max = s32(r.get(32));
                out[count++] = e;
                if (cmd == 5) break;  // user config metadata follows; not decoded
                continue;
            }
            e.id = 0xFFFFFFFF;  // a config command we do not decode
            out[count++] = e;
            break;
        } else if (t == kDevVersion || t == kDevName) {
            e.kind = t == kDevVersion ? DeviceEvent::Version : DeviceEvent::Name;
            e.index = static_cast<uint8_t>(r.get(7));
            const uint32_t len = r.get(7);
            uint32_t k = 0;
            for (; k < len && r.left() >= 7; ++k) {
                const auto c = static_cast<char>(r.get(7));
                if (k < sizeof(e.text) - 1) e.text[k] = c;
            }
        } else {
            e.id = t;
            out[count++] = e;
            break;
        }
        out[count++] = e;
    }
    return count;
}

// ---------------------------------------------------------------- LittleFoot
namespace {

// Opcode numbers of ROLI's reference VM (cross-checked with blocksd's opcodes.py).
enum Op : uint8_t {
    halt, jump, jumpIfTrue, jumpIfFalse, call, retVoid, retValue, callNative, drop, dropMultiple,
    pushMultiple0, push0, push1, push8, push16, push32, dup, dupOffset_01, dupOffset_02, dupOffset_03,
    dupOffset_04, dupOffset_05, dupOffset_06, dupOffset_07, dupOffset, dupOffset16, dropToStack,
    dropToStack16, dupFromGlobal, dropToGlobal, int32ToFloat, floatToInt32, add_int32, add_float,
    mul_int32, mul_float, sub_int32, sub_float, div_int32, div_float, mod_int32, bitwiseOr, bitwiseAnd,
    bitwiseXor, bitwiseNot, bitShiftLeft, bitShiftRight, logicalOr, logicalAnd, logicalNot,
    testZE_int32, testNZ_int32, testGT_int32, testGE_int32, testLT_int32, testLE_int32,
    testZE_float, testNZ_float, testGT_float, testGE_float, testLT_float, testLE_float,
    getHeapByte, getHeapInt, getHeapBits, setHeapByte, setHeapInt,
};

struct Asm {
    std::vector<uint8_t> code;
    size_t loop = 0;                  // the one label we need
    std::vector<size_t> loop_fixups;  // positions of 16-bit args that jump to it

    void op(Op o) { code.push_back(o); }
    void op8(Op o, int8_t a) {
        code.push_back(o);
        code.push_back(static_cast<uint8_t>(a));
    }
    void op16(Op o, int16_t a) {
        code.push_back(o);
        code.push_back(static_cast<uint8_t>(a & 0xFF));
        code.push_back(static_cast<uint8_t>((a >> 8) & 0xFF));
    }
    void op32(Op o, int32_t a) {
        code.push_back(o);
        for (int k = 0; k < 4; ++k) code.push_back(static_cast<uint8_t>((a >> (8 * k)) & 0xFF));
    }
    void push(int v) {
        if (v == 0) op(push0);
        else if (v == 1) op(push1);
        else if (v >= -128 && v < 128) op8(push8, static_cast<int8_t>(v));
        else if (v >= -32768 && v < 32768) op16(push16, static_cast<int16_t>(v));
        else op32(push32, v);
    }
    void dup_depth(int d) {
        if (d == 0) op(dup);
        else if (d <= 7) op(static_cast<Op>(dupOffset_01 + d - 1));
        else op8(dupOffset, static_cast<int8_t>(d));
    }
    void native(std::string_view sig) { op16(callNative, function_id(sig)); }
};

void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = static_cast<uint8_t>(x & 0xFF);
    v[at + 1] = static_cast<uint8_t>(x >> 8);
}

}  // namespace

int16_t function_id(std::string_view sig) {
    const size_t slash = sig.find('/');
    uint32_t h = 0;
    for (size_t i = 0; i < sig.size(); ++i)
        if (i != slash + 1) h = h * 31 + static_cast<uint8_t>(sig[i]);
    return static_cast<int16_t>(static_cast<uint16_t>((h + static_cast<uint32_t>(sig.size())) & 0xFFFF));
}

uint16_t program_checksum(const uint8_t* prog, size_t n) {
    uint32_t c = static_cast<uint32_t>(n) & 0xFFFF;
    for (size_t i = 2; i < n; ++i) c = (c + c * 2 + prog[i]) & 0xFFFF;
    return static_cast<uint16_t>(c);
}

std::vector<uint8_t> build_bitmap_program(int heap_bytes, int width, int height, bool keep_key_handler) {
    struct Fn {
        const char* sig;
        Asm a;
    };
    std::vector<Fn> funcs;
    {  // initialise(): status overlay off (and keep the musical key handler on a LUMI)
        Asm a;
        a.push(0);
        a.native("setStatusOverlayActive/vb");
        if (keep_key_handler) {  // setUseDefaultKeyHandler(musical = true, lighting = false)
            a.push(0);
            a.push(1);
            a.native("setUseDefaultKeyHandler/vbb");
        }
        a.op8(retVoid, 0);
        funcs.push_back({"initialise/v", std::move(a)});
    }
    // handleButtonDown/Up(index): sendMessageToHost(1, index, down) - with our program on the
    // block the firmware no longer sends button messages to the host (Lightpad M).
    for (int down = 1; down >= 0; --down) {
        Asm a;
        a.push(down);
        a.dup_depth(2);
        a.push(kButtonEvent);
        a.native("sendMessageToHost/viii");
        a.op8(retVoid, 1);
        funcs.push_back({down ? "handleButtonDown/vi" : "handleButtonUp/vi", std::move(a)});
    }
    {  // repaint(): for i: fillPixel(makeARGB(255, r, g, b), i % width, i / width)
        Asm a;
        a.op8(pushMultiple0, 1);
        a.loop = a.code.size();
        a.dup_depth(0);
        a.push(width);
        a.op(div_int32);
        a.dup_depth(1);
        a.push(width);
        a.op(mod_int32);
        const int steps[3][4] = {{3, 11, 5, 3}, {2, 5, 6, 4}, {3, 0, 5, 5}};  // b, g, r: shift, bit offset, bits, depth
        for (const auto& s : steps) {
            a.push(s[2]);
            a.dup_depth(s[3]);
            a.push(16);
            a.op(mul_int32);
            if (s[1]) {
                a.push(s[1]);
                a.op(add_int32);
            }
            a.op(getHeapBits);
            a.push(s[0]);
            a.op(bitShiftLeft);
        }
        a.push(255);
        a.native("makeARGB/iiiii");
        a.native("fillPixel/viii");
        a.push(1);
        a.op(add_int32);
        a.dup_depth(0);
        a.push(width * height);
        a.op(sub_int32);
        a.op(testLT_int32);
        a.loop_fixups.push_back(a.code.size() + 1);
        a.op16(jumpIfTrue, 0);
        a.op(drop);
        a.op8(retVoid, 0);
        funcs.push_back({"repaint/v", std::move(a)});
    }
    const size_t header = 10 + 4 * funcs.size();
    std::vector<uint8_t> code, table;
    for (auto& f : funcs) {
        const size_t base = header + code.size();
        for (size_t pos : f.a.loop_fixups) put16(f.a.code, pos, static_cast<uint16_t>(base + f.a.loop));
        const auto id = static_cast<uint16_t>(function_id(f.sig));
        table.push_back(static_cast<uint8_t>(id & 0xFF));
        table.push_back(static_cast<uint8_t>(id >> 8));
        table.push_back(static_cast<uint8_t>(base & 0xFF));
        table.push_back(static_cast<uint8_t>(base >> 8));
        code.insert(code.end(), f.a.code.begin(), f.a.code.end());
    }
    std::vector<uint8_t> prog(10, 0);
    put16(prog, 2, static_cast<uint16_t>(header + code.size()));
    put16(prog, 4, static_cast<uint16_t>(funcs.size()));
    put16(prog, 6, 0);
    put16(prog, 8, static_cast<uint16_t>(heap_bytes));
    prog.insert(prog.end(), table.begin(), table.end());
    prog.insert(prog.end(), code.begin(), code.end());
    put16(prog, 0, program_checksum(prog.data(), prog.size()));
    return prog;
}

std::vector<uint8_t> build_key_program(int keys) { return build_bitmap_program(keys * 2, keys, 1, true); }

bool parse_program(const uint8_t* b, size_t n, ProgramInfo& info) {
    if (n < 10) return false;
    auto u16 = [&](size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); };
    const uint16_t size = u16(2), nfun = u16(4), heap = u16(8);
    if (size < 10 || size > n || 10u + 4u * nfun > size) return false;
    if (program_checksum(b, size) != u16(0)) return false;
    info = {size, heap, nfun};
    return true;
}

void pixels_to_bytes(const uint8_t (*rgb)[3], int count, uint8_t* out) {
    for (int i = 0; i < count; ++i) {
        const uint16_t v = rgb565(rgb[i][0], rgb[i][1], rgb[i][2]);
        out[2 * i] = static_cast<uint8_t>(v & 0xFF);
        out[2 * i + 1] = static_cast<uint8_t>(v >> 8);
    }
}

// ---------------------------------------------------------------- HeapSync
void HeapSync::reset() {
    known_ = false;
    target_.fill(0);
    inflight_.clear();
    out_.clear();
    index_ = 0;
    sent_at_ = started_ = 0;
}

const std::vector<DataPacket>& HeapSync::start(const uint8_t* target, uint64_t t) {
    out_.clear();
    std::array<uint8_t, kDataBlock> inverted;
    const uint8_t* old = confirmed_.data();
    if (!known_) {  // unknown block contents: write every byte
        for (size_t i = 0; i < kDataBlock; ++i) inverted[i] = static_cast<uint8_t>(target[i] ^ 0xFF);
        old = inverted.data();
    }
    host_data_changes(static_cast<uint16_t>((index_ + 1) & 0x3FF), old, target, kDataBlock, out_);
    std::memcpy(target_.data(), target, kDataBlock);
    if (out_.empty()) {
        confirmed_ = target_;
        known_ = true;
        return out_;
    }
    pending_ = target_;
    inflight_ = out_;
    index_ = out_.back().index;
    sent_at_ = started_ = t;
    bytes_last = 0;
    for (const auto& p : out_) bytes_last += p.msg.len;
    return out_;
}

bool HeapSync::on_ack(uint16_t idx, uint64_t t) {
    idx &= 0x3FF;
    auto it = std::find_if(inflight_.begin(), inflight_.end(), [&](const DataPacket& p) { return p.index == idx; });
    if (it == inflight_.end()) return false;
    inflight_.erase(inflight_.begin(), it + 1);  // cumulative: every packet sent before it is in too
    if (!inflight_.empty()) return false;
    confirmed_ = pending_;
    known_ = true;
    sync_us = static_cast<uint32_t>(std::min<uint64_t>(t - started_, 0xFFFFFFFFu));
    ++frames;
    return true;
}

const std::vector<DataPacket>& HeapSync::due(uint64_t t) {
    out_.clear();
    if (!inflight_.empty() && t - sent_at_ > kResendUs) {
        sent_at_ = t;
        resends += inflight_.size();
        out_ = inflight_;
    }
    return out_;
}

}  // namespace himmel::weapon::blocks
