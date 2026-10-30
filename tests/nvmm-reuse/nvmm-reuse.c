/* Hardware-free reproduction of ARKP-1546: a listener attached with
 * interpipesrc stream-sync=compensate-ts reads NVMM surfaces its producer's
 * buffer pool has already reused, and so gets frames from up to four frames
 * later, then the last four again. Run by tests/nvmm-reuse/run.sh; not part
 * of the meson build.
 *
 * The producer stands in for a Jetson upload `nvvidconv`. Its output pool
 * copies the shape of nvvidconv's GstNvFilterBufferPool and
 * GstNvFilterMemoryAllocator (L4T R36.4.3 gst-nvvidconv/gstnvvconv.c):
 *   - min = max = 4 buffers (`output-buffers`, NVFILTER_MAX_BUF);
 *   - CUSTOM_ALLOC allocator, memory flagged NO_SHARE, no mem_copy;
 *   - the mapped bytes are a small descriptor (NvBufSurface) that points at
 *     pixels owned elsewhere (the hardware surface).
 * Each frame is written into the pixels when the producer acquires the
 * buffer, as the VIC write is, and carries its true frame number in OFFSET.
 * A frame the pool has no buffer for is dropped, as a leaky queue in front of
 * the converter drops frames while it waits on its pool.
 *
 *   appsrc ! tee ! queue leaky=2 ! fakesink                         (preview)
 *              tee ! queue leaky=2 max-size-buffers=10 ! interpipesink name=feed
 *   interpipesrc listen-to=feed stream-sync=MODE ! queue ! appsink
 *
 * The consumer stands in for a recording pipeline: it reads each frame's
 * content through its descriptor, costs FRAME_US per frame and stalls for
 * STALL_US every 0.5 s, as a video branch does while qtmux waits on its audio
 * pad. A frame whose content is not its own frame number read a reused
 * surface.
 *
 * Build: cc -O2 -o nvmm-reuse nvmm-reuse.c \
 *          $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
 * Run:   GST_PLUGIN_PATH=<interpipe build> ./nvmm-reuse MODE SECONDS FRAME_US STALL_US
 *        MODE = compensate-ts | passthrough-ts; prints one JSON line.
 */
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

#define FPS 60
#define POOL_BUFFERS 4

/* ---- mock NVMM allocator ------------------------------------------------ */

typedef struct {
  volatile guint64 frame; /* the "picture": which frame was last written */
} MockPixels;

typedef struct {
  MockPixels *pixels; /* like NvBufSurface.surfaceList → dmabuf */
} MockSurface;

typedef struct {
  GstMemory mem;
  MockSurface *surface;
} MockMemory;

typedef struct {
  GstAllocator parent;
} MockAllocator;
typedef struct {
  GstAllocatorClass parent_class;
} MockAllocatorClass;

GType mock_allocator_get_type (void);
G_DEFINE_TYPE (MockAllocator, mock_allocator, GST_TYPE_ALLOCATOR);

static gpointer
mock_mem_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  return ((MockMemory *) mem)->surface;
}

static void
mock_mem_unmap (GstMemory * mem)
{
}

static GstMemory *
mock_mem_share (GstMemory * mem, gssize offset, gssize size)
{
  g_assert_not_reached ();
  return NULL;
}

static GstMemory *
mock_alloc (GstAllocator * allocator, gsize size, GstAllocationParams * params)
{
  MockMemory *mem = g_new0 (MockMemory, 1);
  mem->surface = g_new0 (MockSurface, 1);
  mem->surface->pixels = g_new0 (MockPixels, 1);
  gst_memory_init (GST_MEMORY_CAST (mem), GST_MEMORY_FLAG_NO_SHARE, allocator,
      NULL, sizeof (MockSurface), 0, 0, sizeof (MockSurface));
  return GST_MEMORY_CAST (mem);
}

static void
mock_free (GstAllocator * allocator, GstMemory * memory)
{
  MockMemory *mem = (MockMemory *) memory;
  g_free (mem->surface->pixels);
  g_free (mem->surface);
  g_free (mem);
}

