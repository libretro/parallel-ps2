/* d3dswdraw: tests/swdraw's GS draw cases on the D3D11 and D3D12
 * renderers, driven through the core's libretro exports by a frontend
 * that speaks libretro_d3d11.h / libretro_d3d12.h, version 1 or 2.
 *
 * The BIOS image is one tests/swdraw writes for a case (swdraw --bios):
 * its reset program sends the case's packet down the GIF and then idles
 * with the display on. The frames run, and the last image the core
 * handed over is read back and written out as a PPM, top row first, for
 * the picture of one renderer to be compared with another's. The
 * software renderer runs through the same frontend (renderer SW), so the
 * reference comes out of the same harness.
 *
 *   d3dswdraw <core> <scratch-dir> <bios-image> <D3D11|D3D12|SW> <out.ppm> [v1|v2] [scale]
 *   d3dswdraw --compare <a.ppm> <b.ppm> <tolerance> <max-bad-per-mille>
 *
 * --compare fails when the two pictures differ in size, when the first
 * is blank while the second is not, or when more than max-bad-per-mille
 * of the pixels differ in some channel by more than the tolerance.
 *
 * D3DSW_FRAMES sets the frames run (30); D3DSW_SYNC_SLOTS the frontend's
 * sync slots on version 2 (1 to 3; 3 rotates the present textures);
 * D3DSW_REINIT_AT a frame after which the frontend rebuilds its video
 * driver, as RetroArch does on SET_SYSTEM_AV_INFO: context destroyed, a
 * new device, context reset; D3DSW_DEINTERLACE the deinterlace mode
 * (Automatic). D3DSW_LATENCY runs tests/latency's case instead (see
 * ds_latency), on the image latency --bios writes. The D3D12 texture is
 * to be left in COPY_SOURCE, as RetroArch asks, or with D3DSW_SRV_STATE
 * in PIXEL_SHADER_RESOURCE.
 *
 * D3DSW_CONTENT loads real content, a disc image's path, with the BIOS
 * image a real one (its .nvm and .mec beside it are taken too), and reads
 * every frame (see ds_content_frame): D3DSW_PAD scripts the pad,
 * D3DSW_HASH_LOG reports each picture's hash, D3DSW_DUMP and
 * D3DSW_DUMP_DIR write chosen pictures out, D3DSW_LOAD_STATE (after
 * D3DSW_LOAD_AFTER retro_runs) and D3DSW_SAVE_STATE bring a state in and
 * take one out, and D3DSW_OPTIONS sets core options, "key=value;...".
 *
 * The frontend is Windows only (MSYS2 MINGW64, or wine with DXVK and
 * vkd3d-proton); --compare builds anywhere. C89. */

#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <libretro.h>
#include <libretro_d3d11.h>
#include <libretro_d3d12.h>
#endif

/* ---- the picture ---- */

typedef struct ds_image
{
   unsigned char *rgb;   /* w * h * 3, top row first */
   unsigned       w, h;
} ds_image_t;

static int ds_write_ppm(const char *path, const ds_image_t *img)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return 0;
   fprintf(f, "P6\n%u %u\n255\n", img->w, img->h);
   fwrite(img->rgb, 1, (size_t)img->w * img->h * 3, f);
   return fclose(f) == 0;
}

static int ds_read_ppm(const char *path, ds_image_t *img)
{
   FILE    *f = fopen(path, "rb");
   unsigned max;
   size_t   n;
   img->rgb = NULL;
   if (!f)
      return 0;
   if (fscanf(f, "P6 %u %u %u", &img->w, &img->h, &max) != 3 || max != 255)
   {
      fclose(f);
      return 0;
   }
   fgetc(f);
   n        = (size_t)img->w * img->h * 3;
   img->rgb = (unsigned char*)malloc(n ? n : 1);
   if (!img->rgb || fread(img->rgb, 1, n, f) != n)
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   return 1;
}

static unsigned long ds_lit(const ds_image_t *img)
{
   size_t        i, n = (size_t)img->w * img->h;
   unsigned long lit  = 0;
   for (i = 0; i < n; i++)
      if (img->rgb[i * 3] | img->rgb[i * 3 + 1] | img->rgb[i * 3 + 2])
         lit++;
   return lit;
}

static int ds_compare(const char *a_path, const char *b_path, int tol, unsigned long max_pm)
{
   ds_image_t    a, b;
   size_t        i, n;
   unsigned long bad = 0, lit_a, lit_b;
   if (!ds_read_ppm(a_path, &a) || !ds_read_ppm(b_path, &b))
   {
      printf("compare: cannot read %s or %s\n", a_path, b_path);
      return 1;
   }
   if (a.w != b.w || a.h != b.h)
   {
      printf("compare: %ux%u against %ux%u\n", a.w, a.h, b.w, b.h);
      return 1;
   }
   n     = (size_t)a.w * a.h;
   lit_a = ds_lit(&a);
   lit_b = ds_lit(&b);
   for (i = 0; i < n * 3; i += 3)
   {
      int c;
      for (c = 0; c < 3; c++)
      {
         int d = (int)a.rgb[i + c] - (int)b.rgb[i + c];
         if (d < 0)
            d = -d;
         if (d > tol)
         {
            bad++;
            break;
         }
      }
   }
   printf("compare: %ux%u, %lu and %lu pixels lit, %lu differ by more than %d (%lu per mille)\n",
         a.w, a.h, lit_a, lit_b, bad, tol, n ? (unsigned long)(bad * 1000 / n) : 0UL);
   if (lit_b && !lit_a)
      return 1;
   return (n && bad * 1000 > max_pm * n) ? 1 : 0;
}

