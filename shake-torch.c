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
 *                                    (--monitor and --record never switch the torch)
 *        shake-torch --replay FILE   run the detector over a recording; no torch
 *        shake-torch --toggle        switch the torch once and exit
 * Settings: ~/.config/shake-torch.conf, reread on SIGHUP (see README.md), and over
 * D-Bus while running (local.ShakeTorch, for the Sailfish settings page).
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
    double threshold_g;   /* how hard the strongest swing must be, gravity taken out */
    int cooldown_ms;      /* at least this long between two switches */
    gboolean screen_off;  /* keep the sensor running with the screen off */
    gboolean vibrate;
    int buzz_on_ms, buzz_second_ms, buzz_gap_ms, buzz_off_ms;  /* on: two buzzes with a pause; off: one */
    gboolean log_movement;  /* log each movement's strength, for tuning */
    gboolean notify;      /* "Flashlight is on" notification while a shake has it on */
    guint32 notification; /* its id, 0 when none is shown */

    gboolean monitor;
    gboolean replay;      /* reading a recording: report, don't switch */
    FILE *record;
    GMainLoop *loop;
    GDBusConnection *system, *session;
    guint watch, retry;
    gboolean sensor_up;   /* sensorfwd is on the bus */

    /* sensor sessions: the accelerometer, and the gyroscope where there is one */
    struct Session {
        const char *id, *path, *iface, *what;
        void (*reading)(guint64 t, double x, double y, double z);
        int sid, fd;
        guint fd_source;
        GByteArray *buf;
        gboolean got_tag;
    } acc, gyr;
    double max_rotation;  /* degrees the phone may turn during a chop; 0: no check */

    /* the detector: two rings of recent readings, oldest first */
    struct { guint64 t; double x, y, z; } grav[WINDOW_MAX];   /* last second: gravity */
    int gn, gfirst;
    double gsx, gsy, gsz;                                     /* their sums */
    struct { guint64 t; double x, y, z, m; } mov[WINDOW_MAX]; /* last 0.6 s: movement */
    int mn, mfirst;
    gboolean armed;
    guint64 calm_since, quiet_until, switched_at;
    struct { guint64 t; double rate; } rot[WINDOW_MAX];      /* last second: turning, degrees/s */
    int rn, rfirst;
    double bump;          /* strongest movement in the current bump (log-movement) */
    guint64 last_print;
    double print_max, print_rot;
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
    app.threshold_g = 1.4;
    app.cooldown_ms = 1700;  /* the Motorola's quickest on-then-off in the user's hands */
    app.screen_off = TRUE;
    app.vibrate = TRUE;
    /* Motorola's: on is 200 ms, 100 ms pause, 400 ms; off is 300 ms */
    app.buzz_on_ms = 200;
    app.buzz_second_ms = 400;  /* 0: as long as the first */
    app.buzz_gap_ms = 100;
    app.buzz_off_ms = 300;
    app.log_movement = FALSE;
    app.max_rotation = 108;
    app.notify = TRUE;

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
        GET(integer, cooldown_ms, "cooldown-ms");
        GET(boolean, screen_off, "screen-off");
        GET(boolean, vibrate, "vibrate");
        GET(integer, buzz_on_ms, "buzz-on-ms");
        GET(integer, buzz_second_ms, "buzz-second-ms");
        GET(integer, buzz_gap_ms, "buzz-gap-ms");
        GET(integer, buzz_off_ms, "buzz-off-ms");
        GET(boolean, log_movement, "log-movement");
        GET(double, max_rotation, "max-rotation");
        GET(boolean, notify, "notify");
#undef GET
    }
    app.threshold_g = CLAMP(app.threshold_g, 0.5, 6.0);
    app.max_rotation = CLAMP(app.max_rotation, 0, 3600);
    app.cooldown_ms = CLAMP(app.cooldown_ms, 200, 10000);
    app.buzz_on_ms = CLAMP(app.buzz_on_ms, 10, 2000);
    if (app.buzz_second_ms <= 0)
        app.buzz_second_ms = app.buzz_on_ms;
    app.buzz_second_ms = CLAMP(app.buzz_second_ms, 10, 2000);
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

static gboolean furios_torch;  /* which torch service answered torch_state() */