static void
mock_allocator_class_init (MockAllocatorClass * klass)
{
  GstAllocatorClass *ac = GST_ALLOCATOR_CLASS (klass);
  ac->alloc = mock_alloc;
  ac->free = mock_free;
}

static void
mock_allocator_init (MockAllocator * self)
{
  GstAllocator *a = GST_ALLOCATOR_CAST (self);
  a->mem_type = "MockNVMM";
  a->mem_map = mock_mem_map;
  a->mem_unmap = mock_mem_unmap;
  a->mem_share = mock_mem_share;
  /* mem_copy left as GstAllocator's fallback, as nvvidconv leaves it */
  GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

/* ---- producer ----------------------------------------------------------- */

typedef struct {
  GstElement *pipeline;
  GstAppSrc *src;
  GstBufferPool *pool;
  volatile gboolean running;
  guint64 produced;
  guint64 dropped; /* pool empty: the converter would have blocked */
} Producer;

static gpointer
produce (gpointer data)
{
  Producer *p = data;
  GstClock *clock = gst_element_get_clock (p->pipeline);
  GstClockTime base = gst_element_get_base_time (p->pipeline);
  GstClockTime t0 = gst_clock_get_time (clock);
  GstBufferPoolAcquireParams params = {.flags =
        GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT };

  for (guint64 n = 0; p->running; n++) {
    GstClockTime due = t0 + gst_util_uint64_scale (n, GST_SECOND, FPS);
    GstClockTime now = gst_clock_get_time (clock);
    if (due > now)
      g_usleep ((due - now) / 1000);

    GstBuffer *buf = NULL;
    if (gst_buffer_pool_acquire_buffer (p->pool, &buf, &params) != GST_FLOW_OK) {
      p->dropped++;
      continue;
    }
    GstMapInfo map;
    gst_buffer_map (buf, &map, GST_MAP_WRITE);
    ((MockSurface *) map.data)->pixels->frame = n;      /* the VIC write */
    gst_buffer_unmap (buf, &map);
    GST_BUFFER_OFFSET (buf) = n;
    GST_BUFFER_PTS (buf) = gst_clock_get_time (clock) - base;
    GST_BUFFER_DURATION (buf) = GST_SECOND / FPS;
    gst_app_src_push_buffer (p->src, buf);
    p->produced++;
  }
  gst_object_unref (clock);
  return NULL;
}

/* ---- consumer ----------------------------------------------------------- */

typedef struct {
  guint64 frame;   /* the entry's own frame number (OFFSET) */
  guint64 content; /* what its surface showed when read */
  gboolean copied; /* the entry was a descriptor copy, not the pool buffer */
} Entry;

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  if (argc < 5) {
    fprintf (stderr, "usage: %s compensate-ts|passthrough-ts SECONDS FRAME_US "
        "STALL_US\n", argv[0]);
    return 2;
  }
  const char *mode = argv[1];
  double seconds = g_ascii_strtod (argv[2], NULL);
  gulong frame_us = strtoul (argv[3], NULL, 10);
  gulong stall_us = strtoul (argv[4], NULL, 10);

  GstAllocator *allocator = g_object_new (mock_allocator_get_type (), NULL);
  GstCaps *caps = gst_caps_from_string
      ("video/x-raw(memory:NVMM),format=NV12,width=1080,height=1920,framerate=60/1");

  Producer p = { 0 };
  p.pool = gst_buffer_pool_new ();
  GstStructure *config = gst_buffer_pool_get_config (p.pool);
  gst_buffer_pool_config_set_params (config, caps, sizeof (MockSurface),
      POOL_BUFFERS, POOL_BUFFERS);
  gst_buffer_pool_config_set_allocator (config, allocator, NULL);
  g_assert (gst_buffer_pool_set_config (p.pool, config));
  g_assert (gst_buffer_pool_set_active (p.pool, TRUE));

  GError *err = NULL;
  p.pipeline = gst_parse_launch ("appsrc name=src is-live=true format=time "
      "do-timestamp=false ! tee name=t "
      "t. ! queue leaky=2 max-size-buffers=10 ! fakesink sync=false async=false "
      "t. ! queue leaky=2 max-size-buffers=10 name=record_queue ! "
      "interpipesink name=feed forward-events=true forward-eos=false sync=false "
      "async=false",
      &err);
  g_assert_no_error (err);
  p.src = GST_APP_SRC (gst_bin_get_by_name (GST_BIN (p.pipeline), "src"));
  gst_app_src_set_caps (p.src, caps);
  gst_element_set_state (p.pipeline, GST_STATE_PLAYING);
  gst_element_get_state (p.pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  p.running = TRUE;
  GThread *producer = g_thread_new ("producer", produce, &p);
  g_usleep (G_USEC_PER_SEC);

  gchar *desc = g_strdup_printf ("interpipesrc listen-to=feed format=time "
      "is-live=true allow-renegotiation=false stream-sync=%s ! "
      "queue max-size-buffers=200 max-size-time=1000000000 "
      "max-size-bytes=10485760 ! appsink name=sink sync=false max-buffers=1 "
      "drop=false", mode);
  GstElement *rec = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  GstAppSink *sink = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (rec), "sink"));
  gst_element_set_state (rec, GST_STATE_PLAYING);

  GArray *entries = g_array_new (FALSE, FALSE, sizeof (Entry));
  gint64 end = g_get_monotonic_time () + (gint64) (seconds * G_USEC_PER_SEC);
  gint64 next_stall = g_get_monotonic_time () + G_USEC_PER_SEC / 2;
  while (g_get_monotonic_time () < end) {
    GstSample *s = gst_app_sink_try_pull_sample (sink, GST_SECOND);
    if (!s)
      continue;
    GstBuffer *b = gst_sample_get_buffer (s);
    GstMapInfo map;
    gst_buffer_map (b, &map, GST_MAP_READ);
    Entry e = {
      .frame = GST_BUFFER_OFFSET (b),
      .content = ((MockSurface *) map.data)->pixels->frame,
      .copied = gst_buffer_peek_memory (b, 0)->allocator != allocator,
    };
    gst_buffer_unmap (b, &map);
    g_array_append_val (entries, e);
    gst_sample_unref (s);
    g_usleep (frame_us);
    if (stall_us && g_get_monotonic_time () >= next_stall) {
      g_usleep (stall_us);
      next_stall += G_USEC_PER_SEC / 2;
    }
  }

  gst_element_set_state (rec, GST_STATE_NULL);
  p.running = FALSE;
  g_thread_join (producer);
  gst_element_set_state (p.pipeline, GST_STATE_NULL);

  guint64 wrong = 0, copied = 0;
  guint64 by_offset[9] = { 0 };
  for (guint i = 0; i < entries->len; i++) {
    Entry *e = &g_array_index (entries, Entry, i);
    copied += e->copied;
    if (e->content != e->frame) {
      wrong++;
      by_offset[MIN (ABS ((gint64) e->content - (gint64) e->frame), 8)]++;
    }
  }

  printf ("{\"mode\":\"%s\",\"produced\":%" G_GUINT64_FORMAT
      ",\"producerDropped\":%" G_GUINT64_FORMAT ",\"recorded\":%u,"
      "\"descriptorCopies\":%" G_GUINT64_FORMAT ",\"wrongContent\":%"
      G_GUINT64_FORMAT ",\"wrongByOffset\":{", mode, p.produced, p.dropped,
      entries->len, copied, wrong);
  gboolean first = TRUE;
  for (int k = 1; k <= 8; k++) {
    if (!by_offset[k])
      continue;
    printf ("%s\"%s%d\":%" G_GUINT64_FORMAT, first ? "" : ",",
        k == 8 ? ">=" : "", k, by_offset[k]);
    first = FALSE;
  }
  printf ("}}\n");
  return 0;
}