#ifdef _WIN32

/* ---- the frontend ---- */

typedef void (*ds_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef void* retro_get_memory_data_t(unsigned);

enum { DS_SW, DS_D3D11, DS_D3D12 };

static int          s_api;
static int          s_v2 = 1;
static int          s_asked_v2;   /* the core asked for version 2 */
static unsigned     s_slots = 1;
static const char  *s_scale = "1";
static char         s_system_dir[1024];
static struct retro_hw_render_callback s_hw;
static int          s_hw_set;
static unsigned     s_frames, s_valid;
static int          s_read_now;
static ds_image_t   s_img;
static pD3DCompile  s_compile;

/* D3D11 */
static ID3D11Device        *s_dev11;
static ID3D11DeviceContext *s_ctx11;
static ID3D11Texture2D     *s_tex11;
static int                  s_lock_depth, s_used_by_frontend;
static struct retro_hw_render_interface_d3d11 s_if11;

/* D3D12 */
static ID3D12Device        *s_dev12;
static ID3D12CommandQueue  *s_queue12;
static ID3D12Fence         *s_host_fence;
static UINT64               s_host_value;
static HANDLE               s_host_event;
static ID3D12Resource      *s_tex12;
static ID3D12Fence         *s_tex12_fence;
static UINT64               s_tex12_value;
static struct retro_hw_render_interface_d3d12 s_if12;

static ds_fn_t ds_sym(HMODULE h, const char *name)
{
   return (ds_fn_t)GetProcAddress(h, name);
}

static void ds_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("D3DSW_VERBOSE"))
      return;
   va_start(ap, fmt);
   printf("  [core] ");
   vprintf(fmt, ap);
   va_end(ap);
   fflush(stdout);
}

/* B8G8R8A8 or R8G8B8A8 rows into s_img, top row first. */
static void ds_take(const unsigned char *src, unsigned pitch, unsigned w, unsigned h, int bgra)
{
   unsigned x, y;
   free(s_img.rgb);
   s_img.w   = w;
   s_img.h   = h;
   s_img.rgb = (unsigned char*)malloc((size_t)w * h * 3 + 1);
   for (y = 0; y < h; y++)
   {
      const unsigned char *row = src + (size_t)y * pitch;
      unsigned char       *out = s_img.rgb + (size_t)y * w * 3;
      for (x = 0; x < w; x++)
      {
         out[x * 3 + 0] = row[x * 4 + (bgra ? 2 : 0)];
         out[x * 3 + 1] = row[x * 4 + 1];
         out[x * 3 + 2] = row[x * 4 + (bgra ? 0 : 2)];
      }
   }
}

/* D3DSW_CONTENT (ds_content_frame, below the reads it uses). */
static int      s_content_on;
static unsigned s_run;   /* retro_runs begun, counted from 1 */
static void ds_content_frame(void);

static int ds_is_bgra(DXGI_FORMAT f)
{
   return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
       || f == DXGI_FORMAT_B8G8R8X8_UNORM;
}

static int ds_read11(ID3D11Texture2D *tex, unsigned w, unsigned h)
{
   D3D11_TEXTURE2D_DESC     desc;
   D3D11_MAPPED_SUBRESOURCE map;
   ID3D11Texture2D         *staging = NULL;
   int                      ok      = 0;
   ID3D11Texture2D_GetDesc(tex, &desc);
   if (w > desc.Width || h > desc.Height)
      return 0;
   desc.MipLevels      = 1;
   desc.ArraySize      = 1;
   desc.SampleDesc.Count   = 1;
   desc.SampleDesc.Quality = 0;
   desc.Usage          = D3D11_USAGE_STAGING;
   desc.BindFlags      = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   desc.MiscFlags      = 0;
   if (FAILED(ID3D11Device_CreateTexture2D(s_dev11, &desc, NULL, &staging)))
      return 0;
   ID3D11DeviceContext_CopySubresourceRegion(s_ctx11, (ID3D11Resource*)staging, 0, 0, 0, 0,
         (ID3D11Resource*)tex, 0, NULL);
   if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx11, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map)))
   {
      ds_take((const unsigned char*)map.pData, map.RowPitch, w, h, ds_is_bgra(desc.Format));
      ID3D11DeviceContext_Unmap(s_ctx11, (ID3D11Resource*)staging, 0);
      ok = 1;
   }
   ID3D11Texture2D_Release(staging);
   return ok;
}

static void ds_wait12(void)
{
   s_host_value++;
   ID3D12CommandQueue_Signal(s_queue12, s_host_fence, s_host_value);
   if (ID3D12Fence_GetCompletedValue(s_host_fence) < s_host_value)
   {
      ID3D12Fence_SetEventOnCompletion(s_host_fence, s_host_value, s_host_event);
      WaitForSingleObject(s_host_event, INFINITE);
   }
}

