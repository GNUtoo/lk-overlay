#include <app.h>
#include <assert.h>
#include <lib/gfx.h>
#include <lk/console_cmd.h>
#include <lk/reg.h>
#include <math.h>
#include <platform/bcm28xx/cm.h>
#include <platform/bcm28xx/udelay.h>
#include <platform/interrupts.h>
#include <stdio.h>
#include <stdlib.h>

#define ASB_V3D_S_CTRL 0x7e00a008
#define ASB_V3D_M_CTRL 0x7e00a00c
#define CLR_REQ        0x00000001
#define CLR_ACK        0x00000002

#define PM_GRAFX       0x7e10010c
#define PM_GRAFX_POWUP_SET   0x00000001
#define PM_GRAFX_POWOK_SET   0x00000002
#define PM_GRAFX_ISPOW_SET   0x00000004
#define PM_GRAFX_MEMREP_SET  0x00000008
#define PM_GRAFX_MRDONE_SET  0x00000010
#define PM_GRAFX_ISFUNC_SET  0x00000020
#define PM_GRAFX_V3DRSTN_SET 0x00000040

#define CM_V3DCTL      0x7e101038
#define CM_V3DDIV      0x7e10103c

#define V3D_BASE       0x7ec00000
#define V3D_IDENT0 (V3D_BASE + 0x0000)
#define V3D_INTCTL (V3D_BASE + 0x0030)
#define V3D_INTENA (V3D_BASE + 0x0034)

#define V3D_CT0CS  (V3D_BASE + 0x0100)
#define V3D_CT0EA  (V3D_BASE + 0x0108)
#define V3D_CT0CA  (V3D_BASE + 0x0110)

static int cmd_v3d_probe(int argc, const console_cmd_args *argv);
static int cmd_v3d_probe2(int argc, const console_cmd_args *argv);
static int cmd_v3d_bin(int argc, const console_cmd_args *argv);

STATIC_COMMAND_START
STATIC_COMMAND("v3d_probe", "probe for v3d hw", &cmd_v3d_probe)
STATIC_COMMAND("v3d_probe2", "probe for v3d hw", &cmd_v3d_probe2)
STATIC_COMMAND("v3d_bin", "run the binner job", &cmd_v3d_bin)
STATIC_COMMAND_END(v3d);

static int cmd_v3d_probe(int argc, const console_cmd_args *argv) {
  printf("ASB_V3D_M_CTRL: 0x%x\n", *REG32(ASB_V3D_M_CTRL));
  printf("ASB_V3D_S_CTRL: 0x%x\n", *REG32(ASB_V3D_S_CTRL));
  printf("PM_GRAFX:       0x%x\n", *REG32(PM_GRAFX));
  printf("CM_V3DCTL:      0x%x\n", *REG32(CM_V3DCTL));
  printf("CM_V3DDIV:      0x%x\n", *REG32(CM_V3DDIV));
  return 0;
}

static int cmd_v3d_probe2(int argc, const console_cmd_args *argv) {
  printf("ident is 0x%x\n", *REG32(V3D_IDENT0));
  printf("V3D_CT0CS: 0x%x\n", *REG32(V3D_CT0CS));
  printf("V3D_CT0CA: 0x%x\n", *REG32(V3D_CT0CA));
  printf("V3D_CT0EA: 0x%x\n", *REG32(V3D_CT0EA));
  return 0;
}

typedef struct {
  void *tileAllocation;
  uint32_t tileAllocationSize;
  uint32_t width;
  uint32_t height;
  uint32_t tilewidth;
  uint32_t tileheight;
  void *tileState;
  uint32_t *shaderCode;
  void *uniforms;
  void *vertexData;
  void *shaderRecord;
  uint8_t *primitiveList;
  void *binner;
  uint32_t binnerSize;
  void *renderer;
  uint32_t renderSize;
  gfx_surface *surface;
} v3d_client_state;

v3d_client_state state;

#define DIV_CIEL(x,y) ( ((x)+(y-1)) / y)

uint32_t shaderCode[] = {
  0x958e0dbf, 0xd1724823, /* mov r0, vary; mov r3.8d, 1.0 */
  0x818e7176, 0x40024821, /* fadd r0, r0, r5; mov r1, vary */
  0x818e7376, 0x10024862, /* fadd r1, r1, r5; mov r2, vary */
  0x819e7540, 0x114248a3, /* fadd r2, r2, r5; mov r3.8a, r0 */
  0x809e7009, 0x115049e3, /* nop; mov r3.8b, r1 */
  0x809e7012, 0x116049e3, /* nop; mov r3.8c, r2 */
  0x159e76c0, 0x30020ba7, /* mov tlbc, r3; nop; thrend */
  0x009e7000, 0x100009e7, /* nop; nop; nop */
  0x009e7000, 0x500009e7, /* nop; nop; sbdone */
};

