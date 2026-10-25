// Draws a textured grid with the VU1 and the GS (state through gsKit, vertices through a VIF1 chain and a microprogram): a test of
// the path the console renderer will use for triangle soups (see vu1_soup.vsm).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dma.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>
#include <malloc.h>
#include <packet2.h>
#include <packet2_utils.h>

#include "render/math.hpp"

extern u32 VU1Soup_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Soup_CodeEnd __attribute__((section(".vudata")));

namespace {

using ts2::render::Mat4;

constexpr unsigned kHeaderQw = 8;       // state packet (7 qw) and the vertices' GIF tag
constexpr unsigned kBatch = 156;        // corners a batch: 3 qw each and the header must fit half of the VU memory's buffers
constexpr unsigned kBufferQw = 476;     // kHeaderQw + 3 * kBatch

struct alignas(16) Qw {
    u64 lo, hi;
};

struct alignas(16) Tag {  // a DMA tag and the two VIF codes that go with it
    u64 tag;
    u32 vif0, vif1;
};

u64 giftag(u64 nloop, u64 eop, u64 pre, u64 prim, u64 flg, u64 nreg) {
    return nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60);
}

u32 vif(u32 imm, u32 num, u32 cmd) {
    return imm | (num << 16) | (cmd << 24);
}

enum : u32 { kNop = 0x00, kStcycl = 0x01, kMscal = 0x14, kUnpackV2_16 = 0x65, kUnpackV4_32 = 0x6C, kUnpackV4_16 = 0x6D, kUnpackV4_8 = 0x6E, kFlusha = 0x13 };

u32 unpackAddress(u32 address, bool usn, bool tops) {
    return address | (usn ? 1u << 14 : 0) | (tops ? 1u << 15 : 0);
}

u64 ref(const void* data, unsigned qwc) {
    return qwc | (3ull << 28) | (static_cast<u64>(reinterpret_cast<u32>(data) & 0x0FFFFFFF) << 32);
}