static void ds_barrier12(ID3D12GraphicsCommandList *cl, ID3D12Resource *res,
      D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b;
   if (from == to)
      return;
   b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
   b.Transition.pResource   = res;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = from;
   b.Transition.StateAfter  = to;
   ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

static int ds_read12(ID3D12Resource *tex, ID3D12Fence *fence, UINT64 value, unsigned w, unsigned h)
{
   D3D12_RESOURCE_DESC                desc, bdesc;
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
   D3D12_HEAP_PROPERTIES              heap;
   D3D12_TEXTURE_COPY_LOCATION        src, dst;
   UINT64                             total = 0;
   ID3D12Resource                    *buf   = NULL;
   ID3D12CommandAllocator            *alloc = NULL;
   ID3D12GraphicsCommandList         *cl    = NULL;
   void                              *map   = NULL;
   int                                ok    = 0;

   /* through the vtable: C sees GetDesc as returning through a pointer */
   tex->lpVtbl->GetDesc(tex, &desc);
   if (w > desc.Width || h > desc.Height)
      return 0;
   ID3D12Device_GetCopyableFootprints(s_dev12, &desc, 0, 1, 0, &fp, NULL, NULL, &total);
   heap.Type                   = D3D12_HEAP_TYPE_READBACK;
   heap.CPUPageProperty        = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
   heap.MemoryPoolPreference   = D3D12_MEMORY_POOL_UNKNOWN;
   heap.CreationNodeMask       = 0;
   heap.VisibleNodeMask        = 0;
   bdesc.Dimension             = D3D12_RESOURCE_DIMENSION_BUFFER;
   bdesc.Alignment             = 0;
   bdesc.Width                 = total;
   bdesc.Height                = 1;
   bdesc.DepthOrArraySize      = 1;
   bdesc.MipLevels             = 1;
   bdesc.Format                = DXGI_FORMAT_UNKNOWN;
   bdesc.SampleDesc.Count      = 1;
   bdesc.SampleDesc.Quality    = 0;
   bdesc.Layout                = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   bdesc.Flags                 = D3D12_RESOURCE_FLAG_NONE;
   if (FAILED(ID3D12Device_CreateCommittedResource(s_dev12, &heap, D3D12_HEAP_FLAG_NONE, &bdesc,
               D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&buf))
         || FAILED(ID3D12Device_CreateCommandAllocator(s_dev12, D3D12_COMMAND_LIST_TYPE_DIRECT,
               &IID_ID3D12CommandAllocator, (void**)&alloc))
         || FAILED(ID3D12Device_CreateCommandList(s_dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
               &IID_ID3D12GraphicsCommandList, (void**)&cl)))
      goto done;
   ds_barrier12(cl, tex, s_if12.required_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
   src.pResource        = tex;
   src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   src.SubresourceIndex = 0;
   dst.pResource        = buf;
   dst.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   dst.PlacedFootprint  = fp;
   ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
   ds_barrier12(cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, s_if12.required_state);
   ID3D12GraphicsCommandList_Close(cl);
   if (fence)
      ID3D12CommandQueue_Wait(s_queue12, fence, value);
   ID3D12CommandQueue_ExecuteCommandLists(s_queue12, 1, (ID3D12CommandList* const*)&cl);
   ds_wait12();
   if (SUCCEEDED(ID3D12Resource_Map(buf, 0, NULL, &map)))
   {
      ds_take((const unsigned char*)map, fp.Footprint.RowPitch, w, h, ds_is_bgra(desc.Format));
      ID3D12Resource_Unmap(buf, 0, NULL);
      ok = 1;
   }
done:
   if (cl)    ID3D12GraphicsCommandList_Release(cl);
   if (alloc) ID3D12CommandAllocator_Release(alloc);
   if (buf)   ID3D12Resource_Release(buf);
   return ok;
}

/* D3D11 version 1: the frame is the view left at pixel shader slot 0. */
static ID3D11Texture2D *ds_slot0_texture(void)
{
   ID3D11ShaderResourceView *srv = NULL;
   ID3D11Resource           *res = NULL;
   ID3D11Texture2D          *tex = NULL;
   ID3D11DeviceContext_PSGetShaderResources(s_ctx11, 0, 1, &srv);
   if (!srv)
      return NULL;
   ID3D11ShaderResourceView_GetResource(srv, &res);
   ID3D11ShaderResourceView_Release(srv);
   if (res)
   {
      ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void**)&tex);
      ID3D11Resource_Release(res);
   }
   return tex;
}

static void ds_video(const void *d, unsigned w, unsigned h, size_t p)
{
   s_frames++;
   if (!d)
      return;
   s_valid++;
   if (d != RETRO_HW_FRAME_BUFFER_VALID)
   {
      if (s_read_now)
         ds_take((const unsigned char*)d, (unsigned)p, w, h, 1);
      if (s_content_on && s_read_now)
         ds_content_frame();
      return;
   }
   if (s_api == DS_D3D11)
   {
      /* The frontend draws its frame now, on the context. */
      s_used_by_frontend = 1;
      if (s_read_now)
      {
         ID3D11Texture2D *tex = s_v2 ? s_tex11 : ds_slot0_texture();
         if (tex)
         {
            ds_read11(tex, w, h);
            if (!s_v2)
               ID3D11Texture2D_Release(tex);
         }
      }
   }
   else if (s_api == DS_D3D12 && s_read_now && s_tex12)
      ds_read12(s_tex12, s_tex12_fence, s_tex12_value, w, h);
   if (s_content_on && s_read_now)
      ds_content_frame();
}

