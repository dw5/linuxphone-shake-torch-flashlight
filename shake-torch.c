/* shake-torch: shake the phone to switch the torch on or off.
 *
 * Runs on FuriOS and Sailfish OS. Both have sensorfwd for the accelerometer and a
 * torch service on the session bus; only the torch service differs:
 *   FuriOS:   io.furios.Flashlightd, Brightness / MaxBrightness / SetBrightness
 *   Sailfish: org.sailfish.flashlight.provider, toggleFlashlight
 *
 * Accelerometer, from sensorfwd: ask for a session over D-Bus (system bus), connect to
 * /run/sensord.sock, read one '\n', send the session id (int32). Then each write is a
 * uint32 count followed by that many readings of 24 bytes: uint64 timestamp (us), then
 * x, y, z as floats in milli-g, then 4 bytes of padding.
 *
 * Usage: shake-torch                 run (as a user service)
 *        shake-torch --monitor       print how hard the phone is moving, to tune
 *        shake-torch --record FILE   also save every reading to FILE, to tune later
 *        shake-torch --replay FILE   run the detector over a recording; no torch
 *        shake-torch --toggle        switch the torch once and exit
 * Settings: ~/.config/shake-torch.conf, reread on SIGHUP (see README.md).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>

#define SENSOR_SERVICE "com.nokia.SensorService"
#define SENSOR_MANAGER "/SensorManager"
#define SENSOR_ID "accelerometersensor"
#define SENSOR_PATH "/SensorManager/accelerometersensor"
#define SENSOR_IFACE "local.AccelerometerSensor"
#define SENSOR_SOCKET "/run/sensord.sock"
#define READING_SIZE 24
#define INTERVAL_MS 20
#define WINDOW_MAX 256  /* readings kept for the detector: 5 s at 50 a second */

#define FURIOS_TORCH "io.furios.Flashlightd"
#define FURIOS_TORCH_PATH "/io/furios/Flashlightd"
#define SAILFISH_TORCH "org.sailfish.flashlight.provider"
#define SAILFISH_TORCH_PATH "/org/sailfish/flashlight/provider"

typedef struct {
    /* settings */
    gboolean enabled;
    double threshold_g;   /* how far from 1 g a reading must be to count as shaking */
    int shake_ms;         /* how long the shaking must last ... */
    double share;         /* ... with this share of the readings over the threshold */
    int cooldown_ms;      /* after switching, ignore movement this long */
    gboolean screen_off;  /* keep the sensor running with the screen off */
    gboolean vibrate;
    int buzz_on_ms, buzz_gap_ms, buzz_off_ms;  /* on: two buzzes with a pause; off: one */
    gboolean log_movement;  /* log each movement's strength, for tuning */

    gboolean monitor;
    gboolean replay;      /* reading a recording: report, don't switch */
    FILE *record;
    GMainLoop *loop;
    GDBusConnection *system, *session;
    guint watch, retry;

    /* the sensor session */
    int sid;
    int fd;
    guint fd_source;
    GByteArray *buf;
    gboolean got_tag;

    /* the detector: recent readings, oldest first */
    guint64 when[WINDOW_MAX];
    gboolean over[WINDOW_MAX];
    int n, first;
    guint64 quiet_until;
    double bump;          /* strongest movement in the current bump (log-movement) */
    guint64 last_print;
    double print_max;
} App;

static App app;

/* Settings */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "shake-torch.conf", NULL);
}

static void load_settings(void)
{
    app.enabled = TRUE;
    app.threshold_g = 1.0;
    app.shake_ms = 500;
    app.share = 0.5;
    app.cooldown_ms = 1500;
    app.screen_off = TRUE;
    app.vibrate = TRUE;
    app.buzz_on_ms = 150;
    app.buzz_gap_ms = 250;
    app.buzz_off_ms = 300;
    app.log_movement = FALSE;

    g_autofree char *path = config_path();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_autoptr(GError) e = NULL;
#define GET(type, field, key) do { \
            g_clear_error(&e); \
            __typeof__(app.field) v = g_key_file_get_##type(kf, "shake-torch", key, &e); \
            if (!e) app.field = v; \
        } while (0)
        GET(boolean, enabled, "enabled");
        GET(double, threshold_g, "threshold");
        GET(integer, shake_ms, "shake-ms");
        GET(double, share, "share");
        GET(integer, cooldown_ms, "cooldown-ms");
        GET(boolean, screen_off, "screen-off");
        GET(boolean, vibrate, "vibrate");
        GET(integer, buzz_on_ms, "buzz-on-ms");
        GET(integer, buzz_gap_ms, "buzz-gap-ms");
        GET(integer, buzz_off_ms, "buzz-off-ms");
        GET(boolean, log_movement, "log-movement");