static inline void addbyte(uint8_t **list, uint8_t d) {
  *((*list)++) = d;
}
static inline void addshort(uint8_t **list, uint16_t d) {
  *((*list)++) = (d) & 0xff;
  *((*list)++) = (d >> 8)  & 0xff;
}
static inline void addword(uint8_t **list, uint32_t d) {
  *((*list)++) = (d) & 0xff;
  *((*list)++) = (d >> 8)  & 0xff;
  *((*list)++) = (d >> 16) & 0xff;
  *((*list)++) = (d >> 24) & 0xff;
}
static inline void addfloat(uint8_t **list, float f) {
  uint32_t d = *((uint32_t *)&f);
  *((*list)++) = (d) & 0xff;
  *((*list)++) = (d >> 8)  & 0xff;
  *((*list)++) = (d >> 16) & 0xff;
  *((*list)++) = (d >> 24) & 0xff;
}

void *makeShaderRecord(uint32_t *shader, void *uniforms, void *vertexData) {
  uint8_t *shaderRecord = malloc(0x20);
  uint8_t *p = shaderRecord;
  addbyte(&p, 0x01); // flags
  addbyte(&p, 6*4); // stride
  addbyte(&p, 0xcc); // num uniforms (not used)
  addbyte(&p, 3); // num varyings
  addword(&p, (uint32_t)shader); // Fragment shader code
  addword(&p, (uint32_t)uniforms); // Fragment shader uniforms
  addword(&p, (uint32_t)vertexData); // Vertex Data
  assert((p - shaderRecord) < 0x20);
  return shaderRecord;
}

void makeBinner(v3d_client_state *s) {
  uint8_t *binner = malloc(0x80);
  uint8_t *p = binner;
  // Configuration stuff
  // Tile Binning Configuration.
  //   Tile state data is 48 bytes per tile, I think it can be thrown away
  //   as soon as binning is finished.
  addbyte(&p, 112);
  addword(&p, (uint32_t)s->tileAllocation); // tile allocation memory address
  addword(&p, s->tileAllocationSize); // tile allocation memory size
  addword(&p, (uint32_t)s->tileState); // Tile state data address
  addbyte(&p, s->tilewidth); // 1920/64
  addbyte(&p, s->tileheight); // 1080/64 (16.875)
  addbyte(&p, 0x04); // config, sets tile allocation size to 32bytes per tile
  printf("112 tile binning configuration, tile allocation at 0x%x+0x%x", s->tileAllocation, s->tileAllocationSize);
  printf(", size (in tiles) %dx%x\n", s->tilewidth, s->tileheight);

  // Start tile binning.
  addbyte(&p, 6);

  // Primitive type
  addbyte(&p, 56);
  addbyte(&p, 0x32); // 16 bit triangle

  // Clip Window
  addbyte(&p, 102);
  addshort(&p, 0);
  addshort(&p, 0);
  addshort(&p, s->width); // width
  addshort(&p, s->height); // height
  printf("102, clip window %dx%d\n", s->width, s->height);

  // State
  addbyte(&p, 96);
  addbyte(&p, 0x03); // enable both foward and back facing polygons
  addbyte(&p, 0x00); // depth testing disabled
  addbyte(&p, 0x02); // enable early depth write

  // Viewport offset
  addbyte(&p, 103);
  addshort(&p, 0);
  addshort(&p, 0);

  // The triangle
  // No Vertex Shader state (takes pre-transformed vertexes,
  // so we don't have to supply a working coordinate shader to test the binner.
  addbyte(&p, 65);
  addword(&p, (uint32_t)s->shaderRecord); // Shader Record
  printf("65 NV shader state, 0x%x\n", s->shaderRecord);

  // primitive index list
  addbyte(&p, 32);
  addbyte(&p, 0x04); // 8bit index, triangles
  addword(&p, 3); // Length
  addword(&p, (uint32_t)s->primitiveList); // address
  addword(&p, 2); // Maximum index
  printf("32, indexed primitive list, 0x%x\n", s->primitiveList);

  // End of bin list
  // Flush
  addbyte(&p, 5);
  // Nop
  addbyte(&p, 1);
  // Nop
  addbyte(&p, 1);

  int length = p - binner;
  s->binnerSize = length;
  s->binner = binner;
  assert(length < 0x80);
}