static void ds_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t ds_audio_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void ds_poll(void) { }

/* D3DSW_PAD: a scripted pad on port 0, "first+count=button|button;...":
 * the buttons are held through the retro_runs first to first + count - 1,
 * counted from 1. Buttons by their libretro names (b is cross, a circle,
 * y square, x triangle). */
static const char *const ds_button_names[16] = {
   "b", "y", "select", "start", "up", "down", "left", "right",
   "a", "x", "l", "r", "l2", "r2", "l3", "r3"
};
static unsigned ds_pad_mask(unsigned run)
{
   const char *s    = getenv("D3DSW_PAD");
   unsigned    mask = 0;
   while (s && *s)
   {
      char               *end;
      const unsigned long first = strtoul(s, &end, 10);
      unsigned long       count = 1;
      unsigned            held  = 0;
      s = end;
      if (*s == '+')
      {
         count = strtoul(s + 1, &end, 10);
         s     = end;
      }
      if (*s == '=')
         s++;
      while (*s && *s != ';')
      {
         size_t   len = strcspn(s, "|;");
         unsigned b;
         for (b = 0; b < 16; b++)
            if (strlen(ds_button_names[b]) == len && !strncmp(s, ds_button_names[b], len))
               held |= 1u << b;
         s += len;
         if (*s == '|')
            s++;
      }
      if (run >= first && run < first + count)
         mask |= held;
      if (*s == ';')
         s++;
   }
   return mask;
}
static int16_t ds_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
   unsigned mask;
   (void)i;
   if (p != 0 || d != RETRO_DEVICE_JOYPAD)
      return 0;
   mask = ds_pad_mask(s_run);
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)mask;
   return id < 16 ? (int16_t)((mask >> id) & 1) : 0;
}

/* D3DSW_OPTIONS: the value of core option @key, "key=value;key=value", or NULL. */
static const char *ds_option(const char *key)
{
   static char     values[8][128];
   static unsigned next;
   const char     *opts = getenv("D3DSW_OPTIONS");
   size_t          klen = strlen(key);
   while (opts && *opts)
   {
      const char *end = strchr(opts, ';');
      size_t      len = end ? (size_t)(end - opts) : strlen(opts);
      if (len > klen && opts[klen] == '=' && !strncmp(opts, key, klen) && len - klen - 1 < sizeof(values[0]))
      {
         char *v = values[next++ % 8];
         memcpy(v, opts + klen + 1, len - klen - 1);
         v[len - klen - 1] = '\0';
         return v;
      }
      opts = end ? end + 1 : NULL;
   }
   return NULL;
}

/* D3D11 version 2 */
static bool ds_lock11(void *h)
{
   bool used = false;
   (void)h;
   if (s_lock_depth++ == 0)
   {
      used               = s_used_by_frontend ? true : false;
      s_used_by_frontend = 0;
   }
   return used;
}
static void ds_unlock11(void *h) { (void)h; s_lock_depth--; }
static void ds_set_texture11(void *h, ID3D11Texture2D *t)
{
   (void)h;
   if (t)
      ID3D11Texture2D_AddRef(t);
   if (s_tex11)
      ID3D11Texture2D_Release(s_tex11);
   s_tex11 = t;
}
static unsigned ds_sync_index(void *h) { (void)h; return s_valid % s_slots; }
static unsigned ds_sync_mask(void *h) { (void)h; return (1u << s_slots) - 1u; }
static void ds_wait_sync11(void *h) { (void)h; }

/* D3D12 */
static void ds_set_texture12(void *h, ID3D12Resource *t, DXGI_FORMAT f)
{
   (void)h; (void)f;
   if (t)
      ID3D12Resource_AddRef(t);
   if (s_tex12)
      ID3D12Resource_Release(s_tex12);
   s_tex12       = t;
   s_tex12_fence = NULL;
}
static void ds_set_texture_fenced12(void *h, ID3D12Resource *t, DXGI_FORMAT f, ID3D12Fence *fence, UINT64 v)
{
   ds_set_texture12(h, t, f);
   s_tex12_fence = fence;
   s_tex12_value = v;
}
static void ds_wait_sync12(void *h) { (void)h; ds_wait12(); }

