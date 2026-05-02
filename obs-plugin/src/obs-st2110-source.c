/* OBS Source: ST 2110-20 RFC 4175 RTP receiver.
 *
 * Owns one libst2110rx receiver per source instance. Library frames are
 * pushed into OBS as async UYVY video via obs_source_output_video().
 *
 * Properties (settings):
 *   multicast_addr  -- e.g. "239.10.21.20"
 *   port            -- e.g. 5000
 *   interface_addr  -- e.g. "10.0.0.1" or "" for any
 *   width / height  -- frame dimensions
 *   socket_buffer_mb-- SO_RCVBUF in MiB (default 128)
 *
 * Color: BT.709, limited range. Emit VIDEO_FORMAT_UYVY frames.
 *
 * Threading: libst2110rx runs its own receive thread; the frame callback
 * fires from that thread. obs_source_output_video() is thread-safe for
 * async sources, so we can call it directly without queueing.
 */

#include <obs-module.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "st2110rx.h"

#define ST2110_SETTING_ADDR       "multicast_addr"
#define ST2110_SETTING_PORT       "port"
#define ST2110_SETTING_IFACE      "interface_addr"
#define ST2110_SETTING_WIDTH      "width"
#define ST2110_SETTING_HEIGHT     "height"
#define ST2110_SETTING_SOCKBUF_MB "socket_buffer_mb"

struct st2110_source {
    obs_source_t *source;
    st2110rx_t *rx;

    /* Cached settings so get_width/get_height return the correct dims even
       before the first frame arrives. */
    uint32_t width;
    uint32_t height;
};

static const char *st2110_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "SMPTE ST 2110-20";
}

static void st2110_log_cb(st2110rx_log_level_t level, const char *msg, void *ud)
{
    int log_level;
    UNUSED_PARAMETER(ud);
    switch (level) {
    case ST2110RX_LOG_ERROR: log_level = LOG_ERROR; break;
    case ST2110RX_LOG_WARN:  log_level = LOG_WARNING; break;
    case ST2110RX_LOG_INFO:  log_level = LOG_INFO; break;
    default:                 log_level = LOG_DEBUG; break;
    }
    blog(log_level, "[obs-st2110] %s", msg);
}

static void st2110_frame_cb(const st2110rx_frame_t *frame, void *ud)
{
    struct st2110_source *ctx = (struct st2110_source *)ud;
    struct obs_source_frame obs_frame;

    if (!ctx || !frame || frame->incomplete) {
        return;
    }

    memset(&obs_frame, 0, sizeof(obs_frame));
    obs_frame.format = VIDEO_FORMAT_UYVY;
    obs_frame.width = frame->width;
    obs_frame.height = frame->height;
    obs_frame.timestamp = frame->timestamp_ns;
    obs_frame.data[0] = frame->data[0];
    obs_frame.linesize[0] = frame->linesize[0];

    /* BT.709 limited range, matches what camera bridge sender produces. */
    video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
                                obs_frame.color_matrix,
                                obs_frame.color_range_min,
                                obs_frame.color_range_max);

    obs_source_output_video(ctx->source, &obs_frame);
}

static void st2110_destroy_receiver(struct st2110_source *ctx)
{
    if (ctx->rx) {
        st2110rx_destroy(ctx->rx);
        ctx->rx = NULL;
    }
}