u64 cnt() { return 1ull << 28; }
u64 end() { return 7ull << 28; }

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    GSGLOBAL* gs = gsKit_init_global();
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_24;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_ON);
    std::printf("vu1_soup: screen ready\n");
    dma_channel_initialize(DMA_CHANNEL_VIF1, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);
    std::printf("vu1_soup: vif1 ready\n");

    // The microprogram and the double buffer (two buffers of kBufferQw from address 16).
    {
        const u32 size = packet2_utils_get_packet_size_for_program(&VU1Soup_CodeStart, &VU1Soup_CodeEnd) + 1;
        packet2_t* p = packet2_create(size, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
        packet2_vif_add_micro_program(p, 0, &VU1Soup_CodeStart, &VU1Soup_CodeEnd);
        packet2_utils_vu_add_end_tag(p);
        std::printf("vu1_soup: program packet %u qw\n", size);
        dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
        std::printf("vu1_soup: program uploaded\n");
        packet2_free(p);
        p = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
        packet2_utils_vu_add_double_buffer(p, 16, kBufferQw);
        packet2_utils_vu_add_end_tag(p);
        dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
        packet2_free(p);
    }

    std::printf("vu1_soup: double buffer set\n");
    // A 64x64 texture of 16 colours.
    static unsigned char texels[64 * 64 / 2];
    static unsigned int palette[16];
    for (int i = 0; i < 16; ++i) {
        palette[i] = (40 + i * 13) | ((255 - i * 14) << 8) | (((i & 1) ? 220 : 60) << 16) | (0x80u << 24);
    }
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int index = ((x / 8) + (y / 8) * 3) & 15;
            unsigned char& byte = texels[(y * 64 + x) / 2];
            byte = static_cast<unsigned char>((x & 1) ? (byte | (index << 4)) : index);
        }
    }
    GSTEXTURE tex;
    std::memset(&tex, 0, sizeof tex);
    tex.Width = 64;
    tex.Height = 64;
    tex.PSM = GS_PSM_T4;
    tex.Mem = reinterpret_cast<u32*>(texels);
    tex.Clut = palette;
    tex.ClutPSM = GS_PSM_CT32;
    tex.Filter = GS_FILTER_NEAREST;
    tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(64, 64, GS_PSM_T4), GSKIT_ALLOC_USERBUFFER);
    tex.VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(8, 2, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
    gsKit_setup_tbw(&tex);
    gsKit_texture_upload(gs, &tex);

    // The soup: a grid of 20 x 20 cells, two triangles a cell, positions as 16-bit values over the grid's box.
    constexpr int kCells = 20;
    std::vector<float> world;     // x, y, z of each corner
    std::vector<float> uv;        // u, v
    std::vector<unsigned char> colour;
    const auto height = [](float x, float y) { return 1.2f * std::sin(x * 0.5f) * std::cos(y * 0.5f); };
    for (int cy = 0; cy < kCells; ++cy) {
        for (int cx = 0; cx < kCells; ++cx) {
            const int idx[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
            for (const auto& c : idx) {
                const float x = static_cast<float>(cx + c[0]), y = static_cast<float>(cy + c[1]);
                world.insert(world.end(), {x, y, height(x, y)});
                uv.insert(uv.end(), {x * 0.25f, y * 0.25f});
                colour.insert(colour.end(), {static_cast<unsigned char>(0x60 + (cx * 8) % 0x40), static_cast<unsigned char>(0x60 + (cy * 8) % 0x40), 0x70, 0x40});
            }
        }
    }
    unsigned corners = static_cast<unsigned>(world.size() / 3);
    while (corners % 12 != 0) {  // pad with degenerate triangles: arrays must fill whole quadwords
        world.insert(world.end(), world.end() - 3, world.end());
        uv.insert(uv.end(), uv.end() - 2, uv.end());
        colour.insert(colour.end(), colour.end() - 4, colour.end());
        ++corners;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (unsigned i = 0; i < corners; ++i) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], world[i * 3 + static_cast<unsigned>(k)]);
            hi[k] = std::max(hi[k], world[i * 3 + static_cast<unsigned>(k)]);
        }
    }
    float uvLo = 0, uvHi = 0;
    for (const float f : uv) {
        uvHi = std::max(uvHi, f);
    }
    auto* positions = static_cast<u16*>(memalign(64, corners * 8));
    auto* uvs = static_cast<u16*>(memalign(64, corners * 4));
    auto* colours = static_cast<u8*>(memalign(64, corners * 4));
    for (unsigned i = 0; i < corners; ++i) {
        for (int k = 0; k < 3; ++k) {
            const float range = hi[k] - lo[k];
            positions[i * 4 + static_cast<unsigned>(k)] = static_cast<u16>(range > 0 ? std::lround((world[i * 3 + static_cast<unsigned>(k)] - lo[k]) / range * 65535.0f) : 0);
        }
        positions[i * 4 + 3] = 0;
        uvs[i * 2] = static_cast<u16>(std::lround((uv[i * 2] - uvLo) / (uvHi - uvLo) * 65535.0f));
        uvs[i * 2 + 1] = static_cast<u16>(std::lround((uv[i * 2 + 1] - uvLo) / (uvHi - uvLo) * 65535.0f));
        std::memcpy(&colours[i * 4], &colour[i * 4], 4);
    }

    // The state packet and the vertices' GIF tags.
    auto* header = static_cast<Qw*>(memalign(64, 8 * sizeof(Qw)));
    auto* tags = static_cast<Qw*>(memalign(64, 2 * sizeof(Qw)));  // a full batch's, and the last batch's
    const u64 prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, 0, 0);
    const unsigned fullBatches = corners / kBatch, remainder = corners % kBatch;
    tags[0] = {giftag(kBatch, 1, 1, prim, 0, 3), 0x512};
    tags[1] = {giftag(remainder, 1, 1, prim, 0, 3), 0x512};
    header[0] = {giftag(6, 0, 0, 0, 0, 1), 0xE};
    header[1] = {GS_SETREG_TEX0(tex.Vram / 256, tex.TBW, tex.PSM, 6, 6, 1, 0, tex.VramClut / 256, tex.ClutPSM, 0, 0, GS_CLUT_STOREMODE_LOAD), GS_TEX0_1};
    header[2] = {GS_SETREG_TEX1(1, 0, 0, 0, 0, 0, 0), GS_TEX1_1};
    header[3] = {GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0), GS_CLAMP_1};
    header[4] = {GS_SETREG_ALPHA(0, 1, 0, 1, 0), GS_ALPHA_1};
    header[5] = {GS_SETREG_TEST(0, 1, 0, 0, 0, 0, 1, 2), GS_TEST_1};
    header[6] = {GS_SETREG_ZBUF_1(gs->ZBuffer / 8192, gs->PSMZ, 0), GS_ZBUF_1};
    header[7] = {0, 0};  // replaced by a batch's own tag

    auto* constants = static_cast<float*>(memalign(64, 9 * 16));
    auto* chain = static_cast<Tag*>(memalign(64, (2 + (fullBatches + 1) * 6) * sizeof(Tag)));

    std::printf("vu1_soup: %u corners (%u triangles), %u full batches and %u left\n", corners, corners / 3, fullBatches, remainder);
    const float aspect = 640.0f / 448.0f;
    for (int frame = 0; frame < 600; ++frame) {
        const float angle = 0.6f + frame * 0.01f;
        const ts2::render::Vec3 eye{10.0f + 22.0f * std::cos(angle), 10.0f + 22.0f * std::sin(angle), 14.0f}, target{10, 10, 0};
        const Mat4 mvp = Mat4::perspective(0.8f, aspect, 0.5f, 200.0f) * Mat4::lookAt(eye, target, {0, 0, 1});
        const auto& r = mvp.m;
        for (int k = 0; k < 3; ++k) {
            const float scale = (hi[k] - lo[k]) / 65535.0f;
            for (int row = 0; row < 4; ++row) {
                constants[k * 4 + row] = r[static_cast<unsigned>(row * 4 + k)] * scale;
            }
        }
        for (int row = 0; row < 4; ++row) {
            constants[12 + row] = r[static_cast<unsigned>(row * 4)] * lo[0] + r[static_cast<unsigned>(row * 4 + 1)] * lo[1] + r[static_cast<unsigned>(row * 4 + 2)] * lo[2] +
                                  r[static_cast<unsigned>(row * 4 + 3)];
        }
        const float screen[4] = {320.0f, -224.0f, 16777215.0f, 0}, offset[4] = {2048.0f + 320.0f, 2048.0f + 224.0f, 0, 0};
        const float uvScale[4] = {(uvHi - uvLo) / 65535.0f, (uvHi - uvLo) / 65535.0f, 0, 0}, uvOffset[4] = {uvLo, uvLo, 0, 0}, guard[4] = {1.0f / 3, 1.0f / 3, 1, 1};
        std::memcpy(&constants[16], screen, 16);
        std::memcpy(&constants[20], offset, 16);
        std::memcpy(&constants[24], uvScale, 16);
        std::memcpy(&constants[28], uvOffset, 16);
        std::memcpy(&constants[32], guard, 16);

        // The chain: the constants, then for each batch its header, tag and three arrays, and the microprogram's start.
        unsigned t = 0;
        chain[t++] = {ref(constants, 9), vif(0x0101, 0, kStcycl), vif(unpackAddress(0, false, false), 9, kUnpackV4_32)};
        for (unsigned b = 0; b <= fullBatches; ++b) {
            const unsigned n = b < fullBatches ? kBatch : remainder;
            if (n == 0) {
                break;
            }
            const unsigned first = b * kBatch;
            chain[t++] = {ref(header, 7), vif(0x0101, 0, kStcycl), vif(unpackAddress(0, false, true), 7, kUnpackV4_32)};
            chain[t++] = {ref(&tags[b < fullBatches ? 0 : 1], 1), vif(0, 0, kNop), vif(unpackAddress(7, false, true), 1, kUnpackV4_32)};
            chain[t++] = {ref(&positions[first * 4], n / 2), vif(kBatch >= 1 ? (3 | (1 << 8)) : 0, 0, kStcycl), vif(unpackAddress(kHeaderQw + 2, true, true), n, kUnpackV4_16)};
            chain[t++] = {ref(&uvs[first * 2], n / 4), vif(0, 0, kNop), vif(unpackAddress(kHeaderQw + 0, true, true), n, kUnpackV2_16)};
            chain[t++] = {ref(&colours[first * 4], n / 4), vif(0, 0, kNop), vif(unpackAddress(kHeaderQw + 1, true, true), n, kUnpackV4_8)};
            chain[t++] = {cnt(), vif(0, 0, kNop), vif(0, 0, kMscal)};
        }
        chain[t++] = {end(), vif(0, 0, kFlusha), vif(0, 0, kNop)};

        gsKit_clear(gs, GS_SETREG_RGBAQ(0x20, 0x28, 0x40, 0x80, 0x00));
        gsKit_queue_exec(gs);
        FlushCache(0);
        dma_channel_send_chain(DMA_CHANNEL_VIF1, chain, 0, DMA_FLAG_TRANSFERTAG, 0);
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
        gsKit_sync_flip(gs);
        if (frame < 2) {
            std::printf("vu1_soup: frame %d sent, %u tags\n", frame, t);
        }
    }
    std::printf("vu1_soup: done\n");
    SleepThread();
}
