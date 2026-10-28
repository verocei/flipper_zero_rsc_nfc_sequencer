/*
 * RSC NFC Sequencer  (v0.1)
 * ------------------------------------------------------------------
 * Cycles through saved .nfc tags and emulates each one for a settable
 * interval, so an access-control reader can be tested unattended.
 *
 * Playback modes:
 *   SEQ  - directory order (as returned by storage)
 *   ABC  - alphabetical by file name
 *   RND  - random pick each step
 *
 * Tags live in:  /ext/apps_data/rsc_nfc_sequencer/tags/ (one .nfc file per tag)
 * (create them with the built-in NFC app, then move/copy the files there)
 *
 * Target firmware: stock (OFW) and Momentum. Uses only the shared OFW
 * NFC API, so one build runs on both via ufbt.
 *
 * NOTE ON SCOPE: emulation is only guaranteed faithful for UID-based
 * readers. Full MIFARE Classic content emulation is reader-dependent on
 * the Flipper and outside this app's intended use.
 */

#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>

#define TAG "RscNfcSeq"

#define TAGS_DIR EXT_PATH("apps_data/rsc_nfc_sequencer/tags")
#define NFC_EXT  ".nfc"

#define MAX_TAGS    64
#define NAME_LEN    64
#define PATH_LEN    256

#define INTERVAL_MIN 1
#define INTERVAL_MAX 60
#define INTERVAL_DEFAULT 3

typedef enum {
    ModeSequential = 0,
    ModeAlphabetical,
    ModeRandom,
    ModeCount,
} PlayMode;

static const char* const mode_label[ModeCount] = {"SEQ", "ABC", "RND"};

typedef enum {
    EventTypeInput,
    EventTypeTick, // 1 Hz
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    // infra
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriTimer* timer; // 1 Hz tick
    FuriMutex* mutex;
    Storage* storage;
    NotificationApp* notif;

    // nfc
    Nfc* nfc; // allocated once while running
    NfcDevice* device; // current tag, per-step
    NfcListener* listener; // current tag, per-step

    // tag library
    char names[MAX_TAGS][NAME_LEN];
    uint8_t tag_count;

    // state
    PlayMode mode;
    uint8_t interval; // seconds
    bool running;
    int current; // index into names[]
    uint8_t remaining; // seconds until next step
    bool last_load_ok;
} App;

/* ---------------- tag library ---------------- */

static void tags_scan(App* app) {
    app->tag_count = 0;

    // make sure the directory exists so first run doesn't look broken
    storage_common_mkdir(app->storage, EXT_PATH("apps_data/rsc_nfc_sequencer"));
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

// Called once per second from the main loop on a tick event.
static void run_tick(App* app) {
    if(!app->running) return;
    if(app->remaining > 0) app->remaining--;
    if(app->remaining == 0) {
        app->current = next_index(app, app->current);
        app->last_load_ok = emu_start_index(app, app->current);
        app->remaining = app->interval;
    }
}

/* ---------------- UI ---------------- */

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "RSC NFC Sequencer");
    canvas_draw_line(canvas, 0, 14, 128, 14);

    canvas_set_font(canvas, FontSecondary);
    char line[48];

    snprintf(line, sizeof(line), "Mode: %s   Delay: %ds", mode_label[app->mode], app->interval);
    canvas_draw_str(canvas, 2, 26, line);

    snprintf(line, sizeof(line), "Tags found: %d", app->tag_count);
    canvas_draw_str(canvas, 2, 38, line);

    if(app->running) {
        const char* nm = (app->current >= 0 && app->current < app->tag_count)
                             ? app->names[app->current]
                             : "?";
        snprintf(line, sizeof(line), "%s %.20s", app->last_load_ok ? ">>" : "!!", nm);
        canvas_draw_str(canvas, 2, 50, line);
        snprintf(line, sizeof(line), "next in %ds   OK=stop", app->remaining);
        canvas_draw_str(canvas, 2, 62, line);
    } else {
        if(app->tag_count == 0) {
            canvas_draw_str(canvas, 2, 50, "No .nfc in tags folder");
        } else {
            canvas_draw_str(canvas, 2, 50, "<> mode  ^v delay");
        }
        canvas_draw_str(canvas, 2, 62, "OK=start   Back=exit");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    AppEvent e = {.type = EventTypeInput, .input = *event};
    furi_message_queue_put(app->queue, &e, FuriWaitForever);
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    AppEvent e = {.type = EventTypeTick};
    furi_message_queue_put(app->queue, &e, 0);
}

/* ---------------- app lifecycle ---------------- */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->mode = ModeSequential;
    app->interval = INTERVAL_DEFAULT;
    app->current = -1;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    app->storage = furi_record_open(RECORD_STORAGE);
    app->notif = furi_record_open(RECORD_NOTIFICATION);

    tags_scan(app);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void app_free(App* app) {
    if(app->running) run_stop(app);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);

    furi_timer_free(app->timer);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t rsc_nfc_sequencer_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();

    bool exit = false;
    AppEvent e;
    while(!exit) {
        if(furi_message_queue_get(app->queue, &e, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        if(e.type == EventTypeTick) {
            run_tick(app);
        } else if(e.type == EventTypeInput) {
            if(e.input.type == InputTypeShort || e.input.type == InputTypeRepeat) {
                switch(e.input.key) {
                case InputKeyOk:
                    if(app->running)
                        run_stop(app);
                    else
                        run_start(app);
                    break;
                case InputKeyLeft:
                    if(!app->running) app->mode = (app->mode + ModeCount - 1) % ModeCount;
                    break;
                case InputKeyRight:
                    if(!app->running) app->mode = (app->mode + 1) % ModeCount;
                    break;
                case InputKeyUp:
                    if(!app->running && app->interval < INTERVAL_MAX) app->interval++;
                    break;
                case InputKeyDown:
                    if(!app->running && app->interval > INTERVAL_MIN) app->interval--;
                    break;
                case InputKeyBack:
                    if(app->running)
                        run_stop(app);
                    else
                        exit = true;
                    break;
                default:
                    break;
                }
            } else if(e.input.type == InputTypeLong && e.input.key == InputKeyBack) {
                exit = true;
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    app_free(app);
    return 0;
}
