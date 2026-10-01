#pragma once
/* graphics.library and the display hardware, as far as the game uses them (port/amiga/spec/platform_video.md §5,
 * title_select.md §5).
 *
 * The game's own structures stay in amem[] with the AmigaOS 1.3 layouts (View, ViewPort, RasInfo, BitMap,
 * ColorMap, UCopList, RastPort), at their D:xxxx addresses or in AllocMem blocks, and the game code reads and
 * writes their fields directly, as the original does. The functions below are the library calls. Instead of a
 * real copper list, MrgCop takes a snapshot of the View's ViewPort chain (offsets, sizes, modes, bitmap, user
 * copper list) and LoadView selects the snapshot shown from the next vertical blank. Colours come live from the
 * ColorMaps (LoadRGB4 shows at once, as it does on the real machine) and the pixels live from the planes. */
#include "amem.h"

/* struct View */
#define VIEW_ViewPort      0x00
#define VIEW_LOFCprList    0x04
#define VIEW_SHFCprList    0x08
#define VIEW_DyOffset      0x0C
#define VIEW_DxOffset      0x0E
#define VIEW_Modes         0x10
#define VIEW_SIZE          0x12
/* struct ViewPort */
#define VP_Next            0x00
#define VP_ColorMap        0x04
#define VP_DspIns          0x08
#define VP_SprIns          0x0C
#define VP_ClrIns          0x10
#define VP_UCopIns         0x14
#define VP_DWidth          0x18
#define VP_DHeight         0x1A
#define VP_DxOffset        0x1C
#define VP_DyOffset        0x1E
#define VP_Modes           0x20
#define VP_RasInfo         0x24
#define VP_SIZE            0x28
/* struct RasInfo */
#define RI_Next            0x00
#define RI_BitMap          0x04
#define RI_RxOffset        0x08
#define RI_RyOffset        0x0A
#define RI_SIZE            0x0C
/* struct BitMap */
#define BM_BytesPerRow     0x00
#define BM_Rows            0x02
#define BM_Flags           0x04
#define BM_Depth           0x05
#define BM_Planes          0x08
#define BM_SIZE            0x28
/* struct ColorMap (1.3) */
#define CM_Flags           0x00
#define CM_Type            0x01
#define CM_Count           0x02
#define CM_ColorTable      0x04
#define CM_SIZE            0x08
/* struct UCopList: Next, FirstCopList, CopList. The port keeps its own instruction buffer in FirstCopList. */
#define UCL_Next           0x00
#define UCL_FirstCopList   0x04
#define UCL_CopList        0x08
#define UCL_SIZE           0x0C
/* struct RastPort */
#define RP_BitMap          0x04
#define RP_Mask            0x18
#define RP_FgPen           0x19
#define RP_BgPen           0x1A
#define RP_AOlPen          0x1B
#define RP_DrawMode        0x1C
#define RP_LinePtrn        0x22
#define RP_cp_x            0x24
#define RP_cp_y            0x26
#define RP_Font            0x34
#define RP_SIZE            0x64

#define V_HIRES            0x8000

/* Custom-chip registers the user copper lists move to (offsets from $DFF000). */
#define CUSTOM_COLOR00     0x180
#define CUSTOM_SPR0PT      0x120
#define CUSTOM_SPR0POS     0x140
#define CUSTOM_DMACON      0x096

/* The composed picture: 640 x 200, lowres pixels doubled. */
#define GFX_FRAME_W 640
#define GFX_FRAME_H 200

void gfx_InitView(APTR view);
void gfx_InitVPort(APTR vp);
void gfx_InitBitMap(APTR bm, u8 depth, u16 width, u16 height);
void gfx_InitRastPort(APTR rp);
APTR gfx_GetColorMap(u16 entries);
void gfx_FreeColorMap(APTR cm);
void gfx_LoadRGB4(APTR vp, APTR colors, u16 count);
void gfx_SetRGB4(APTR vp, u16 n, u8 r, u8 g, u8 b);
u16  gfx_GetRGB4(APTR cm, u16 n);
void gfx_MakeVPort(APTR view, APTR vp);
void gfx_MrgCop(APTR view);
void gfx_LoadView(APTR view);           /* shown from the next vertical blank */
APTR gfx_ActiView(void);                /* GfxBase->ActiView */
void gfx_FreeVPortCopLists(APTR vp);    /* frees the ViewPort's user copper list (UCopIns) */
void gfx_FreeCprList(APTR cprlist);
void gfx_WaitTOF(void);

/* User copper lists (CINIT is implicit: the first CWait/CMove on a cleared UCopList starts it). Wait lines are
 * relative to the ViewPort's top, as in graphics.library. */
void gfx_CWait(APTR ucl, s16 v, s16 h);
void gfx_CMove(APTR ucl, u32 reg, u16 value);   /* reg: $DFFxxx or the offset xxx */
void gfx_CBump(APTR ucl);

/* Hardware sprites. The drive sets the eight SPRxPT pointers from its user copper list and turns sprite DMA on
 * by writing DMACON directly (drive_scene.md §4.8, §5); the composition then reads each sprite's segment chain
 * out of amem[] and draws the sprites over the ViewPorts, in the colours (16..31) of the line's palette.
 * gfx_dmacon takes the value the game writes to $DFF096 (bit 15 set/clear, bit 5 = SPREN). */
void gfx_dmacon(u16 value);

/* Blitter-backed library calls, done by the CPU. */
void gfx_BltClear(APTR mem, u32 bytecount, u32 flags);
void gfx_BltBitMap(APTR src, s16 sx, s16 sy, APTR dst, s16 dx, s16 dy, s16 w, s16 h, u8 minterm, u8 mask);

/* Installs the display as the host's frame source. */
void gfx_display_init(void);
