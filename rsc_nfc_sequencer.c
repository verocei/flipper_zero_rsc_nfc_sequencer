/*
 * RSC NFC Sequencer  (v0.2)
 * ------------------------------------------------------------------
 * Cycles through saved .nfc tags and emulates each one for a settable
 * interval, so an access-control reader can be tested unattended.
 *
 * UI follows the usual Flipper pattern: the app opens on a menu
 * (Start / Settings / About) with the app name as the menu header.
 * All parameters live on the Settings page (a scrolling list); the
 * emulation screen only shows status and the current tag, with the
 * tag name (or UID) in a large font.
 *
 * Playback modes:
 *   SEQ  - directory order (as returned by storage)
 *   ABC  - alphabetical by file name
 *   RND  - random pick each step
 *
 * Tags live in:  /ext/apps_data/rsc_nfc_sequencer/tags/ (one .nfc file per tag)
 * Settings persist in: /ext/apps_data/rsc_nfc_sequencer/settings.bin
 *
 * Target firmware: stock (OFW) and Momentum. Uses only the shared OFW
 * NFC API, so one build runs on both via ufbt.
 *
 * NOTE ON SCOPE: emulation is only guaranteed faithful for UID-based
 * readers. Full MIFARE Classic content emulation is reader-dependent
 * on the Flipper and outside this app's intended use.
 */

#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>

#define TAG "RscNfcSeq"

#define APP_NAME "RSC NFC Sequencer"

#define BASE_DIR      EXT_PATH("apps_data/rsc_nfc_sequencer")
#define TAGS_DIR      EXT_PATH("apps_data/rsc_nfc_sequencer/tags")
#define SETTINGS_PATH EXT_PATH("apps_data/rsc_nfc_sequencer/settings.bin")
#define NFC_EXT       ".nfc"

#define MAX_TAGS 64
#define NAME_LEN 64
#define PATH_LEN 256

#define INTERVAL_MIN     1
#define INTERVAL_MAX     60
#define INTERVAL_DEFAULT 3

#define SETTINGS_MAGIC   0x52 // 'R'
#define SETTINGS_VERSION 1

typedef enum {
    ModeSequential = 0,
    ModeAlphabetical,
    ModeRandom,
    ModeCount,
} PlayMode;

static const char* const mode_label[ModeCount] = {"SEQ", "ABC", "RND"};

typedef enum {
    ViewIdMenu = 0,
    ViewIdSettings,
    ViewIdEmu,
    ViewIdAbout,
} ViewId;

typedef enum {
    MenuIndexStart = 0,
    MenuIndexSettings,
    MenuIndexAbout,
} MenuIndex;

typedef enum {
    CustomEventTick = 1,
} CustomEvent;

typedef struct App App;

// The emulation view's model only needs a back-reference; all state lives
// in App and is only ever touched on the view-dispatcher thread.
typedef struct {
    App* app;
} EmuModel;

struct App {
    // infra
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    VariableItemList* settings_list;
    View* emu_view;
    Widget* about;
    Storage* storage;
    NotificationApp* notif;
    FuriTimer* timer; // 1 Hz tick while running

    VariableItem* item_mode;
    VariableItem* item_delay;

    // nfc
    Nfc* nfc; // allocated while running
    NfcDevice* device; // current tag
    NfcListener* listener; // current tag

    // tag library
    char names[MAX_TAGS][NAME_LEN];
    uint8_t tag_count;

    // settings
    PlayMode mode;
    uint8_t interval; // seconds

    // runtime state
    bool running;
    int current; // index into names[]
    uint8_t remaining; // seconds until next step
    bool last_load_ok;
    char uid_str[48]; // current tag UID as hex, for display
    ViewId current_view;
};

/* ---------------- settings persistence ---------------- */

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t interval;
} SettingsBlob;

static void settings_load(App* app) {
    app->mode = ModeSequential;
    app->interval = INTERVAL_DEFAULT;

    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        SettingsBlob blob;
        if(storage_file_read(f, &blob, sizeof(blob)) == sizeof(blob) &&
           blob.magic == SETTINGS_MAGIC && blob.version == SETTINGS_VERSION) {
            if(blob.mode < ModeCount) app->mode = blob.mode;
            if(blob.interval >= INTERVAL_MIN && blob.interval <= INTERVAL_MAX)
                app->interval = blob.interval;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}

static void settings_save(App* app) {
    SettingsBlob blob = {
        .magic = SETTINGS_MAGIC,
        .version = SETTINGS_VERSION,
        .mode = (uint8_t)app->mode,
        .interval = app->interval,
    };
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, &blob, sizeof(blob));
    }
    storage_file_close(f);
    storage_file_free(f);
}