#undef GET
    }
    app.threshold_g = CLAMP(app.threshold_g, 0.3, 4.0);
    app.shake_ms = CLAMP(app.shake_ms, 200, 3000);
    app.share = CLAMP(app.share, 0.1, 1.0);
    app.cooldown_ms = CLAMP(app.cooldown_ms, 300, 10000);
    app.buzz_on_ms = CLAMP(app.buzz_on_ms, 10, 2000);
    app.buzz_gap_ms = CLAMP(app.buzz_gap_ms, 50, 2000);
    app.buzz_off_ms = CLAMP(app.buzz_off_ms, 10, 2000);
}

/* Torch */

static GVariant *call(GDBusConnection *bus, const char *name, const char *path,
                      const char *iface, const char *method, GVariant *args,
                      const char *reply, GError **error)
{
    return g_dbus_connection_call_sync(bus, name, path, iface, method, args,
                                       reply ? G_VARIANT_TYPE(reply) : NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 3000, NULL, error);
}

static guint32 furios_prop(const char *prop, GError **error)
{
    g_autoptr(GVariant) r = call(app.session, FURIOS_TORCH, FURIOS_TORCH_PATH,
                                 "org.freedesktop.DBus.Properties", "Get",
                                 g_variant_new("(ss)", FURIOS_TORCH, prop), "(v)", error);
    if (!r)
        return 0;
    g_autoptr(GVariant) v = NULL;
    g_variant_get(r, "(v)", &v);
    return g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : 0;
}

/* Switch the torch; returns whether it is now on, -1 if no torch answered. */
static int toggle_torch(void)
{
    g_autoptr(GError) e = NULL;
    guint32 now = furios_prop("Brightness", &e);
    if (!e) {
        guint32 want = now ? 0 : furios_prop("MaxBrightness", NULL);
        if (!now && !want)
            want = 1;
        g_autoptr(GVariant) r = call(app.session, FURIOS_TORCH, FURIOS_TORCH_PATH, FURIOS_TORCH,
                                     "SetBrightness", g_variant_new("(u)", want), NULL, &e);
        if (r)
            return want > 0;
        g_warning("FuriOS torch: %s", e->message);
        return -1;
    }
    g_clear_error(&e);
    g_autoptr(GVariant) r = call(app.session, SAILFISH_TORCH, SAILFISH_TORCH_PATH, SAILFISH_TORCH,
                                 "toggleFlashlight", NULL, "(b)", &e);
    if (r) {
        gboolean on;
        g_variant_get(r, "(b)", &on);
        return on;
    }
    g_warning("No torch service answered (%s, %s): %s", FURIOS_TORCH, SAILFISH_TORCH, e->message);
    return -1;
}

/* Buzz twice for on, once for off, so it can be told without looking. Always, also
 * in the silent profile: the user asked for that.
 *
 * FuriOS and other hybris phones: the Android vibrator as an LED class device
 * (duration in ms, then activate), writable by the phone's user. This plays whatever
 * the feedback profile is: feedbackd's "silent" profile would swallow it, and its
 * button events are a 7 ms tick. Elsewhere (Sailfish): ngfd. */
#define VIBRATOR "/sys/class/leds/vibrator"

/* A plain write: g_file_set_contents() writes a temporary file and renames it, which a
 * device file in /sys can't take. */
static void write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    if (write(fd, value, strlen(value)) < 0)
        g_debug("%s: %s", path, g_strerror(errno));
    close(fd);
}

static gboolean buzz_sysfs(gpointer ms)
{
    char d[16];
    g_snprintf(d, sizeof d, "%d", GPOINTER_TO_INT(ms));
    write_sysfs(VIBRATOR "/duration", d);
    write_sysfs(VIBRATOR "/activate", "1");
    return G_SOURCE_REMOVE;
}