/* Whether the torch is on: 1 or 0, -1 if no torch service answered. */
static int torch_state(void)
{
    g_autoptr(GError) e = NULL;
    guint32 now = furios_prop("Brightness", &e);
    furios_torch = !e;
    if (!e)
        return now > 0;
    g_clear_error(&e);
    g_autoptr(GVariant) r = call(app.session, SAILFISH_TORCH, SAILFISH_TORCH_PATH,
                                 "org.freedesktop.DBus.Properties", "Get",
                                 g_variant_new("(ss)", SAILFISH_TORCH, "flashlightOn"), "(v)", &e);
    if (r) {
        g_autoptr(GVariant) v = NULL;
        g_variant_get(r, "(v)", &v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
            return g_variant_get_boolean(v);
    }
    g_warning("No torch service answered (%s, %s): %s", FURIOS_TORCH, SAILFISH_TORCH,
              e ? e->message : "flashlightOn isn't a boolean");
    return -1;
}

static void torch_set(GObject *bus, GAsyncResult *res, gpointer done)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), res, &e);
    if (!r)
        g_warning("Torch: %s", e->message);
    if (done)
        ((GSourceFunc)done)(NULL);
}

/* Switch the torch to the other state than torch_state() just gave, without waiting
 * for it: on Sailfish the torch can take a moment, more so with the screen off, and
 * the buzz shouldn't wait for it. "done", if given, is called once it has answered. */
static void set_torch(gboolean on, GSourceFunc done)
{
    if (furios_torch) {
        guint32 want = on ? MAX(furios_prop("MaxBrightness", NULL), 1) : 0;
        g_dbus_connection_call(app.session, FURIOS_TORCH, FURIOS_TORCH_PATH, FURIOS_TORCH,
                               "SetBrightness", g_variant_new("(u)", want), NULL,
                               G_DBUS_CALL_FLAGS_NONE, 5000, NULL, torch_set, done);
    } else {
        g_dbus_connection_call(app.session, SAILFISH_TORCH, SAILFISH_TORCH_PATH, SAILFISH_TORCH,
                               "toggleFlashlight", NULL, G_VARIANT_TYPE("(b)"),
                               G_DBUS_CALL_FLAGS_NONE, 5000, NULL, torch_set, done);
    }
}

/* Buzz twice for on, once for off, so it can be told without looking. Always, also
 * in the silent profile: the user asked for that.
 *
 * Hybris phones (the FLX1s on FuriOS, the Jolla Phone on Sailfish): the Android
 * vibrator as an LED class device (duration in ms, then activate), writable by the
 * phone's user (on the Jolla through the "input" group). This plays whatever the
 * feedback profile is: feedbackd's "silent" profile would swallow it, and its button
 * events are a 7 ms tick. Without one: ngfd, Sailfish's feedback daemon. */
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

static void buzz_pattern(gboolean on, int on_ms, int second_ms, int gap_ms, int off_ms)
{
    if (access(VIBRATOR "/activate", W_OK) == 0) {
        if (on) {
            buzz_sysfs(GINT_TO_POINTER(on_ms));
            g_timeout_add(on_ms + gap_ms, buzz_sysfs, GINT_TO_POINTER(second_ms));
        } else {
            buzz_sysfs(GINT_TO_POINTER(off_ms));
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

static void vibrate(gboolean on)
{
    if (app.vibrate)
        buzz_pattern(on, app.buzz_on_ms, app.buzz_second_ms, app.buzz_gap_ms, app.buzz_off_ms);
}

/* Shake detection
 *
 * Gravity is the average of the last second (a shake goes back and forth, so it averages
 * out); what's left is how the phone is being moved. A shake, like Motorola's chop, is a
 * hard swing one way and back again: within the last 0.6 s, the strongest movement is
 * over the threshold, and along the same line the other way, within 200 ms of it, there
 * is at least 60% of the threshold. Measured: in every shake, gentle or hard, the swing
 * back came 60 to 140 ms from the swing; moving the phone and setting it down on a table
 * also go one way and back, but slowly (480 to 600 ms), and switched the torch before
 * this limit.
 * After switching nothing happens for cooldown-ms (1.7 s): with 0.8 to 1.2 s the end of
 * the same shake could switch the torch straight back off, which the user found silly.
 * The Motorola's quickest on-then-off in the user's hands took 1.7 to 2.1 s. After that
 * it is ready once the phone is calm (under 0.8 g for 0.25 s) or after 2 s at the
 * latest: a hand that keeps moving between "on" and "off" never calms down, and the
 * second shake should still work, as on the Motorola.
 *
 * Tuned on a recording from the FLX1s (2026-11-10, user's hand): walking peaked at
 * 1.25 g, gentle shakes at 1.5 to 2 g, normal ones at 4 to 10 g. At 1.4 g it caught 28
 * of 36 separate bursts and nothing while walking; see README.md. */

#define GRAVITY_US 1000000
#define SWING_US 600000
#define BACK 0.6
#define BACK_US 200000  /* the swing back must come this soon before or after */
#define CALM_G 0.8
#define CALM_US 250000
#define REARM_US 2000000  /* ready again after this, calm or not */
#define CHOP_US 350000    /* how far back to add up turning: Motorola's max_chop_duration_ms */
#define ROT_US 1000000    /* turning kept for the last second */

static void reset_detector(void)
{
    app.gn = app.gfirst = 0;
    app.gsx = app.gsy = app.gsz = 0;
    app.mn = app.mfirst = 0;
    app.armed = TRUE;
    app.calm_since = 0;
    app.rn = app.rfirst = 0;
}

/* The notification, like Motorola's: "Flashlight is on", tap to turn it off. Standard
 * org.freedesktop.Notifications, which Phosh and Sailfish's lipstick both serve. A tap
 * comes back as ActionInvoked "default"; on Sailfish, lipstick instead calls the
 * x-nemo-remote-action-default method (TorchOff on our own D-Bus interface). */
#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"

static void close_notification(void)
{
    if (!app.notification)
        return;
    g_dbus_connection_call(app.session, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "CloseNotification",
                           g_variant_new("(u)", app.notification), NULL, G_DBUS_CALL_FLAGS_NONE,
                           2000, NULL, NULL, NULL);
    app.notification = 0;
}

static void notified(GObject *bus, GAsyncResult *res, gpointer _)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), res, &e);
    if (r)
        g_variant_get(r, "(u)", &app.notification);
    else
        g_warning("Notification: %s", e->message);
}