void makeRenderer(uint32_t outputFrame, v3d_client_state *s) {
  uint8_t *render = malloc(0x2000);
  uint8_t *p = render;
  // Render control list

  // Clear color
  addbyte(&p, 114);
  addword(&p, 0x00000000); // Opaque Black
  addword(&p, 0x00000000); // 32 bit clear colours need to be repeated twice
  addword(&p, 0); // clear zs and clear vg mask
  addbyte(&p, 0); // clear stencil

  // Tile Rendering Mode Configuration
  // linear rgba8888 == VC_IMAGE_RGBA32
  // t-format rgba8888 = VC_IMAGE_TF_RGBA32
  addbyte(&p, 113);
  addword(&p, outputFrame);	//  0->31 framebuffer addresss
  addshort(&p, s->width);	// 32->47 width
  addshort(&p, s->height);	// 48->63 height
  addbyte(&p, 0x4);		// 64 multisample mpe
                                  // 65 tilebuffer depth
                                  // 66->67 framebuffer mode (t-format rgba8888)
                                  // 68->69 decimate mode
                                  // 70->71 memory format
  addbyte(&p, 0x00); // vg mask, coverage mode, early-z update, early-z cov, double-buffer

  // Do a store of the first tile to force the tile buffer to be cleared
  // Tile Coordinates
  addbyte(&p, 115);
  addbyte(&p, 0);
  addbyte(&p, 0);

  // Store Tile Buffer General
  addbyte(&p, 28);
  addshort(&p, 0); // Store nothing (just clear)
  addword(&p, 0); // no address is needed

  // Link all binned lists together
  for(uint32_t x = 0; x < s->tilewidth; x++) {
    for(uint32_t y = 0; y < s->tileheight; y++) {
      // Tile Coordinates
      addbyte(&p, 115);
      addbyte(&p, x);
      addbyte(&p, y);

      // Call Tile sublist
      addbyte(&p, 17);
      addword(&p, s->tileAllocation + (y * s->tilewidth + x) * 32); // 2d array of 32byte objects

      // Last tile needs a special store instruction
      if ((x == (s->tilewidth-1)) && (y == (s->tileheight-1))) {
        // Store resolved tile color buffer and signal end of frame
        addbyte(&p, 25);
      } else {
        // Store resolved tile color buffer
        addbyte(&p, 24);
      }
    }
  }
  s->renderSize = p - render;
  s->renderer = render;
  printf("render size %d\n", s->renderSize);
  assert(s->renderSize < 0x2000);
}

static void makeVertexData(uint8_t *vertexvirt,int width,int height, int degrees) {
  //MemoryReference *vertexData = allocator->Allocate(0x60);
  uint8_t *p = vertexvirt;

  double angle = degrees / (180.0/M_PI);

  int w = 200;
  int h = 200;
  int xoff = width/2;
  int yoff = height/2;
  int16_t x = (sin(angle) * w) + xoff;
  int16_t y = (cos(angle) * h) + yoff;
  //printf("%d %d %d\n",x,y,degrees);
  // Vertex: Top, red
  addshort(&p, x << 4); // X in 12.4 fixed point
  addshort(&p, y << 4); // Y in 12.4 fixed point
  addfloat(&p, 1.0f); // Z
  addfloat(&p, 1.0f); // 1/W
  addfloat(&p, 1.0f); // Varying 0 (Red)
  addfloat(&p, 0.0f); // Varying 1 (Green)
  addfloat(&p, 0.0f); // Varying 2 (Blue)

  angle = (degrees+120) / (180.0/M_PI);
  x = (sin(angle) * w) + xoff;
  y = (cos(angle) * h) + yoff;
  // Vertex: bottom left, Green
  addshort(&p, x << 4); // X in 12.4 fixed point
  addshort(&p, y << 4); // Y in 12.4 fixed point
  addfloat(&p, 1.0f); // Z
  addfloat(&p, 1.0f); // 1/W
  addfloat(&p, 0.0f); // Varying 0 (Red)
  addfloat(&p, 1.0f); // Varying 1 (Green)
  addfloat(&p, 0.0f); // Varying 2 (Blue)

  angle = (degrees+120+120) / (180.0/M_PI);
  x = (sin(angle) * w) + xoff;
  y = (cos(angle) * h) + yoff;
  // Vertex: bottom right, Blue
  addshort(&p, x << 4); // X in 12.4 fixed point
  addshort(&p, y << 4); // Y in 12.4 fixed point
  addfloat(&p, 1.0f); // Z
  addfloat(&p, 1.0f); // 1/W
  addfloat(&p, 0.0f); // Varying 0 (Red)
  addfloat(&p, 0.0f); // Varying 1 (Green)
  addfloat(&p, 1.0f); // Varying 2 (Blue)

  assert((p - vertexvirt) < 0x60);
  //return vertexData;
}