/* ---------------- tag library ---------------- */

static void tags_scan(App* app) {
    app->tag_count = 0;

    storage_common_mkdir(app->storage, BASE_DIR);
    storage_common_mkdir(app->storage, TAGS_DIR);

    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, TAGS_DIR)) {
        FileInfo info;
        char name[NAME_LEN];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            size_t len = strlen(name);
            size_t ext = strlen(NFC_EXT);
            if(len <= ext) continue;
            if(strcasecmp(name + (len - ext), NFC_EXT) != 0) continue;
            if(app->tag_count >= MAX_TAGS) break;
            strncpy(app->names[app->tag_count], name, NAME_LEN - 1);
            app->names[app->tag_count][NAME_LEN - 1] = '\0';
            app->tag_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
}

// Insertion sort: qsort is disabled in the firmware API, and MAX_TAGS is small.
static void tags_sort_alpha(App* app) {
    char tmp[NAME_LEN];
    for(int i = 1; i < app->tag_count; i++) {
        memcpy(tmp, app->names[i], NAME_LEN);
        int j = i - 1;
        while(j >= 0 && strncmp(app->names[j], tmp, NAME_LEN) > 0) {
            memcpy(app->names[j + 1], app->names[j], NAME_LEN);
            j--;
        }
        memcpy(app->names[j + 1], tmp, NAME_LEN);
    }
}

// Copy the display name of tag `idx` (file name without the .nfc extension).
static void tag_display_name(App* app, int idx, char* out, size_t out_len) {
    out[0] = '\0';
    if(idx < 0 || idx >= app->tag_count) return;
    strncpy(out, app->names[idx], out_len - 1);
    out[out_len - 1] = '\0';
    size_t len = strlen(out);
    size_t ext = strlen(NFC_EXT);
    if(len > ext && strcasecmp(out + (len - ext), NFC_EXT) == 0) out[len - ext] = '\0';
}

/* ---------------- emulation core ---------------- */

static void emu_stop(App* app) {
    if(app->listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
    }
    if(app->device) {
        nfc_device_free(app->device);
        app->device = NULL;
    }
    app->uid_str[0] = '\0';
}

static void uid_to_str(const uint8_t* uid, size_t len, char* out, size_t out_len) {
    out[0] = '\0';
    size_t pos = 0;
    for(size_t i = 0; i < len && pos + 3 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, (i == 0) ? "%02X" : " %02X", uid[i]);
    }
}

// Load tag at index `idx` and start emulating it. Returns true on success.
static bool emu_start_index(App* app, int idx) {
    emu_stop(app);
    if(idx < 0 || idx >= app->tag_count) return false;

    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", TAGS_DIR, app->names[idx]);

    app->device = nfc_device_alloc();
    if(!nfc_device_load(app->device, path)) {
        FURI_LOG_W(TAG, "load failed: %s", path);
        nfc_device_free(app->device);
        app->device = NULL;
        return false;
    }

    NfcProtocol protocol = nfc_device_get_protocol(app->device);
    if(protocol == NfcProtocolInvalid) {
        FURI_LOG_W(TAG, "invalid protocol: %s", path);
        nfc_device_free(app->device);
        app->device = NULL;
        return false;
    }

    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
    if(uid && uid_len) uid_to_str(uid, uid_len, app->uid_str, sizeof(app->uid_str));

    app->listener =
        nfc_listener_alloc(app->nfc, protocol, nfc_device_get_data(app->device, protocol));
    // NULL callback: we only need the reader to see the tag, not react to events.
    nfc_listener_start(app->listener, NULL, NULL);

    if(app->notif) notification_message(app->notif, &sequence_blink_blue_10);
    return true;
}