static bool ds_environment(unsigned cmd, void *data)
{
   struct retro_variable *var;
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = ds_log;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = s_system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_LANGUAGE:
         *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = 2;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         const struct retro_hw_render_callback *cb = (const struct retro_hw_render_callback*)data;
         if (cb->context_type == RETRO_HW_CONTEXT_NONE)
            return true;
         if (   (s_api == DS_D3D11 && cb->context_type == RETRO_HW_CONTEXT_D3D11)
             || (s_api == DS_D3D12 && cb->context_type == RETRO_HW_CONTEXT_D3D12))
         {
            s_hw     = *cb;
            s_hw_set = 1;
            return true;
         }
         return false;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      {
         struct retro_hw_render_context_negotiation_interface *q =
            (struct retro_hw_render_context_negotiation_interface*)data;
         q->interface_version = 0;
         if (s_v2 && (   (s_api == DS_D3D11 && q->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11)
                      || (s_api == DS_D3D12 && q->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12)))
            q->interface_version = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
         /* (both negotiation structs start with type, version, max) */
         if (!s_v2)
            return false;
         s_asked_v2 = ((const struct retro_hw_render_context_negotiation_interface_d3d12*)data)
               ->max_render_interface_version >= 2;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         if (s_api == DS_D3D11 && s_dev11)
         {
            if (!s_asked_v2)
               s_if11.interface_version = RETRO_HW_RENDER_INTERFACE_D3D11_VERSION;
            *(const struct retro_hw_render_interface**)data = (const struct retro_hw_render_interface*)&s_if11;
            return true;
         }
         if (s_api == DS_D3D12 && s_dev12)
         {
            if (!s_asked_v2)
               s_if12.interface_version = RETRO_HW_RENDER_INTERFACE_D3D12_VERSION;
            *(const struct retro_hw_render_interface**)data = (const struct retro_hw_render_interface*)&s_if12;
            return true;
         }
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
         var        = (struct retro_variable*)data;
         var->value = NULL;
         if      (!strcmp(var->key, "pcsx2_bios"))     var->value = "lrps2_d3dswdraw.bin";
         else if (!strcmp(var->key, "pcsx2_renderer")) var->value =
            s_api == DS_D3D11 ? "D3D11" : s_api == DS_D3D12 ? "D3D12" : "Software (SW)";
         else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = ds_option(var->key) ? ds_option(var->key) : "disabled";
         else if (!strcmp(var->key, "pcsx2_upscale_multiplier")) var->value = s_scale;
         else if (!strcmp(var->key, "pcsx2_deinterlace_mode")) var->value =
            getenv("D3DSW_DEINTERLACE") ? getenv("D3DSW_DEINTERLACE") : "Automatic";
         if (!var->value)
            var->value = ds_option(var->key);
         return var->value != NULL;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      default:
         return false;
   }
}

static int ds_device11(void)
{
   static const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
   D3D_FEATURE_LEVEL got;
   if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
               &want, 1, D3D11_SDK_VERSION, &s_dev11, &got, &s_ctx11)))
      return 0;
   s_if11.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D11;
   s_if11.interface_version = s_v2 ? RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2 : RETRO_HW_RENDER_INTERFACE_D3D11_VERSION;
   s_if11.handle            = &s_if11;
   s_if11.device            = s_dev11;
   s_if11.context           = s_ctx11;
   s_if11.featureLevel      = got;
   s_if11.D3DCompile        = s_compile;
   if (s_v2)
   {
      s_if11.lock_context        = ds_lock11;
      s_if11.unlock_context      = ds_unlock11;
      s_if11.set_texture         = ds_set_texture11;
      s_if11.get_sync_index      = ds_sync_index;
      s_if11.get_sync_index_mask = ds_sync_mask;
      s_if11.wait_sync_index     = ds_wait_sync11;
   }
   return 1;
}

static int ds_device12(void)
{
   D3D12_COMMAND_QUEUE_DESC qd;
   if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&s_dev12)))
      return 0;
   qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
   qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
   qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
   qd.NodeMask = 0;
   if (   FAILED(ID3D12Device_CreateCommandQueue(s_dev12, &qd, &IID_ID3D12CommandQueue, (void**)&s_queue12))
       || FAILED(ID3D12Device_CreateFence(s_dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_host_fence)))
      return 0;
   s_host_event = CreateEventA(NULL, FALSE, FALSE, NULL);
   s_if12.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D12;
   s_if12.interface_version = s_v2 ? RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2 : RETRO_HW_RENDER_INTERFACE_D3D12_VERSION;
   s_if12.handle            = &s_if12;
   s_if12.device            = s_dev12;
   s_if12.queue             = s_queue12;
   s_if12.D3DCompile        = s_compile;
   /* RetroArch's: it copies the frame out */
   s_if12.required_state    = getenv("D3DSW_SRV_STATE") ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                        : D3D12_RESOURCE_STATE_COPY_SOURCE;
   s_if12.set_texture       = ds_set_texture12;
   if (s_v2)
   {
      s_if12.get_sync_index      = ds_sync_index;
      s_if12.get_sync_index_mask = ds_sync_mask;
      s_if12.wait_sync_index     = ds_wait_sync12;
      s_if12.set_texture_fenced  = ds_set_texture_fenced12;
   }
   return 1;
}

/* --latency: tests/latency's case (its --bios image) on this frontend.
 * Before a retro_run the host writes a grey to the word the guest reads
 * every vsync, as the core latches the pad there; the frontend reads the
 * picture it was handed in the video callback, after the fence or the
 * context work that carries it and before anything the core issues
 * later. The grey must be in the picture of the retro_run it was written
 * before. */
#define DS_LAT_INPUT    0x180000u
#define DS_LAT_WARMUP   60
#define DS_LAT_TRIALS   24
#define DS_LAT_MAX_RUNS 8

static void ds_put32(unsigned char *p, unsigned long v)
{
   p[0] = (unsigned char)v;
   p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16);
   p[3] = (unsigned char)(v >> 24);
}