static void show_notification(void)
{
    if (!app.notify)
        return;
    const char *actions[] = { "default", "Turn off", NULL };
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(1));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("device"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("local.ShakeTorch.Settings"));
    g_variant_builder_add(&hints, "{sv}", "x-nemo-icon", g_variant_new_string("icon-m-flashlight"));
    g_variant_builder_add(&hints, "{sv}", "x-nemo-remote-action-default",
                          g_variant_new_string("local.ShakeTorch /local/ShakeTorch local.ShakeTorch TorchOff"));
    g_variant_builder_add(&hints, "{sv}", "x-nemo-feedback", g_variant_new_string(""));
    g_dbus_connection_call(app.session, NOTIFY_NAME, NOTIFY_PATH, NOTIFY_NAME, "Notify",
                           g_variant_new("(susss^asa{sv}i)", "Shake Torch", app.notification,
                                         "flashlight-enabled-symbolic", "Flashlight is on",
                                         "Tap to turn off the flashlight.", actions, &hints, 0),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, notified, NULL);
}

/* Turn the torch off from the notification: no buzz, the tap is feedback enough. */
static void torch_off(void)
{
    close_notification();
    if (torch_state() == 1)
        set_torch(FALSE, NULL);
}

static void on_notification_signal(GDBusConnection *c, const char *sender, const char *path,
                                   const char *iface, const char *signal, GVariant *args, gpointer _)
{
    guint32 id;
    if (g_str_equal(signal, "ActionInvoked")) {
        const char *action;
        g_variant_get(args, "(u&s)", &id, &action);
        if (id && id == app.notification && g_str_equal(action, "default"))
            torch_off();
    } else if (g_str_equal(signal, "NotificationClosed")) {
        g_variant_get(args, "(uu)", &id, NULL);
        if (id && id == app.notification)
            app.notification = 0;
    }
}

/* The torch went off some other way (quick settings): the notification goes too. */
static void on_torch_signal(GDBusConnection *c, const char *sender, const char *path,
                            const char *iface, const char *signal, GVariant *args, gpointer _)
{
    if (!app.notification)
        return;
    gboolean on = TRUE;
    if (g_str_equal(signal, "flashlightOnChanged")) {
        g_variant_get(args, "(b)", &on);
    } else {  /* PropertiesChanged on io.furios.Flashlightd */
        const char *name;
        g_autoptr(GVariant) changed = NULL;
        g_variant_get(args, "(&s@a{sv}@as)", &name, &changed, NULL);
        guint32 brightness;
        if (g_str_equal(name, FURIOS_TORCH)
            && g_variant_lookup(changed, "Brightness", "u", &brightness))
            on = brightness > 0;
    }
    if (!on)
        close_notification();
}

