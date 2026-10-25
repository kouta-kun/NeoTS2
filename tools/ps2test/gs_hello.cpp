// Draws a 4-bit paletted texture on two Gouraud triangles with the Z buffer on, to check the display path
// (gsKit, PCSX2's software renderer and a screen grab) before the renderer is built on it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dmaKit.h>
#include <gsKit.h>
#include <gsInline.h>
#include <kernel.h>

int main() {
    GSGLOBAL* gs = gsKit_init_global();
    gs->Mode = GS_MODE_NTSC;
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    // A 64x64 checkerboard of 16 colours: the squares walk through the palette. The same picture as 4-bit, 8-bit and 32-bit.
    static unsigned char texels4[64 * 64 / 2], texels8[64 * 64];
    static unsigned int palette[256], pixels32[64 * 64];
    for (int i = 0; i < 256; ++i) {
        const unsigned r = 40 + (i & 15) * 13, g = 255 - (i & 15) * 14, b = (i & 1) ? 220 : 60;
        palette[i] = r | (g << 8) | (b << 16) | (0x80u << 24);  // the GS reads alpha 0x80 as 1.0
    }
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int index = ((x / 8) + (y / 8) * 3) & 15;
            unsigned char& byte = texels4[(y * 64 + x) / 2];
            byte = static_cast<unsigned char>((x & 1) ? (byte | (index << 4)) : index);
            texels8[y * 64 + x] = static_cast<unsigned char>(index);
            pixels32[y * 64 + x] = palette[index];
        }
    }
    // The palette of an 8-bit texture is stored with entries 8-15 and 16-23 of each 32 swapped (CSM1).
    static unsigned int palette8[256];
    for (int i = 0; i < 256; ++i) {
        const int block = (i >> 3) & 3;
        const int j = block == 1 ? i + 8 : block == 2 ? i - 8 : i;
        palette8[j] = palette[i];
    }
    GSTEXTURE tex[3];
    std::memset(tex, 0, sizeof tex);
    const int psms[3] = {GS_PSM_T4, GS_PSM_T8, GS_PSM_CT32};
    for (int k = 0; k < 3; ++k) {
        tex[k].Width = 64;
        tex[k].Height = 64;
        tex[k].PSM = psms[k];
        tex[k].Mem = reinterpret_cast<u32*>(k == 0 ? static_cast<void*>(texels4) : k == 1 ? static_cast<void*>(texels8) : static_cast<void*>(pixels32));
        tex[k].Clut = k == 0 ? palette : k == 1 ? palette8 : nullptr;
        tex[k].ClutPSM = GS_PSM_CT32;
        tex[k].Filter = GS_FILTER_NEAREST;
        tex[k].Vram = gsKit_vram_alloc(gs, gsKit_texture_size(tex[k].Width, tex[k].Height, tex[k].PSM), GSKIT_ALLOC_USERBUFFER);
        if (tex[k].Clut) {
            tex[k].VramClut = gsKit_vram_alloc(gs, gsKit_texture_size(k == 0 ? 8 : 16, k == 0 ? 2 : 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
        }
        gsKit_texture_upload(gs, &tex[k]);
    }
    std::printf("gs_hello: textures uploaded\n");

    for (int frame = 0; frame < 600; ++frame) {
        gsKit_clear(gs, GS_SETREG_RGBAQ(0x20, 0x28, 0x40, 0x80, 0x00));
        for (int k = 0; k < 3; ++k) {
            const float cx = 110 + k * 210, cy = 200, s = 90;
            GSPRIMUVPOINT v[6];
            const float xs[4] = {cx - s, cx + s, cx + s, cx - s}, ys[4] = {cy - s, cy - s, cy + s, cy + s};
            const float us[4] = {0, 64, 64, 0}, vs[4] = {0, 0, 64, 64};
            const int idx[6] = {0, 1, 2, 0, 2, 3};
            for (int n = 0; n < 6; ++n) {
                const int i = idx[n];
                v[n].rgbaq = color_to_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
                v[n].uv.coord.u = static_cast<u16>(us[i] * 16);
                v[n].uv.coord.v = static_cast<u16>(vs[i] * 16);
                v[n].uv.tag = GS_UV;
                v[n].xyz2 = vertex_to_XYZ2(gs, xs[i], ys[i], 0x4000);
            }
            gsKit_prim_list_triangle_goraud_texture_uv_3d(gs, &tex[k], 6, v);
        }
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    std::printf("gs_hello: done\n");
    SleepThread();
}