static void v3d_allocate(void) {
  state.tileAllocationSize = 0x8000;
  state.tileAllocation = malloc(state.tileAllocationSize);
  state.width = 720;
  state.height = 480;
  state.tilewidth = DIV_CIEL(state.width, 64);
  state.tileheight = DIV_CIEL(state.height, 64);
  state.tileState = malloc(48 * state.tilewidth * state.tileheight);
  state.shaderCode = shaderCode;
  state.uniforms = malloc(0x10);
  state.vertexData = malloc(0x60);
  state.shaderRecord = makeShaderRecord(state.shaderCode, state.uniforms, state.vertexData);
  state.primitiveList = malloc(3);
  state.primitiveList[0] = 0;
  state.primitiveList[1] = 1;
  state.primitiveList[2] = 2;
  makeBinner(&state);
  state.surface = gfx_create_surface(NULL, state.width, state.height, state.width, GFX_FORMAT_ARGB_8888);
  makeRenderer(state.surface->ptr, &state);

  makeVertexData(state.vertexData, state.width, state.height, 0);
}

static int cmd_v3d_bin(int argc, const console_cmd_args *argv) {
  printf("running job that spans 0x%x to 0x%x\n", state.binner, state.binner + state.binnerSize);
  *REG32(V3D_CT0CS) = 0x8000; // reset control thread
  *REG32(V3D_CT0CA) = state.binner;
  *REG32(V3D_CT0EA) = (state.binner + state.binnerSize) - 1;
  printf("V3D_CT0CS: 0x%x\n", *REG32(V3D_CT0CS));
  return 0;
}

enum handler_return v3d_irq(void *arg) {
  uint32_t status = *REG32(V3D_INTCTL);
  *REG32(V3D_INTCTL) = ~0;
  printf("V3D_INTCTL: 0x%x\n", status);
  return INT_NO_RESCHEDULE;
}

static void v3d_init(const struct app_descriptor *app) {
  *REG32(CM_V3DCTL) = CM_PASSWORD;
  *REG32(CM_V3DDIV) = CM_PASSWORD | (1 << 12);
  *REG32(CM_V3DCTL) = CM_PASSWORD | CM_SRC_PLLC_CORE0;
  *REG32(CM_V3DCTL) = CM_PASSWORD | CM_SRC_PLLC_CORE0 | 0x10;

  *REG32(ASB_V3D_M_CTRL) |= CLR_REQ;
  while ((*REG32(ASB_V3D_M_CTRL) & CLR_ACK) == 0) {}
  *REG32(ASB_V3D_S_CTRL) |= CLR_REQ;
  while ((*REG32(ASB_V3D_S_CTRL) & CLR_ACK) == 0) {}

  udelay(100);
  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) & ~0x40); // disable v3d
  udelay(100);
  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) | PM_GRAFX_POWUP_SET); // enable power
  while ((*REG32(PM_GRAFX) & PM_GRAFX_POWOK_SET) == 0) {} // wait for power ok
  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) | PM_GRAFX_ISPOW_SET);
  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) | PM_GRAFX_MEMREP_SET); // do memory repair
  while ((*REG32(PM_GRAFX) & PM_GRAFX_MRDONE_SET) == 0) {} // wait for memory repair to complete
  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) | PM_GRAFX_ISFUNC_SET); // disable functional isolation
  udelay(100);

  *REG32(ASB_V3D_S_CTRL) &= ~CLR_REQ;
  while ((*REG32(ASB_V3D_S_CTRL) & CLR_ACK)) {}
  *REG32(ASB_V3D_M_CTRL) &= ~CLR_REQ;
  while ((*REG32(ASB_V3D_M_CTRL) & CLR_ACK)) {}
  udelay(100);

  *REG32(PM_GRAFX) = CM_PASSWORD | (*REG32(PM_GRAFX) | 0x40); // enable v3d
  udelay(1000);
  cmd_v3d_probe(0, 0);
  v3d_allocate();
  *REG32(V3D_INTENA) = (1<<1) | (1<<0);
  *REG32(V3D_INTCTL) = ~0;
  register_int_handler(10, v3d_irq, NULL);
  unmask_interrupt(10);
}

APP_START(v3d)
  .init = v3d_init,
APP_END