static void st2110_create_receiver(struct st2110_source *ctx, obs_data_t *settings)
{
    st2110rx_config_t cfg;
    int rc;

    st2110_destroy_receiver(ctx);

    memset(&cfg, 0, sizeof(cfg));
    cfg.multicast_addr = obs_data_get_string(settings, ST2110_SETTING_ADDR);
    cfg.port = (uint16_t)obs_data_get_int(settings, ST2110_SETTING_PORT);
    cfg.interface_addr = obs_data_get_string(settings, ST2110_SETTING_IFACE);
    cfg.width = (uint32_t)obs_data_get_int(settings, ST2110_SETTING_WIDTH);
    cfg.height = (uint32_t)obs_data_get_int(settings, ST2110_SETTING_HEIGHT);
    cfg.input_fmt = ST2110RX_FMT_YCBCR422_10BIT;
    cfg.output_fmt = ST2110RX_FMT_UYVY;
    cfg.socket_buffer_size = (uint32_t)(
        obs_data_get_int(settings, ST2110_SETTING_SOCKBUF_MB) * 1024UL * 1024UL);
    cfg.frame_cb = st2110_frame_cb;
    cfg.user_data = ctx;
    cfg.log_cb = st2110_log_cb;

    if (cfg.width == 0 || cfg.height == 0 || !cfg.multicast_addr ||
        cfg.multicast_addr[0] == '\0') {
        blog(LOG_WARNING, "[obs-st2110] missing required settings");
        return;
    }

    ctx->width = cfg.width;
    ctx->height = cfg.height;

    ctx->rx = st2110rx_create(&cfg);
    if (!ctx->rx) {
        blog(LOG_ERROR, "[obs-st2110] st2110rx_create failed");
        return;
    }

    rc = st2110rx_start(ctx->rx);
    if (rc != ST2110RX_OK) {
        blog(LOG_ERROR, "[obs-st2110] st2110rx_start failed: %d", rc);
        st2110_destroy_receiver(ctx);
        return;
    }

    blog(LOG_INFO, "[obs-st2110] receiving %s:%u (%ux%u) on iface=%s",
         cfg.multicast_addr, cfg.port, cfg.width, cfg.height,
         (cfg.interface_addr && cfg.interface_addr[0]) ? cfg.interface_addr : "any");
}

static void *st2110_create(obs_data_t *settings, obs_source_t *source)
{
    struct st2110_source *ctx = (struct st2110_source *)bzalloc(sizeof(*ctx));
    ctx->source = source;
    st2110_create_receiver(ctx, settings);
    return ctx;
}

static void st2110_destroy(void *data)
{
    struct st2110_source *ctx = (struct st2110_source *)data;
    if (!ctx) {
        return;
    }
    st2110_destroy_receiver(ctx);
    bfree(ctx);
}

static void st2110_update(void *data, obs_data_t *settings)
{
    struct st2110_source *ctx = (struct st2110_source *)data;
    /* Simplest approach: tear down + recreate on any change. The receiver
       is cheap to start (sub-millisecond), and any setting change (port,
       multicast group, dimensions) requires a full restart anyway. */
    st2110_create_receiver(ctx, settings);
}

static void st2110_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, ST2110_SETTING_ADDR, "239.10.21.20");
    obs_data_set_default_int(settings, ST2110_SETTING_PORT, 5000);
    obs_data_set_default_string(settings, ST2110_SETTING_IFACE, "");
    obs_data_set_default_int(settings, ST2110_SETTING_WIDTH, 1920);
    obs_data_set_default_int(settings, ST2110_SETTING_HEIGHT, 1080);
    obs_data_set_default_int(settings, ST2110_SETTING_SOCKBUF_MB, 128);
}

static obs_properties_t *st2110_get_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    UNUSED_PARAMETER(data);

    obs_properties_add_text(props, ST2110_SETTING_ADDR,
                            "Multicast group", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, ST2110_SETTING_PORT,
                           "Port", 1, 65535, 1);
    obs_properties_add_text(props, ST2110_SETTING_IFACE,
                            "Interface IP (blank = any)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, ST2110_SETTING_WIDTH,
                           "Width", 16, 8192, 2);
    obs_properties_add_int(props, ST2110_SETTING_HEIGHT,
                           "Height", 16, 4320, 1);
    obs_properties_add_int(props, ST2110_SETTING_SOCKBUF_MB,
                           "Socket RX buffer (MiB)", 4, 1024, 4);

    return props;
}

static uint32_t st2110_get_width(void *data)
{
    struct st2110_source *ctx = (struct st2110_source *)data;
    return ctx ? ctx->width : 0;
}

static uint32_t st2110_get_height(void *data)
{
    struct st2110_source *ctx = (struct st2110_source *)data;
    return ctx ? ctx->height : 0;
}

struct obs_source_info obs_st2110_source_info = {
    .id             = "obs_st2110_source",
    .type           = OBS_SOURCE_TYPE_INPUT,
    .output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name       = st2110_get_name,
    .create         = st2110_create,
    .destroy        = st2110_destroy,
    .update         = st2110_update,
    .get_defaults   = st2110_defaults,
    .get_properties = st2110_get_properties,
    .get_width      = st2110_get_width,
    .get_height     = st2110_get_height,
    .icon_type      = OBS_ICON_TYPE_CAMERA,
};