// Pick the next index according to the current mode.
static int next_index(App* app, int from) {
    if(app->tag_count == 0) return -1;
    switch(app->mode) {
    case ModeRandom:
        if(app->tag_count == 1) return 0;
        return (int)(furi_hal_random_get() % app->tag_count);
    case ModeSequential:
    case ModeAlphabetical:
    default: {
        int n = from + 1;
        if(n >= app->tag_count) n = 0;
        return n;
    }
    }
}

static void run_start(App* app) {
    if(app->tag_count == 0) return;

    // SEQ follows directory order; re-scan so a previous ABC sort doesn't stick.
    tags_scan(app);
    if(app->mode == ModeAlphabetical) tags_sort_alpha(app);

    app->nfc = nfc_alloc();
    app->current = (app->mode == ModeRandom) ? next_index(app, -1) : 0;
    app->last_load_ok = emu_start_index(app, app->current);
    app->remaining = app->interval;
    app->running = true;
    furi_timer_start(app->timer, furi_kernel_get_tick_frequency()); // 1 Hz
}

static void run_stop(App* app) {
    furi_timer_stop(app->timer);
    emu_stop(app);
    if(app->nfc) {
        nfc_free(app->nfc);
        app->nfc = NULL;
    }
    app->running = false;
}

// Called once per second (via a custom event) while running.
static void run_tick(App* app) {
    if(!app->running) return;
    if(app->remaining > 0) app->remaining--;
    if(app->remaining == 0) {
        app->current = next_index(app, app->current);
        app->last_load_ok = emu_start_index(app, app->current);
        app->remaining = app->interval;
    }
}

/* ---------------- emulation view ---------------- */

// Draw a string centered horizontally, truncated with "…" if too wide.
static void draw_centered_trunc(Canvas* canvas, int y, const char* text) {
    char buf[NAME_LEN + 2];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while(strlen(buf) > 1 && canvas_string_width(canvas, buf) > 126) {
        size_t l = strlen(buf);
        buf[l - 1] = '\0';
    }
    int w = canvas_string_width(canvas, buf);
    int x = (128 - w) / 2;
    if(x < 1) x = 1;
    canvas_draw_str(canvas, x, y, buf);
}

static void emu_draw_cb(Canvas* canvas, void* model) {
    EmuModel* m = model;
    App* app = m->app;
    canvas_clear(canvas);

    if(app->tag_count == 0) {
        canvas_set_font(canvas, FontPrimary);
        draw_centered_trunc(canvas, 30, "No tags");
        canvas_set_font(canvas, FontSecondary);
        draw_centered_trunc(canvas, 44, "Put .nfc in tags folder");
        canvas_draw_str(canvas, 2, 62, "Back = menu");
        return;
    }

    // Top line: mode, delay and position — small.
    canvas_set_font(canvas, FontSecondary);
    char top[32];
    snprintf(
        top,
        sizeof(top),
        "%s  %ds  %d/%d",
        mode_label[app->mode],
        app->interval,
        app->current + 1,
        app->tag_count);
    canvas_draw_str(canvas, 2, 10, top);
    canvas_draw_line(canvas, 0, 13, 128, 13);

    // Current tag name — large.
    char name[NAME_LEN];
    tag_display_name(app, app->current, name, sizeof(name));
    canvas_set_font(canvas, FontPrimary);
    draw_centered_trunc(canvas, 32, app->last_load_ok ? name : "!! load failed");

    // UID — small, under the name.
    canvas_set_font(canvas, FontSecondary);
    if(app->last_load_ok && app->uid_str[0]) {
        char uid[56];
        snprintf(uid, sizeof(uid), "UID: %s", app->uid_str);
        draw_centered_trunc(canvas, 45, uid);
    } else if(!app->last_load_ok) {
        draw_centered_trunc(canvas, 45, name);
    }

    // Bottom: countdown and hint.
    char bottom[32];
    snprintf(bottom, sizeof(bottom), "next %ds", app->remaining);
    canvas_draw_str(canvas, 2, 62, bottom);
    canvas_draw_str(canvas, 74, 62, "OK/Back=stop");
}

static void switch_view(App* app, ViewId id) {
    app->current_view = id;
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

static bool emu_input_cb(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) {
        run_stop(app);
        switch_view(app, ViewIdMenu);
        return true;
    }
    // Swallow the other keys so nothing adjusts from here.
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) return true;
    return false;
}

/* ---------------- menu / settings / about ---------------- */