static void vibrate(gboolean on)
{
    if (!app.vibrate)
        return;
    if (access(VIBRATOR "/activate", W_OK) == 0) {
        if (on) {
            buzz_sysfs(GINT_TO_POINTER(app.buzz_on_ms));
            g_timeout_add(app.buzz_on_ms + app.buzz_gap_ms, buzz_sysfs, GINT_TO_POINTER(app.buzz_on_ms));
        } else {
            buzz_sysfs(GINT_TO_POINTER(app.buzz_off_ms));
        }
        return;
    }
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(app.session, "com.nokia.NonGraphicFeedback1.Backend",
                           "/com/nokia/NonGraphicFeedback1", "com.nokia.NonGraphicFeedback1",
                           "Play", g_variant_new("(sa{sv})", "feedback_press", &props),
                           NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
}

/* Shake detection
 *
 * How far the total acceleration is from 1 g: zero at rest however the phone is held,
 * large while it's shaken (it speeds up and slows down hard, back and forth). One step
 * or bump gives a short spike; a shake keeps it high. So: switch when, over the last
 * shake-ms, at least a share of the readings were over the threshold.
 *
 * Measured on the FLX1s (2026-11-10), as the strongest point of each movement with
 * gravity taken out: walking and handling 0.5 to 1.25 g, firm shakes 1.8 to 9 g. */

static void shaken(guint64 t)
{
    app.n = 0;
    app.quiet_until = t + (guint64)app.cooldown_ms * 1000;
    if (app.replay) {
        g_print("%8.2f s  SHAKE\n", t / 1e6);
        return;
    }
    if (!app.enabled)
        return;
    int on = toggle_torch();
    if (on >= 0) {
        g_message("Shake: torch %s", on ? "on" : "off");
        vibrate(on);
    }
}

static void on_reading(guint64 t, double x, double y, double z)
{
    if (app.record)
        fprintf(app.record, "%" G_GUINT64_FORMAT " %.1f %.1f %.1f\n", t, x, y, z);
    x /= 1000.0, y /= 1000.0, z /= 1000.0;  /* milli-g to g */
    double m = fabs(sqrt(x * x + y * y + z * z) - 1.0);

    if (app.monitor && !app.replay) {
        app.print_max = MAX(app.print_max, m);
        if (t - app.last_print >= 250000) {
            g_autofree char *bars = g_strnfill(MIN((int)(app.print_max * 10), 60), '#');
            g_print("%5.2f g %s%s\n", app.print_max,
                    app.print_max >= app.threshold_g ? "*" : " ", bars);
            app.last_print = t;
            app.print_max = 0;
        }
    }
    if (app.log_movement) {
        /* Each movement over 0.4 g, logged once at its strongest. */
        if (m >= 0.4)
            app.bump = MAX(app.bump, m);
        else if (app.bump > 0 && m < 0.2) {
            if (app.replay)
                g_print("%8.2f s  movement %.2f g\n", t / 1e6, app.bump);
            else
                g_message("Movement: %.2f g", app.bump);
            app.bump = 0;
        }
    }

    if (t < app.quiet_until)
        return;
    /* Keep the readings of the last shake-ms. */
    guint64 span = (guint64)app.shake_ms * 1000;
    if (app.n == WINDOW_MAX) {
        app.first = (app.first + 1) % WINDOW_MAX;
        app.n--;
    }
    int last = (app.first + app.n) % WINDOW_MAX;
    app.when[last] = t;
    app.over[last] = m >= app.threshold_g;
    app.n++;
    while (app.n > 0 && t - app.when[app.first] > span) {
        app.first = (app.first + 1) % WINDOW_MAX;
        app.n--;
    }
    /* Only judge a full window: readings arrive every 20 ms. */
    if (t - app.when[app.first] < span - 40000 || app.n < 5)
        return;
    int over = 0;
    for (int i = 0; i < app.n; i++)
        over += app.over[(app.first + i) % WINDOW_MAX];
    if (over >= app.share * app.n)
        shaken(t);
}

/* The sensor session */

static void stop_sensor(void);
static void start_sensor(void);

static gboolean retry_sensor(gpointer _)
{
    app.retry = 0;
    if (app.sid < 0)
        start_sensor();
    return G_SOURCE_REMOVE;
}

static void restart_later(void)
{
    stop_sensor();
    if (!app.retry)
        app.retry = g_timeout_add_seconds(5, retry_sensor, NULL);
}

static gboolean on_socket(int fd, GIOCondition cond, gpointer _)
{
    guint8 chunk[4096];
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return G_SOURCE_CONTINUE;
    if (n <= 0) {
        g_warning("Sensor connection closed");
        app.fd_source = 0;  /* removed by returning */
        restart_later();
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(app.buf, chunk, n);
    guint off = 0;
    if (!app.got_tag) {
        app.got_tag = TRUE;  /* the '\n' sensorfwd sends first */
        off = 1;
    }
    for (;;) {
        guint32 count;
        if (app.buf->len - off < 4)
            break;
        memcpy(&count, app.buf->data + off, 4);
        if (count == 0 || count > 1000) {
            g_warning("Unexpected sensor data (count %u); reconnecting", count);
            app.fd_source = 0;
            restart_later();
            return G_SOURCE_REMOVE;
        }
        if (app.buf->len - off < 4 + count * READING_SIZE)
            break;
        for (guint32 i = 0; i < count; i++) {
            const guint8 *p = app.buf->data + off + 4 + i * READING_SIZE;
            guint64 t;
            float xyz[3];
            memcpy(&t, p, 8);
            memcpy(xyz, p + 8, 12);
            on_reading(t, xyz[0], xyz[1], xyz[2]);
        }
        off += 4 + count * READING_SIZE;
    }
    g_byte_array_remove_range(app.buf, 0, off);
    return G_SOURCE_CONTINUE;
}

static gboolean sensor_call(const char *method, GVariant *args)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = call(app.system, SENSOR_SERVICE, SENSOR_PATH, SENSOR_IFACE,
                                 method, args, NULL, &e);
    if (!r)
        g_warning("%s: %s", method, e->message);
    return r != NULL;
}

static void start_sensor(void)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager",
                                 "loadPlugin", g_variant_new("(s)", SENSOR_ID), "(b)", &e);
    gboolean ok = FALSE;
    if (r)
        g_variant_get(r, "(b)", &ok);
    if (!ok) {
        g_warning("sensorfwd has no accelerometer: %s", e ? e->message : "loadPlugin failed");
        return;
    }
    g_clear_pointer(&r, g_variant_unref);
    r = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager", "requestSensor",
             g_variant_new("(sx)", SENSOR_ID, (gint64)getpid()), "(i)", &e);
    if (!r) {
        g_warning("requestSensor: %s", e->message);
        restart_later();
        return;
    }
    g_variant_get(r, "(i)", &app.sid);
    if (app.sid < 0) {
        g_warning("requestSensor gave no session");
        restart_later();
        return;
    }

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    g_strlcpy(sa.sun_path, SENSOR_SOCKET, sizeof sa.sun_path);
    app.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    gint32 sid = app.sid;
    if (app.fd < 0 || connect(app.fd, (struct sockaddr *)&sa, sizeof sa) < 0
        || write(app.fd, &sid, sizeof sid) != sizeof sid) {
        g_warning("Can't connect to %s: %s", SENSOR_SOCKET, g_strerror(errno));
        restart_later();
        return;
    }
    g_unix_set_fd_nonblocking(app.fd, TRUE, NULL);
    app.got_tag = FALSE;
    g_byte_array_set_size(app.buf, 0);
    app.fd_source = g_unix_fd_add(app.fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_socket, NULL);

    sensor_call("setInterval", g_variant_new("(ii)", app.sid, INTERVAL_MS));
    sensor_call("setStandbyOverride", g_variant_new("(ib)", app.sid, app.screen_off));
    if (!sensor_call("start", g_variant_new("(i)", app.sid))) {
        restart_later();
        return;
    }
    app.n = 0;
    g_message("Watching the accelerometer (session %d; %.2f g for %d ms%s)",
              app.sid, app.threshold_g, app.shake_ms,
              app.screen_off ? "; also with the screen off" : "");
}