static void watch_notifications(void)
{
    g_dbus_connection_signal_subscribe(app.session, NULL, NOTIFY_NAME, NULL, NOTIFY_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_notification_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe(app.session, NULL, SAILFISH_TORCH, "flashlightOnChanged",
                                       SAILFISH_TORCH_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_torch_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe(app.session, NULL, "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged", FURIOS_TORCH_PATH, FURIOS_TORCH,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_torch_signal, NULL, NULL);
}

static void shaken(guint64 t)
{
    app.armed = FALSE;
    app.calm_since = 0;
    app.switched_at = t;
    app.quiet_until = t + (guint64)app.cooldown_ms * 1000;
    if (app.replay) {
        g_print("%8.2f s  SHAKE\n", t / 1e6);
        return;
    }
    if (!app.enabled || app.monitor || app.record)
        return;  /* tuning runs only report: the service may be switching at the same time */
    /* Buzz first, then switch: the buzz is what says it worked. */
    int was = torch_state();
    if (was < 0)
        return;
    g_message("Shake: torch %s", was ? "off" : "on");
    vibrate(!was);
    set_torch(!was, NULL);
    if (was)
        close_notification();
    else
        show_notification();
}

/* Gyroscope: sensorfwd gives the turning rate in milli-degrees per second. */
static void on_gyro(guint64 t, double x, double y, double z)
{
    if (app.record)
        fprintf(app.record, "g %" G_GUINT64_FORMAT " %.1f %.1f %.1f %" G_GINT64_FORMAT "\n",
                t, x, y, z, app.replay ? 0 : g_get_monotonic_time());
    double rate = sqrt(x * x + y * y + z * z) / 1000.0;
    if (app.rn == WINDOW_MAX)
        app.rfirst = (app.rfirst + 1) % WINDOW_MAX, app.rn--;
    int i = (app.rfirst + app.rn++) % WINDOW_MAX;
    app.rot[i].t = t, app.rot[i].rate = rate;
    while (app.rn > 1 && t - app.rot[app.rfirst].t > ROT_US)
        app.rfirst = (app.rfirst + 1) % WINDOW_MAX, app.rn--;
    app.print_rot = MAX(app.print_rot, rate);
}

/* How many degrees the phone turned between "from" and "to", or -1 if the gyroscope
 * gave nothing for that time (none, or not started). Like Motorola's
 * max_gyro_rotation: a chop moves the phone along a line, a flick of the wrist turns it. */
static double turned(guint64 from, guint64 to)
{
    double deg = 0;
    int n = 0;
    for (int i = 1; i < app.rn; i++) {
        int k = (app.rfirst + i) % WINDOW_MAX, j = (app.rfirst + i - 1) % WINDOW_MAX;
        if (app.rot[k].t < from || app.rot[k].t > to)
            continue;
        guint64 dt = MIN(app.rot[k].t - app.rot[j].t, (guint64)50000);  /* gaps: at most 50 ms */
        deg += app.rot[k].rate * dt / 1e6;
        n++;
    }
    return n >= 3 ? deg : -1;
}

static void on_reading(guint64 t, double x, double y, double z)
{
    if (app.record)
        /* the reading's time, x, y, z, and when it arrived here (monotonic, us) */
        fprintf(app.record, "%" G_GUINT64_FORMAT " %.1f %.1f %.1f %" G_GINT64_FORMAT "\n",
                t, x, y, z, app.replay ? 0 : g_get_monotonic_time());
    x /= 1000.0, y /= 1000.0, z /= 1000.0;  /* milli-g to g */

    /* Gravity: the average of the last second. */
    if (app.gn == WINDOW_MAX) {
        app.gsx -= app.grav[app.gfirst].x, app.gsy -= app.grav[app.gfirst].y,
        app.gsz -= app.grav[app.gfirst].z;
        app.gfirst = (app.gfirst + 1) % WINDOW_MAX, app.gn--;
    }
    int gi = (app.gfirst + app.gn++) % WINDOW_MAX;
    app.grav[gi].t = t, app.grav[gi].x = x, app.grav[gi].y = y, app.grav[gi].z = z;
    app.gsx += x, app.gsy += y, app.gsz += z;
    while (app.gn > 1 && t - app.grav[app.gfirst].t > GRAVITY_US) {
        app.gsx -= app.grav[app.gfirst].x, app.gsy -= app.grav[app.gfirst].y,
        app.gsz -= app.grav[app.gfirst].z;
        app.gfirst = (app.gfirst + 1) % WINDOW_MAX, app.gn--;
    }
    double mx = x - app.gsx / app.gn, my = y - app.gsy / app.gn, mz = z - app.gsz / app.gn;
    double m = sqrt(mx * mx + my * my + mz * mz);

    if (app.monitor && !app.replay) {
        app.print_max = MAX(app.print_max, m);
        if (t - app.last_print >= 250000) {
            g_autofree char *bars = g_strnfill(MIN((int)(app.print_max * 10), 60), '#');
            g_print("%5.2f g %4.0f deg/s %s%s\n", app.print_max, app.print_rot,
                    app.print_max >= app.threshold_g ? "*" : " ", bars);
            app.last_print = t;
            app.print_max = app.print_rot = 0;
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

    if (!app.armed) {
        /* Wait for calm, then start afresh. */
        if (m >= CALM_G)
            app.calm_since = 0;
        else if (!app.calm_since)
            app.calm_since = t;
        gboolean calm = app.calm_since && t - app.calm_since >= CALM_US;
        if (t >= app.quiet_until && (calm || t - app.switched_at >= REARM_US)) {
            app.armed = TRUE;
            app.mn = app.mfirst = 0;
        }
        return;
    }

    /* Movement of the last 0.6 s. */
    if (app.mn == WINDOW_MAX)
        app.mfirst = (app.mfirst + 1) % WINDOW_MAX, app.mn--;
    int mi = (app.mfirst + app.mn++) % WINDOW_MAX;
    app.mov[mi].t = t, app.mov[mi].x = mx, app.mov[mi].y = my, app.mov[mi].z = mz, app.mov[mi].m = m;
    while (app.mn > 1 && t - app.mov[app.mfirst].t > SWING_US)
        app.mfirst = (app.mfirst + 1) % WINDOW_MAX, app.mn--;

    /* The strongest, and the strongest the other way along the same line. */
    int best = app.mfirst;
    for (int i = 0; i < app.mn; i++) {
        int k = (app.mfirst + i) % WINDOW_MAX;
        if (app.mov[k].m > app.mov[best].m)
            best = k;
    }
    double peak = app.mov[best].m;
    if (peak < app.threshold_g)
        return;
    double ux = app.mov[best].x / peak, uy = app.mov[best].y / peak, uz = app.mov[best].z / peak;
    double back = 0;
    guint64 at = app.mov[best].t;
    for (int i = 0; i < app.mn; i++) {
        int k = (app.mfirst + i) % WINDOW_MAX;
        guint64 dt = app.mov[k].t > at ? app.mov[k].t - at : at - app.mov[k].t;
        if (dt <= BACK_US)
            back = MAX(back, -(app.mov[k].x * ux + app.mov[k].y * uy + app.mov[k].z * uz));
    }
    if (back < app.threshold_g * BACK)
        return;
    double deg = turned(at > CHOP_US ? at - CHOP_US : 0, t);
    if (app.max_rotation > 0 && deg > app.max_rotation) {
        /* A turn, not a chop. Start over with the next swing. */
        if (app.replay)
            g_print("%8.2f s  ignored: turned %.0f deg\n", t / 1e6, deg);
        else
            g_message("Shake ignored: the phone turned %.0f degrees", deg);
        app.mn = app.mfirst = 0;
        return;
    }
    if (app.replay && deg >= 0)
        g_print("%8.2f s  (turned %.0f deg)\n", t / 1e6, deg);
    else if (!app.replay && deg >= 0)
        g_message("Shake: the phone turned %.0f degrees", deg);
    shaken(t);
}

/* Sensor sessions. The accelerometer is required; the gyroscope is used when the phone
 * has one and only adds the rotation check. Both use the same protocol and reading
 * layout (sensorfwd's TimedXyzData). */

static void stop_sensors(void);
static void start_sensors(void);

/* Shaking turned off: let go of the sensors, unless tuning. */
static gboolean wants_sensor(void)
{
    return app.enabled || app.monitor || app.record;
}

static gboolean retry_sensor(gpointer _)
{
    app.retry = 0;
    if (app.acc.sid < 0 && app.sensor_up && wants_sensor())
        start_sensors();
    return G_SOURCE_REMOVE;
}

static void restart_later(void)
{
    stop_sensors();
    if (!app.retry)
        app.retry = g_timeout_add_seconds(5, retry_sensor, NULL);
}

static gboolean on_socket(int fd, GIOCondition cond, gpointer data)
{
    struct Session *ss = data;
    guint8 chunk[4096];
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return G_SOURCE_CONTINUE;
    if (n <= 0) {
        g_warning("%s connection closed", ss->what);
        ss->fd_source = 0;  /* removed by returning */
        restart_later();
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(ss->buf, chunk, n);
    guint off = 0;
    if (!ss->got_tag) {
        ss->got_tag = TRUE;  /* the '\n' sensorfwd sends first */
        off = 1;
    }
    for (;;) {
        guint32 count;
        if (ss->buf->len - off < 4)
            break;
        memcpy(&count, ss->buf->data + off, 4);
        if (count == 0 || count > 1000) {
            g_warning("Unexpected %s data (count %u); reconnecting", ss->what, count);
            ss->fd_source = 0;
            restart_later();
            return G_SOURCE_REMOVE;
        }
        if (ss->buf->len - off < 4 + count * READING_SIZE)
            break;
        for (guint32 i = 0; i < count; i++) {
            const guint8 *p = ss->buf->data + off + 4 + i * READING_SIZE;
            guint64 t;
            float xyz[3];
            memcpy(&t, p, 8);
            memcpy(xyz, p + 8, 12);
            ss->reading(t, xyz[0], xyz[1], xyz[2]);
        }
        off += 4 + count * READING_SIZE;
    }
    g_byte_array_remove_range(ss->buf, 0, off);
    return G_SOURCE_CONTINUE;
}

static gboolean sensor_call(struct Session *ss, const char *method, GVariant *args)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = call(app.system, SENSOR_SERVICE, ss->path, ss->iface,
                                 method, args, NULL, &e);
    if (!r)
        g_warning("%s %s: %s", ss->what, method, e->message);
    return r != NULL;
}

static void stop_session(struct Session *ss)
{
    if (ss->fd_source)
        g_source_remove(ss->fd_source);
    ss->fd_source = 0;
    if (ss->fd >= 0)
        close(ss->fd);
    ss->fd = -1;
    if (ss->sid >= 0 && app.system) {
        g_autoptr(GVariant) r1 = call(app.system, SENSOR_SERVICE, ss->path, ss->iface, "stop",
                                      g_variant_new("(i)", ss->sid), NULL, NULL);
        g_autoptr(GVariant) r2 = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager",
                                      "releaseSensor",
                                      g_variant_new("(six)", ss->id, ss->sid, (gint64)getpid()),
                                      NULL, NULL);
    }
    ss->sid = -1;
}

/* Returns FALSE if the sensor isn't there or the session failed. */
static gboolean start_session(struct Session *ss, gboolean quiet)
{
    g_autoptr(GError) e = NULL;
    g_autoptr(GVariant) r = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager",
                                 "loadPlugin", g_variant_new("(s)", ss->id), "(b)", &e);
    gboolean ok = FALSE;
    if (r)
        g_variant_get(r, "(b)", &ok);
    if (!ok) {
        if (!quiet)
            g_warning("sensorfwd has no %s: %s", ss->what, e ? e->message : "loadPlugin failed");
        return FALSE;
    }
    g_clear_pointer(&r, g_variant_unref);
    r = call(app.system, SENSOR_SERVICE, SENSOR_MANAGER, "local.SensorManager", "requestSensor",
             g_variant_new("(sx)", ss->id, (gint64)getpid()), "(i)", &e);
    if (!r || (g_variant_get(r, "(i)", &ss->sid), ss->sid < 0)) {
        g_warning("requestSensor %s: %s", ss->id, e ? e->message : "no session");
        ss->sid = -1;
        return FALSE;
    }

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    g_strlcpy(sa.sun_path, SENSOR_SOCKET, sizeof sa.sun_path);
    ss->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    gint32 sid = ss->sid;
    if (ss->fd < 0 || connect(ss->fd, (struct sockaddr *)&sa, sizeof sa) < 0
        || write(ss->fd, &sid, sizeof sid) != sizeof sid) {
        g_warning("Can't connect to %s: %s", SENSOR_SOCKET, g_strerror(errno));
        stop_session(ss);
        return FALSE;
    }
    g_unix_set_fd_nonblocking(ss->fd, TRUE, NULL);
    ss->got_tag = FALSE;
    g_byte_array_set_size(ss->buf, 0);
    ss->fd_source = g_unix_fd_add(ss->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_socket, ss);

    sensor_call(ss, "setInterval", g_variant_new("(ii)", ss->sid, INTERVAL_MS));
    sensor_call(ss, "setStandbyOverride", g_variant_new("(ib)", ss->sid, app.screen_off));
    if (!sensor_call(ss, "start", g_variant_new("(i)", ss->sid))) {
        stop_session(ss);
        return FALSE;
    }
    return TRUE;
}

static void start_sensors(void)
{
    if (!start_session(&app.acc, FALSE)) {
        restart_later();
        return;
    }
    /* The gyroscope only when the check is on (or when tuning). */
    gboolean gyro = (app.max_rotation > 0 || app.monitor || app.record)
                    && start_session(&app.gyr, TRUE);
    reset_detector();
    g_message("Watching the accelerometer (session %d; swings of %.2f g%s)%s",
              app.acc.sid, app.threshold_g,
              app.screen_off ? "; also with the screen off" : "",
              gyro ? " and the gyroscope" : app.max_rotation > 0 ? "; no gyroscope, so no rotation check" : "");
}

static void stop_sensors(void)
{
    stop_session(&app.gyr);
    stop_session(&app.acc);
}

static void forget_session(struct Session *ss)
{
    if (ss->fd_source)
        g_source_remove(ss->fd_source);
    ss->fd_source = 0;
    if (ss->fd >= 0)
        close(ss->fd);
    ss->fd = -1;
    ss->sid = -1;  /* its sessions are gone with sensorfwd */
}

static void sensor_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer _)
{
    app.sensor_up = TRUE;
    stop_sensors();
    if (wants_sensor())
        start_sensors();
}

static void sensor_vanished(GDBusConnection *c, const char *name, gpointer _)
{
    app.sensor_up = FALSE;
    if (app.acc.sid >= 0)
        g_message("sensorfwd went away; waiting for it to come back");
    forget_session(&app.acc);
    forget_session(&app.gyr);
}

static gboolean on_quit(gpointer _)
{
    g_main_loop_quit(app.loop);
    return G_SOURCE_CONTINUE;
}

static gboolean on_reload(gpointer _)
{
    gboolean screen_off = app.screen_off;
    gboolean gyro_wanted = app.max_rotation > 0;
    load_settings();
    g_message("Settings reloaded (swings of %.2f g, turning at most %.0f degrees)",
              app.threshold_g, app.max_rotation);
    if (app.acc.sid >= 0 && (!wants_sensor() || screen_off != app.screen_off
                             || gyro_wanted != (app.max_rotation > 0))) {
        stop_sensors();
        if (!wants_sensor())
            g_message("Shaking turned off; sensors released");
    }
    if (app.acc.sid < 0 && app.sensor_up && wants_sensor())
        start_sensors();
    return G_SOURCE_CONTINUE;
}

/* Settings over D-Bus, for the Sailfish settings page: QML there can't write files or
 * the vibrator. Session bus, local.ShakeTorch at /local/ShakeTorch.
 *   GetSettings() -> a{sv}            the current settings, and "running"
 *   Set(s key, s value)               change one setting; saved to the config file
 *   TorchOff()                        turn the torch off (the notification's tap)
 *   TryBuzz(b on, i first, i second, i gap, i off)
 *                                     buzz as for torch on or off, with these lengths */
#define BUS_NAME "local.ShakeTorch"
#define BUS_PATH "/local/ShakeTorch"

static const char introspection[] =
    "<node><interface name='" BUS_NAME "'>"
    "<method name='GetSettings'><arg type='a{sv}' direction='out'/></method>"
    "<method name='Set'><arg type='s' name='key' direction='in'/>"
    "<arg type='s' name='value' direction='in'/></method>"
    "<method name='TorchOff'/>"
    "<method name='TryBuzz'><arg type='b' name='on' direction='in'/>"
    "<arg type='i' name='on_ms' direction='in'/><arg type='i' name='second_ms' direction='in'/>"
    "<arg type='i' name='gap_ms' direction='in'/>"
    "<arg type='i' name='off_ms' direction='in'/></method>"
    "</interface></node>";

static const struct { const char *key; char type; } setting_keys[] = {
    { "enabled", 'b' }, { "threshold", 'd' }, { "cooldown-ms", 'i' }, { "screen-off", 'b' },
    { "vibrate", 'b' }, { "buzz-on-ms", 'i' }, { "buzz-second-ms", 'i' }, { "buzz-gap-ms", 'i' }, { "buzz-off-ms", 'i' },
    { "log-movement", 'b' }, { "max-rotation", 'd' }, { "notify", 'b' },
};

/* Check the value and write it in the config file's form; FALSE if it isn't valid. */
static gboolean save_setting(const char *key, const char *value, GError **error)
{
    char type = 0;
    for (guint i = 0; i < G_N_ELEMENTS(setting_keys); i++)
        if (g_str_equal(key, setting_keys[i].key))
            type = setting_keys[i].type;
    if (!type) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unknown setting %s", key);
        return FALSE;
    }
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    const char *out = NULL;
    if (type == 'b') {
        if (g_str_equal(value, "true") || g_str_equal(value, "false"))
            out = value;
    } else if (type == 'i') {
        gint64 v;
        if (g_ascii_string_to_signed(value, 10, 0, 100000, &v, NULL))
            out = value;
    } else {
        char *end;
        double v = g_ascii_strtod(value, &end);
        if (*value && !*end && isfinite(v))
            out = g_ascii_formatd(num, sizeof num, "%.6g", v);
    }
    if (!out) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Bad value for %s: %s", key, value);
        return FALSE;
    }
    g_autofree char *path = config_path();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_value(kf, "shake-torch", key, out);
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    return g_key_file_save_to_file(kf, path, error);
}

