// JVS I/O board emulation (single node): 2 players x 13 buttons, 2 coin slots,
// 8 x 16-bit analog channels (positional guns), 8 general purpose outputs.
#include "common.h"
#include <vector>
#include <mutex>
#include <algorithm>

static std::mutex s_lock;
static bool s_addressed = false;
static uint8_t s_address = 0;
static uint16_t s_coins[2] = {0, 0};
static uint8_t s_gpo[4] = {0, 0, 0, 0};
static std::vector<uint8_t> s_lastReply;
static int s_logged = 0;
static InputSnapshot s_in;

bool JvsAddressed() { return s_addressed; }
const uint8_t* JvsOutputs() { return s_gpo; }

static const char kIdent[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

static void Unescape(const uint8_t* in, size_t n, std::vector<uint8_t>& out, bool& framed) {
    out.clear();
    size_t i = 0;
    framed = n > 0 && in[0] == 0xE0;
    if (framed) i = 1;
    for (; i < n; i++) {
        if (framed && in[i] == 0xD0 && i + 1 < n) out.push_back(in[++i] + 1);
        else out.push_back(in[i]);
    }
}

static size_t Frame(const std::vector<uint8_t>& body, bool framed, uint8_t* out, size_t cap) {
    // body = node, len, status, reports...  (sum appended here)
    std::vector<uint8_t> pkt = body;
    uint8_t sum = 0;
    for (uint8_t b : pkt) sum += b;
    pkt.push_back(sum);
    size_t o = 0;
    if (framed) {
        if (o < cap) out[o++] = 0xE0;
        for (uint8_t b : pkt) {
            if (b == 0xE0 || b == 0xD0) {
                if (o + 1 < cap) { out[o++] = 0xD0; out[o++] = b - 1; }
            } else if (o < cap) out[o++] = b;
        }
    } else {
        for (uint8_t b : pkt) if (o < cap) out[o++] = b;
    }
    return o;
}

static void PutSwitches(std::vector<uint8_t>& r, int players, int bytes) {
    r.push_back(s_in.test ? 0x80 : 0x00);
    for (int p = 0; p < players; p++) {
        uint16_t bits = 0; // bit15 start, 14 service, 13 up, 12 down, 11 left, 10 right, 9 b1, 8 b2, 7 b3 ... 0 b10
        if (p < 2) {
            const PlayerState& ps = s_in.p[p];
            if (ps.start) bits |= 0x8000;
            if (ps.service) bits |= 0x4000;
            auto button = [&](int n, bool on) {
                if (on && n >= 1 && n <= 10) bits |= (uint16_t)(0x0200 >> (n - 1));
            };
            button(g_cfg.triggerButton, ps.trigger);
        }
        for (int b = 0; b < bytes; b++) r.push_back(b == 0 ? (bits >> 8) : b == 1 ? (bits & 0xFF) : 0);
    }
}

static uint16_t Axis(float v, bool invert) {
    v = std::min(1.0f, std::max(0.0f, v));
    if (invert) v = 1.0f - v;
    return (uint16_t)(v * 65535.0f + 0.5f);
}

static void PutAnalog(std::vector<uint8_t>& r, int channels) {
    uint16_t ch[16] = {0};
    for (int i = 0; i < 16; i++) ch[i] = 0x8000;
    auto set = [&](int idx, uint16_t v) { if (idx >= 0 && idx < 16) ch[idx] = v; };
    set(g_cfg.anP1X, Axis(s_in.p[0].x, g_cfg.invertX));
    set(g_cfg.anP1Y, Axis(s_in.p[0].y, g_cfg.invertY));
    set(g_cfg.anP2X, Axis(s_in.p[1].x, g_cfg.invertX));
    set(g_cfg.anP2Y, Axis(s_in.p[1].y, g_cfg.invertY));
    for (int i = 0; i < channels; i++) {
        uint16_t v = i < 16 ? ch[i] : 0;
        r.push_back(v >> 8);
        r.push_back(v & 0xFF);
    }
}

size_t JvsProcess(const uint8_t* req, size_t reqLen, uint8_t* out, size_t outCap) {
    std::lock_guard<std::mutex> l(s_lock);
    std::vector<uint8_t> p;
    bool framed;
    Unescape(req, reqLen, p, framed);
    bool log = s_logged < g_cfg.jvsLogPackets;
    if (log) LogHex("jvs <-", req, reqLen);
    if (p.size() < 3) return 0;

    uint8_t node = p[0];
    size_t len = p[1];
    size_t end = std::min(p.size(), (size_t)2 + len) - 1; // exclude checksum
    size_t i = 2;
    bool broadcast = node == 0xFF;
    if (!broadcast && (!s_addressed || node != s_address)) return 0;

    InputPoll(s_in);
    for (int k = 0; k < 2; k++) {
        if (s_in.p[k].coinEdge && s_coins[k] < 0x3FFF) s_coins[k]++;
    }

    std::vector<uint8_t> rep; // reports
    bool reply = true;
    while (i < end) {
        uint8_t cmd = p[i];
        switch (cmd) {
        case 0xF0: // reset (broadcast, no reply)
            s_addressed = false;
            s_address = 0;
            reply = false;
            i = end;
            break;
        case 0xF1: // set address
            if (i + 1 < end) {
                s_address = p[i + 1];
                s_addressed = true;
                Log("jvs: assigned address %u", s_address);
            }
            rep.push_back(0x01);
            i += 2;
            break;
        case 0xF2: // change comm method
            i += 2;
            rep.push_back(0x01);
            break;
        case 0x10:
            rep.push_back(0x01);
            rep.insert(rep.end(), kIdent, kIdent + sizeof(kIdent)); // includes terminating zero
            i += 1;
            break;
        case 0x11: rep.push_back(0x01); rep.push_back(0x13); i += 1; break;
        case 0x12: rep.push_back(0x01); rep.push_back(0x30); i += 1; break;
        case 0x13: rep.push_back(0x01); rep.push_back(0x10); i += 1; break;
        case 0x14: {
            static const uint8_t feat[] = {0x01, 0x02, 0x0D, 0x00,  // switches: 2 players, 13 buttons
                                           0x02, 0x02, 0x00, 0x00,  // coins: 2 slots
                                           0x03, 0x08, 0x10, 0x00,  // analog: 8 ch, 16 bit
                                           0x12, 0x08, 0x00, 0x00,  // general purpose outputs: 8
                                           0x00};
            rep.push_back(0x01);
            rep.insert(rep.end(), feat, feat + sizeof(feat));
            i += 1;
            break;
        }
        case 0x15: // main board id string
            rep.push_back(0x01);
            i++;
            while (i < end && p[i]) i++;
            i++;
            break;
        case 0x20: {
            int players = i + 1 < end ? p[i + 1] : 0, bytes = i + 2 < end ? p[i + 2] : 0;
            rep.push_back(0x01);
            PutSwitches(rep, players, bytes);
            i += 3;
            break;
        }
        case 0x21: {
            int slots = i + 1 < end ? p[i + 1] : 0;
            rep.push_back(0x01);
            for (int s = 0; s < slots; s++) {
                uint16_t c = s < 2 ? s_coins[s] : 0;
                rep.push_back((c >> 8) & 0x3F);
                rep.push_back(c & 0xFF);
            }
            i += 2;
            break;
        }
        case 0x22: {
            int ch = i + 1 < end ? p[i + 1] : 0;
            rep.push_back(0x01);
            PutAnalog(rep, ch);
            i += 2;
            break;
        }
        case 0x23: { int ch = p[i + 1]; rep.push_back(0x01); for (int k = 0; k < ch * 2; k++) rep.push_back(0); i += 2; break; }
        case 0x24: rep.push_back(0x01); rep.push_back(0x00); i += 1; break;
        case 0x25: rep.push_back(0x01); for (int k = 0; k < 4; k++) rep.push_back(0); i += 2; break;
        case 0x26: { int n = p[i + 1]; rep.push_back(0x01); for (int k = 0; k < n; k++) rep.push_back(0); i += 2; break; }
        case 0x2E: rep.push_back(0x01); for (int k = 0; k < 4; k++) rep.push_back(0); i += 2; break;
        case 0x2F: { // retransmit last reply verbatim
            size_t n = std::min(s_lastReply.size(), outCap);
            memcpy(out, s_lastReply.data(), n);
            return n;
        }
        case 0x30: { // decrease coins
            if (i + 3 < end) {
                int s = p[i + 1] - 1;
                uint16_t amt = (p[i + 2] << 8) | p[i + 3];
                if (s >= 0 && s < 2) s_coins[s] = s_coins[s] > amt ? s_coins[s] - amt : 0;
            }
            rep.push_back(0x01);
            i += 4;
            break;
        }
        case 0x35: { // increase coins
            if (i + 3 < end) {
                int s = p[i + 1] - 1;
                uint16_t amt = (p[i + 2] << 8) | p[i + 3];
                if (s >= 0 && s < 2) s_coins[s] = std::min<int>(0x3FFF, s_coins[s] + amt);
            }
            rep.push_back(0x01);
            i += 4;
            break;
        }
        case 0x31: case 0x36: rep.push_back(0x01); i += 4; break;
        case 0x32: { // general purpose output 1
            int n = i + 1 < end ? p[i + 1] : 0;
            uint8_t prev[4];
            memcpy(prev, s_gpo, 4);
            for (int k = 0; k < n && k < 4 && i + 2 + k < end; k++) s_gpo[k] = p[i + 2 + k];
            if (memcmp(prev, s_gpo, 4) != 0 && s_logged < 400) {
                s_logged++;
                Log("jvs: outputs %02X %02X", s_gpo[0], s_gpo[1]);
            }
            rep.push_back(0x01);
            i += 2 + n;
            break;
        }
        case 0x33: { int n = p[i + 1]; rep.push_back(0x01); i += 2 + 2 * n; break; }
        case 0x34: { int n = p[i + 1]; rep.push_back(0x01); i += 2 + n; break; }
        case 0x37: case 0x38: rep.push_back(0x01); i += 3; break;
        default:
            Log("jvs: unknown command %02X", cmd);
            rep.push_back(0x02); // unsupported
            i = end;
            break;
        }
    }
    if (!reply) {
        if (log) { s_logged++; Log("jvs -> (no reply)"); }
        return 0;
    }
    std::vector<uint8_t> body;
    body.push_back(0x00);                       // to master
    body.push_back((uint8_t)(rep.size() + 2));  // status + reports + sum
    body.push_back(0x01);                       // status: normal
    body.insert(body.end(), rep.begin(), rep.end());
    size_t n = Frame(body, framed, out, outCap);
    s_lastReply.assign(out, out + n);
    if (log) {
        s_logged++;
        LogHex("jvs ->", out, n);
    }
    return n;
}