static void menu_cb(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case MenuIndexStart:
        run_start(app);
        switch_view(app, ViewIdEmu);
        break;
    case MenuIndexSettings:
        switch_view(app, ViewIdSettings);
        break;
    case MenuIndexAbout:
        switch_view(app, ViewIdAbout);
        break;
    default:
        break;
    }
}

static void mode_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= ModeCount) idx = 0;
    app->mode = (PlayMode)idx;
    variable_item_set_current_value_text(item, mode_label[idx]);
}

static void delay_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item); // 0..59
    app->interval = idx + INTERVAL_MIN;
    char text[8];
    snprintf(text, sizeof(text), "%d s", app->interval);
    variable_item_set_current_value_text(item, text);
}

static bool navigation_cb(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewIdEmu:
        run_stop(app);
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdSettings:
        settings_save(app);
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdAbout:
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdMenu:
    default:
        return false; // exit the app
    }
}

static bool custom_event_cb(void* context, uint32_t event) {
    App* app = context;
    if(event == CustomEventTick) {
        run_tick(app);
        // Force an emulation-view redraw.
        with_view_model(app->emu_view, EmuModel * m, { UNUSED(m); }, true);
        return true;
    }
    return false;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventTick);
}

/* ---------------- app lifecycle ---------------- */

static void build_settings_list(App* app) {
    app->settings_list = variable_item_list_alloc();
    variable_item_list_set_header(app->settings_list, "Settings");

    app->item_mode =
        variable_item_list_add(app->settings_list, "Mode", ModeCount, mode_changed_cb, app);
    variable_item_set_current_value_index(app->item_mode, app->mode);
    variable_item_set_current_value_text(app->item_mode, mode_label[app->mode]);

    app->item_delay = variable_item_list_add(
        app->settings_list, "Delay", INTERVAL_MAX - INTERVAL_MIN + 1, delay_changed_cb, app);
    variable_item_set_current_value_index(app->item_delay, app->interval - INTERVAL_MIN);
    char text[8];
    snprintf(text, sizeof(text), "%d s", app->interval);
    variable_item_set_current_value_text(app->item_delay, text);
}

static void build_about(App* app) {
    app->about = widget_alloc();
    widget_add_string_element(
        app->about, 64, 4, AlignCenter, AlignTop, FontPrimary, APP_NAME);
    widget_add_string_multiline_element(
        app->about,
        2,
        20,
        AlignLeft,
        AlignTop,
        FontSecondary,
        "Cycles saved .nfc tags,\n"
        "emulating each in turn to\n"
        "test UID access readers.\n"
        "v0.2  Athos / RSC");
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->mode = ModeSequential;
    app->interval = INTERVAL_DEFAULT;
    app->current = -1;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    settings_load(app);
    tags_scan(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_cb);

    // Menu
    app->menu = submenu_alloc();
    submenu_set_header(app->menu, APP_NAME);
    submenu_add_item(app->menu, "Start", MenuIndexStart, menu_cb, app);
    submenu_add_item(app->menu, "Settings", MenuIndexSettings, menu_cb, app);
    submenu_add_item(app->menu, "About", MenuIndexAbout, menu_cb, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, submenu_get_view(app->menu));

    // Settings
    build_settings_list(app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdSettings, variable_item_list_get_view(app->settings_list));

    // Emulation view
    app->emu_view = view_alloc();
    view_set_context(app->emu_view, app);
    view_allocate_model(app->emu_view, ViewModelTypeLocking, sizeof(EmuModel));
    with_view_model(app->emu_view, EmuModel * m, { m->app = app; }, false);
    view_set_draw_callback(app->emu_view, emu_draw_cb);
    view_set_input_callback(app->emu_view, emu_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdEmu, app->emu_view);

    // About
    build_about(app);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdAbout, widget_get_view(app->about));

    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->current_view = ViewIdMenu;
    return app;
}

static void app_free(App* app) {
    if(app->running) run_stop(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdEmu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdAbout);

    submenu_free(app->menu);
    variable_item_list_free(app->settings_list);
    view_free(app->emu_view);
    widget_free(app->about);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_timer_free(app->timer);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);

    free(app);
}

int32_t rsc_nfc_sequencer_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_run(app->view_dispatcher);

    app_free(app);
    return 0;
}