static void on_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer _)
{
    if (g_str_equal(method, "GetSettings")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(app.enabled));
        g_variant_builder_add(&b, "{sv}", "threshold", g_variant_new_double(app.threshold_g));
        g_variant_builder_add(&b, "{sv}", "cooldown-ms", g_variant_new_int32(app.cooldown_ms));
        g_variant_builder_add(&b, "{sv}", "screen-off", g_variant_new_boolean(app.screen_off));
        g_variant_builder_add(&b, "{sv}", "vibrate", g_variant_new_boolean(app.vibrate));
        g_variant_builder_add(&b, "{sv}", "buzz-on-ms", g_variant_new_int32(app.buzz_on_ms));
        g_variant_builder_add(&b, "{sv}", "buzz-second-ms", g_variant_new_int32(app.buzz_second_ms));
        g_variant_builder_add(&b, "{sv}", "buzz-gap-ms", g_variant_new_int32(app.buzz_gap_ms));
        g_variant_builder_add(&b, "{sv}", "buzz-off-ms", g_variant_new_int32(app.buzz_off_ms));
        g_variant_builder_add(&b, "{sv}", "max-rotation", g_variant_new_double(app.max_rotation));
        g_variant_builder_add(&b, "{sv}", "notify", g_variant_new_boolean(app.notify));
        g_variant_builder_add(&b, "{sv}", "running", g_variant_new_boolean(app.acc.sid >= 0));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv})", &b));
    } else if (g_str_equal(method, "Set")) {
        const char *key, *value;
        g_variant_get(args, "(&s&s)", &key, &value);
        GError *e = NULL;
        if (!save_setting(key, value, &e)) {
            g_dbus_method_invocation_take_error(inv, e);
            return;
        }
        on_reload(NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_str_equal(method, "TorchOff")) {
        torch_off();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_str_equal(method, "TryBuzz")) {
        gboolean on;
        gint32 on_ms, second_ms, gap_ms, off_ms;
        g_variant_get(args, "(biiii)", &on, &on_ms, &second_ms, &gap_ms, &off_ms);
        buzz_pattern(on, CLAMP(on_ms, 10, 2000), CLAMP(second_ms, 10, 2000),
                     CLAMP(gap_ms, 50, 2000), CLAMP(off_ms, 10, 2000));
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static void export_settings(void)
{
    static const GDBusInterfaceVTable vtable = { on_method, NULL, NULL, { 0 } };
    g_autoptr(GError) e = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(introspection, NULL);
    if (!g_dbus_connection_register_object(app.session, BUS_PATH, node->interfaces[0], &vtable,
                                           NULL, NULL, &e)) {
        g_warning("Can't offer settings over D-Bus: %s", e->message);
        return;
    }
    g_bus_own_name_on_connection(app.session, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 NULL, NULL, NULL, NULL);
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
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'g') {
            if (sscanf(line + 1, "%" G_GUINT64_FORMAT " %lf %lf %lf", &t, &x, &y, &z) == 4)
                on_gyro(t, x, y, z);
            continue;
        }
        if (sscanf(line, "%" G_GUINT64_FORMAT " %lf %lf %lf", &t, &x, &y, &z) != 4)
            continue;
        on_reading(t, x, y, z);
        count++;
    }
    fclose(f);
    g_print("%d readings\n", count);
    return 0;
}