static void stop_sensor(void)
{
    if (app.fd_source)
        g_source_remove(app.fd_source);
    app.fd_source = 0;
    if (app.fd >= 0)
        close(app.fd);
    app.fd = -1;
    if (app.sid >= 0 && app.system) {
        g_autoptr(GVariant) r1 = call(app.system, SENSOR_SERVICE, SENSOR_PATH, SENSOR_IFACE, "stop",
                                      g_variant_new("(i)", app.sid), NULL, NULL);
        g_autoptr(GVariant) r2 = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager",
                                      "releaseSensor",
                                      g_variant_new("(six)", SENSOR_ID, app.sid, (gint64)getpid()),
                                      NULL, NULL);
    }
    app.sid = -1;
}

static void sensor_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer _)
{
    stop_sensor();
    start_sensor();
}

static void sensor_vanished(GDBusConnection *c, const char *name, gpointer _)
{
    if (app.sid >= 0)
        g_message("sensorfwd went away; waiting for it to come back");
    if (app.fd_source)
        g_source_remove(app.fd_source);
    app.fd_source = 0;
    if (app.fd >= 0)
        close(app.fd);
    app.fd = -1;
    app.sid = -1;  /* its sessions are gone with it */
}

static gboolean on_quit(gpointer _)
{
    g_main_loop_quit(app.loop);
    return G_SOURCE_CONTINUE;
}