/* The grey of the middle pixel of the picture read last, or -1. */
static int ds_mid(void)
{
   if (!s_img.rgb || !s_img.w || !s_img.h)
      return -1;
   return s_img.rgb[((size_t)(s_img.h / 2) * s_img.w + s_img.w / 2) * 3];
}

static int ds_latency(retro_simple_t *run, unsigned char *ram)
{
   unsigned hist[DS_LAT_MAX_RUNS + 1];
   unsigned t, n, i, worst = 0;
   memset(hist, 0, sizeof(hist));
   s_read_now = 1;
   ds_put32(ram + DS_LAT_INPUT, 0x80000000UL);
   for (i = 0; i < DS_LAT_WARMUP; i++)
      run();
   for (t = 0; t < DS_LAT_TRIALS; t++)
   {
      /* tests/latency's greys and holds: 8 apart, held 1 to 4 retro_runs
       * after they show */
      const unsigned grey = 16 + 8 * (t % 28);
      const unsigned hold = 1 + (t % 4);
      ds_put32(ram + DS_LAT_INPUT, 0x80000000UL | grey | (grey << 8) | ((unsigned long)grey << 16));
      for (n = 0; n < DS_LAT_MAX_RUNS; n++)
      {
         int shown;
         free(s_img.rgb);
         s_img.rgb = NULL;
         run();
         shown = ds_mid();
         if (shown >= (int)grey - 2 && shown <= (int)grey + 2)
            break;
      }
      hist[n]++;
      if (n > worst)
         worst = n;
      for (i = 1; i < hold; i++)
         run();
   }
   printf("  latency: retro_runs after the one the input was written before:");
   for (n = 0; n <= DS_LAT_MAX_RUNS; n++)
      if (hist[n])
         printf(" %u%s:%u", n, n == DS_LAT_MAX_RUNS ? "+" : "", hist[n]);
   printf("\n");
   return worst == 0;
}

/* A frontend rebuilding its video driver: the context goes, and with it
 * the device; a new one comes, and the context is reset on it. */
static int ds_reinit(void)
{
   if (s_hw.context_destroy)
      s_hw.context_destroy();
   ds_set_texture11(NULL, NULL);
   ds_set_texture12(NULL, NULL, DXGI_FORMAT_UNKNOWN);
   if (s_api == DS_D3D11)
   {
      ID3D11DeviceContext_ClearState(s_ctx11);
      ID3D11DeviceContext_Flush(s_ctx11);
      ID3D11DeviceContext_Release(s_ctx11);
      ID3D11Device_Release(s_dev11);
      s_ctx11 = NULL;
      s_dev11 = NULL;
      if (!ds_device11())
         return 0;
   }
   else
   {
      ds_wait12();
      ID3D12Fence_Release(s_host_fence);
      ID3D12CommandQueue_Release(s_queue12);
      ID3D12Device_Release(s_dev12);
      CloseHandle(s_host_event);
      s_host_fence = NULL;
      s_queue12    = NULL;
      s_dev12      = NULL;
      if (!ds_device12())
         return 0;
   }
   if (s_hw.context_reset)
      s_hw.context_reset();
   return 1;
}

static int ds_copy(const char *src, const char *dst)
{
   static unsigned char buf[1 << 16];
   size_t n;
   FILE  *in = fopen(src, "rb"), *out;
   if (!in)
      return 0;
   out = fopen(dst, "wb");
   if (!out)
   {
      fclose(in);
      return 0;
   }
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      fwrite(buf, 1, n, out);
   fclose(in);
   return fclose(out) == 0;
}

/* D3DSW_CONTENT: real content (a disc image's path) is loaded instead of
 * nothing, and every frame is read as the frontend sees it, in the video
 * callback. D3DSW_HASH_LOG has each picture's hash reported with the
 * retro_run that handed it over; D3DSW_DUMP names the retro_runs,
 * "n,n,...", whose picture is written to D3DSW_DUMP_DIR as <run>.ppm.
 * D3DSW_LOAD_STATE names a state to load before the first retro_run, and
 * D3DSW_SAVE_STATE "run:path" a state to save after one. */
typedef size_t retro_serialize_size_t(void);
typedef bool retro_serialize_t(void*, size_t);
typedef bool retro_unserialize_t(const void*, size_t);

static int ds_listed(const char *list, unsigned n)
{
   while (list && *list)
   {
      char *end;
      if (strtoul(list, &end, 10) == n && end != list)
         return 1;
      list = (*end == ',') ? end + 1 : (*end ? end + 1 : end);
   }
   return 0;
}

static void ds_content_frame(void)
{
   unsigned long long h = 14695981039346656037ULL;
   size_t             i, n;
   if (!s_img.rgb)
   {
      if (getenv("D3DSW_HASH_LOG"))
         printf("  run %u: no picture\n", s_run);
      return;
   }
   n = (size_t)s_img.w * s_img.h * 3;
   for (i = 0; i < n; i++)
      h = (h ^ s_img.rgb[i]) * 1099511628211ULL;
   if (getenv("D3DSW_HASH_LOG"))
      printf("  run %u: %ux%u, hash %016llx\n", s_run, s_img.w, s_img.h, h);
   if (ds_listed(getenv("D3DSW_DUMP"), s_run))
   {
      char path[1100];
      sprintf(path, "%.1000s/%u.ppm", getenv("D3DSW_DUMP_DIR") ? getenv("D3DSW_DUMP_DIR") : ".", s_run);
      ds_write_ppm(path, &s_img);
   }
   free(s_img.rgb);
   s_img.rgb = NULL;
}