static int toggle_waits;

static gboolean toggle_step(gpointer _)
{
    if (--toggle_waits == 0)
        g_main_loop_quit(app.loop);
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
    reset_detector();
    if (replay_file)
        return replay(replay_file);
    app.acc = (struct Session){ "accelerometersensor", "/SensorManager/accelerometersensor",
                                "local.AccelerometerSensor", "accelerometer", on_reading, -1, -1, 0,
                                g_byte_array_new(), FALSE };
    app.gyr = (struct Session){ "gyroscopesensor", "/SensorManager/gyroscopesensor",
                                "local.GyroscopeSensor", "gyroscope", on_gyro, -1, -1, 0,
                                g_byte_array_new(), FALSE };
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
        int was = torch_state();
        if (was < 0)
            return 1;
        g_print("Torch %s\n", was ? "off" : "on");
        vibrate(!was);
        /* Wait for the torch service, and for the buzz to finish. */
        app.loop = g_main_loop_new(NULL, FALSE);
        toggle_waits = 2;
        set_torch(!was, toggle_step);
        g_timeout_add(was ? app.buzz_off_ms : app.buzz_on_ms + app.buzz_gap_ms + app.buzz_second_ms,
                      toggle_step, NULL);
        g_main_loop_run(app.loop);
        return 0;
    }
    app.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!app.system) {
        g_printerr("No system bus: %s\n", e->message);
        return 1;
    }

    app.loop = g_main_loop_new(NULL, FALSE);
    if (!app.monitor && !app.record) {
        export_settings();
        watch_notifications();
    }
    g_unix_signal_add(SIGINT, on_quit, NULL);
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGHUP, on_reload, NULL);
    app.watch = g_bus_watch_name_on_connection(app.system, SENSOR_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               sensor_appeared, sensor_vanished, NULL, NULL);
    g_main_loop_run(app.loop);
    g_bus_unwatch_name(app.watch);
    stop_sensors();
    close_notification();
    g_dbus_connection_flush_sync(app.session, NULL, NULL);
    if (app.record)
        fclose(app.record);
    return 0;
}