static gboolean on_reload(gpointer _)
{
    load_settings();
    g_message("Settings reloaded");
    if (app.sid >= 0) {  /* screen-off may have changed */
        stop_sensor();
        start_sensor();
    }
    return G_SOURCE_CONTINUE;
}

static int replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        g_printerr("%s: %s\n", path, g_strerror(errno));
        return 1;
    }
    app.replay = TRUE;
    guint64 t;
    double x, y, z;
    int count = 0;
    while (fscanf(f, "%" G_GUINT64_FORMAT " %lf %lf %lf", &t, &x, &y, &z) == 4) {
        on_reading(t, x, y, z);
        count++;
    }
    fclose(f);
    g_print("%d readings\n", count);
    return 0;
}

static gboolean quit_loop(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    gboolean toggle = FALSE;
    g_autofree char *record = NULL, *replay_file = NULL;
    GOptionEntry entries[] = {
        { "monitor", 'm', 0, G_OPTION_ARG_NONE, &app.monitor, "Print how hard the phone is moving", NULL },
        { "record", 'r', 0, G_OPTION_ARG_FILENAME, &record, "Also save every reading to FILE", "FILE" },
        { "replay", 0, 0, G_OPTION_ARG_FILENAME, &replay_file, "Run the detector over a recording", "FILE" },
        { "toggle", 't', 0, G_OPTION_ARG_NONE, &toggle, "Switch the torch once and exit", NULL },
        { NULL }
    };
    g_autoptr(GOptionContext) ctx = g_option_context_new("- shake the phone to switch the torch");
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_autoptr(GError) e = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &e)) {
        g_printerr("%s\n", e->message);
        return 2;
    }

    load_settings();
    if (replay_file)
        return replay(replay_file);
    app.sid = -1;
    app.fd = -1;
    app.buf = g_byte_array_new();
    if (record) {
        app.record = fopen(record, "a");
        if (!app.record) {
            g_printerr("%s: %s\n", record, g_strerror(errno));
            return 1;
        }
        setvbuf(app.record, NULL, _IOLBF, 0);
    }
    app.session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!app.session) {
        g_printerr("No session bus: %s\n", e->message);
        return 1;
    }
    if (toggle) {
        int on = toggle_torch();
        if (on < 0)
            return 1;
        g_print("Torch %s\n", on ? "on" : "off");
        vibrate(on);
        if (on) {  /* let the second buzz happen */
            g_autoptr(GMainLoop) l = g_main_loop_new(NULL, FALSE);
            g_timeout_add(app.buzz_on_ms * 2 + app.buzz_gap_ms + 50, quit_loop, l);
            g_main_loop_run(l);
        }
        return 0;
    }
    app.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!app.system) {
        g_printerr("No system bus: %s\n", e->message);
        return 1;
    }

    app.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_quit, NULL);
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGHUP, on_reload, NULL);
    app.watch = g_bus_watch_name_on_connection(app.system, SENSOR_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               sensor_appeared, sensor_vanished, NULL, NULL);
    g_main_loop_run(app.loop);
    g_bus_unwatch_name(app.watch);
    stop_sensor();
    if (app.record)
        fclose(app.record);
    return 0;
}