static int ds_state_file(const char *path, int save, retro_serialize_size_t *size_fn,
      retro_serialize_t *save_fn, retro_unserialize_t *load_fn)
{
   FILE  *f;
   void  *buf;
   size_t size = 0;
   int    ok   = 0;
   if (save)
   {
      size = size_fn();
      buf  = size ? malloc(size) : NULL;
      if (buf && save_fn(buf, size) && (f = fopen(path, "wb")) != NULL)
      {
         ok = fwrite(buf, 1, size, f) == size;
         ok = (fclose(f) == 0) && ok;
      }
      free(buf);
      return ok;
   }
   if (!(f = fopen(path, "rb")))
      return 0;
   fseek(f, 0, SEEK_END);
   size = (size_t)ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = size ? malloc(size) : NULL;
   if (buf && fread(buf, 1, size, f) == size)
      ok = load_fn(buf, size);
   fclose(f);
   free(buf);
   return ok;
}

static int ds_fail(const char *what)
{
   fprintf(stderr, "d3dswdraw: %s\n", what);
   return 1;
}

int main(int argc, char **argv)
{
   char                            path[1100];
   HMODULE                         h, dc;
   retro_set_environment_t        *set_environment;
   retro_set_video_refresh_t      *set_video;
   retro_set_audio_sample_t       *set_audio;
   retro_set_audio_sample_batch_t *set_audio_batch;
   retro_set_input_poll_t         *set_poll;
   retro_set_input_state_t        *set_input;
   retro_simple_t                 *init_fn, *deinit_fn, *unload_game, *run;
   retro_load_game_t              *load_game;
   retro_get_memory_data_t        *memory_data;
   retro_serialize_size_t         *serialize_size;
   retro_serialize_t              *serialize;
   retro_unserialize_t            *unserialize;
   struct retro_game_info          game;
   const char                     *save_at = getenv("D3DSW_SAVE_STATE");
   int                             frame, frames, reinit_at;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc == 6 && !strcmp(argv[1], "--compare"))
      return ds_compare(argv[2], argv[3], atoi(argv[4]), strtoul(argv[5], NULL, 10));
   if (argc < 6)
      return ds_fail("usage: d3dswdraw <core> <scratch-dir> <bios-image> <D3D11|D3D12|SW> <out.ppm> [v1|v2] [scale]");
   if      (!strcmp(argv[4], "D3D11")) s_api = DS_D3D11;
   else if (!strcmp(argv[4], "D3D12")) s_api = DS_D3D12;
   else if (!strcmp(argv[4], "SW"))    s_api = DS_SW;
   else return ds_fail("unknown renderer");
   if (argc > 6)
      s_v2 = strcmp(argv[6], "v1") != 0;
   if (argc > 7)
      s_scale = argv[7];
   frames  = getenv("D3DSW_FRAMES") ? atoi(getenv("D3DSW_FRAMES")) : 30;
   reinit_at = getenv("D3DSW_REINIT_AT") ? atoi(getenv("D3DSW_REINIT_AT")) : -1;
   s_slots = getenv("D3DSW_SYNC_SLOTS") ? (unsigned)atoi(getenv("D3DSW_SYNC_SLOTS")) : 1;
   if (s_slots < 1 || s_slots > 3)
      s_slots = 1;
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

   if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
      return ds_fail("scratch path too long");
   strcpy(s_system_dir, argv[2]);
   sprintf(path, "%s/pcsx2", s_system_dir);
   _mkdir(path);
   sprintf(path, "%s/pcsx2/bios", s_system_dir);
   _mkdir(path);
   sprintf(path, "%s/pcsx2/bios/lrps2_d3dswdraw.bin", s_system_dir);
   if (!ds_copy(argv[3], path))
      return ds_fail("cannot copy the BIOS image");
   /* A real BIOS's NVM and MEC images go beside it, under its new name. */
   if (strlen(argv[3]) < 1000 && strrchr(argv[3], '.'))
   {
      static const char *const ext[2] = { "nvm", "mec" };
      char     src[1100];
      unsigned e;
      for (e = 0; e < 2; e++)
      {
         strcpy(src, argv[3]);
         strcpy(strrchr(src, '.') + 1, ext[e]);
         sprintf(path, "%s/pcsx2/bios/lrps2_d3dswdraw.%s", s_system_dir, ext[e]);
         ds_copy(src, path);
      }
   }

   dc = LoadLibraryA("d3dcompiler_47.dll");
   if (dc)
      s_compile = (pD3DCompile)(void (*)(void))GetProcAddress(dc, "D3DCompile");
   if (s_api == DS_D3D11 && !ds_device11())
   {
      printf("d3dswdraw: skip: no D3D11 device\n");
      return 0;
   }
   if (s_api == DS_D3D12 && !ds_device12())
   {
      printf("d3dswdraw: skip: no D3D12 device\n");
      return 0;
   }

   h = LoadLibraryA(argv[1]);
   if (!h)
      return ds_fail("cannot load the core");
   set_environment = (retro_set_environment_t*)ds_sym(h, "retro_set_environment");
   set_video       = (retro_set_video_refresh_t*)ds_sym(h, "retro_set_video_refresh");
   set_audio       = (retro_set_audio_sample_t*)ds_sym(h, "retro_set_audio_sample");
   set_audio_batch = (retro_set_audio_sample_batch_t*)ds_sym(h, "retro_set_audio_sample_batch");
   set_poll        = (retro_set_input_poll_t*)ds_sym(h, "retro_set_input_poll");
   set_input       = (retro_set_input_state_t*)ds_sym(h, "retro_set_input_state");
   init_fn         = (retro_simple_t*)ds_sym(h, "retro_init");
   deinit_fn       = (retro_simple_t*)ds_sym(h, "retro_deinit");
   unload_game     = (retro_simple_t*)ds_sym(h, "retro_unload_game");
   run             = (retro_simple_t*)ds_sym(h, "retro_run");
   load_game       = (retro_load_game_t*)ds_sym(h, "retro_load_game");
   memory_data     = (retro_get_memory_data_t*)ds_sym(h, "retro_get_memory_data");
   serialize_size  = (retro_serialize_size_t*)ds_sym(h, "retro_serialize_size");
   serialize       = (retro_serialize_t*)ds_sym(h, "retro_serialize");
   unserialize     = (retro_unserialize_t*)ds_sym(h, "retro_unserialize");
   if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll
         || !set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game)
      return ds_fail("the core is missing a libretro export");

   set_environment(ds_environment);
   set_video(ds_video);
   set_audio(ds_audio);
   set_audio_batch(ds_audio_batch);
   set_poll(ds_poll);
   set_input(ds_input);
   init_fn();
   memset(&game, 0, sizeof(game));
   game.path = getenv("D3DSW_CONTENT");
   if (!load_game(game.path ? &game : NULL))
      return ds_fail("retro_load_game failed");
   if (s_api != DS_SW)
   {
      if (!s_hw_set)
         return ds_fail("the core did not ask for the context");
      if (s_hw.context_reset)
         s_hw.context_reset();
   }
   if (getenv("D3DSW_LATENCY"))
   {
      unsigned char *ram = memory_data ? (unsigned char*)memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
      if (!ram)
         return ds_fail("no main RAM to write the input to");
      if (!ds_latency(run, ram))
         return ds_fail("an input reached the picture of a later retro_run");
      frames = 0;
   }
   if (game.path)
   {
      /* (D3DSW_LOAD_AFTER: the state is loaded after that many retro_runs) */
      for (frame = getenv("D3DSW_LOAD_AFTER") ? atoi(getenv("D3DSW_LOAD_AFTER")) : 0; frame > 0; frame--)
         run();
      s_content_on = 1;
      if (getenv("D3DSW_LOAD_STATE") && (!serialize_size || !ds_state_file(getenv("D3DSW_LOAD_STATE"), 0,
               serialize_size, serialize, unserialize)))
         return ds_fail("cannot load the state");
   }
   for (frame = 0; frame < frames; frame++)
   {
      s_read_now = s_content_on || (frame == frames - 1);
      s_run++;
      run();
      if (s_content_on && save_at && strchr(save_at, ':') && strtoul(save_at, NULL, 10) == s_run)
      {
         if (!serialize_size || !ds_state_file(strchr(save_at, ':') + 1, 1, serialize_size, serialize, unserialize))
            return ds_fail("cannot save the state");
         printf("  state saved after retro_run %u\n", s_run);
      }
      if (s_api != DS_SW && frame + 1 == reinit_at && !ds_reinit())
         return ds_fail("no device after the reinit");
   }
   if (s_lock_depth != 0)
      return ds_fail("the D3D11 context lock is unbalanced");
   if (s_content_on)
   {
      printf("d3dswdraw: %s, %u frames, %u with a picture\n", argv[4], s_frames, s_valid);
      s_img.rgb = (unsigned char*)malloc(4);
      s_img.w   = s_img.h = 1;
      s_img.rgb[0] = s_img.rgb[1] = s_img.rgb[2] = 0;
   }
   if (!s_img.rgb)
   {
      printf("d3dswdraw: %u frames, %u with a picture\n", s_frames, s_valid);
      return ds_fail("no picture was read back");
   }
   if (!ds_write_ppm(argv[5], &s_img))
      return ds_fail("cannot write the picture");
   printf("d3dswdraw: %s %ux%u, %lu pixels lit, %u frames, %u with a picture\n",
         argv[4], s_img.w, s_img.h, ds_lit(&s_img), s_frames, s_valid);

   if (s_api != DS_SW && s_hw.context_destroy)
      s_hw.context_destroy();
   unload_game();
   deinit_fn();
   ds_set_texture11(NULL, NULL);
   ds_set_texture12(NULL, NULL, DXGI_FORMAT_UNKNOWN);
   return 0;
}

#else

int main(int argc, char **argv)
{
   if (argc == 6 && !strcmp(argv[1], "--compare"))
      return ds_compare(argv[2], argv[3], atoi(argv[4]), strtoul(argv[5], NULL, 10));
   printf("d3dswdraw: skip: D3D is Windows only\n");
   return 0;
}

#endif
