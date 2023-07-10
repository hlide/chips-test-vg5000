/*
    vg5000.c

    The VG5000µ is a French home computer from the early 80s
    It uses a Z80 CPU and a EF9245 video chip

    NOT EMULATED:
        - Joystick
*/

#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/clk.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "systems/vg5000.h"
#include "vg5000-roms.h"
#if defined(CHIPS_USE_UI)
    #define UI_DBG_USE_Z80
    #include "ui.h"
    #include "ui/ui_chip.h"
    #include "ui/ui_memedit.h"
    #include "ui/ui_memmap.h"
    #include "ui/ui_dasm.h"
    #include "ui/ui_dbg.h"
    #include "ui/ui_kbd.h"
    #include "ui/ui_z80.h"
    #include "ui/ui_audio.h"
    #include "ui/ui_snapshot.h"
    #include "ui/ui_vg5000.h"
#endif

typedef struct {
    uint32_t version;
    vg5000_t vg5000;
} vg5000_snapshot_t;

static struct {
    vg5000_t vg5000;
    uint32_t frame_time_us;
    uint32_t ticks;
    double emu_time_ms;
    #ifdef CHIPS_USE_UI
        ui_atom_t ui;
        atom_snapshot_t snapshots[UI_SNAPSHOT_MAX_SLOTS];
    #endif
} state;

#ifdef CHIPS_USE_UI
// static void ui_draw_cb(void);
// static void ui_boot_cb(atom_t* sys);
// static void ui_save_snapshot(size_t slot_index);
// static bool ui_load_snapshot(size_t slot_index);
// static void ui_load_snapshots_from_storage(void);
#define BORDER_TOP (24)
#else
#define BORDER_TOP (8)
#endif
#define BORDER_LEFT (8)
#define BORDER_RIGHT (8)
#define BORDER_BOTTOM (16)

vg5000_desc_t vg5000_desc() {
    return (vg5000_desc_t) {
        .type = VG5000_TYPE_11,
        .roms = {
            .vg5000_10 = { .ptr = dump_vg5000_rom_10, .size = sizeof(dump_vg5000_rom_10) },
            .vg5000_11 = { .ptr = dump_vg5000_rom_11, .size = sizeof(dump_vg5000_rom_11) },
        },
        #if defined(CHIPS_USE_UI)
        .debug = ui_vg5000_get_debug(&state.ui)
        #endif
    };
}

void app_init(void) {
    vg5000_desc_t desc = vg5000_desc();
    vg5000_init(&state.vg5000, &desc);
    gfx_init(&(gfx_desc_t){
        #ifdef CHIPS_USE_UI
        .draw_extra_cb = ui_draw,
        #endif
        .border = {
            .left = BORDER_LEFT,
            .right = BORDER_RIGHT,
            .top = BORDER_TOP,
            .bottom = BORDER_BOTTOM
        },
        .display_info = vg5000_display_info(&state.vg5000)
    });
    keybuf_init(&(keybuf_desc_t){ .key_delay_frames = 10 });
    clock_init();
    prof_init();
    fs_init();
    saudio_setup(&(saudio_desc){
        .logger.func = slog_func,
    });
    #ifdef CHIPS_USE_UI
        ui_init(ui_draw_cb);
        ui_atom_init(&state.ui, &(ui_atom_desc_t){
            .atom = &state.atom,
            .boot_cb = ui_boot_cb,
            .dbg_texture = {
                .create_cb = gfx_create_texture,
                .update_cb = gfx_update_texture,
                .destroy_cb = gfx_destroy_texture,
            },
            .snapshot = {
                .load_cb = ui_load_snapshot,
                .save_cb = ui_save_snapshot,
                .empty_slot_screenshot = {
                    .texture = gfx_shared_empty_snapshot_texture()
                }
            },
            .dbg_keys = {
                .cont = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F5), .name = "F5" },
                .stop = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F5), .name = "F5" },
                .step_over = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F6), .name = "F6" },
                .step_into = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F7), .name = "F7" },
                .step_tick = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F8), .name = "F8" },
                .toggle_breakpoint = { .keycode = simgui_map_keycode(SAPP_KEYCODE_F9), .name = "F9" }
            }
        });
        ui_load_snapshots_from_storage();
    #endif
    bool delay_input = false;
    // TODO: load file from command line
    // if (sargs_exists("file")) {
    //     delay_input = true;
    //     fs_start_load_file(FS_SLOT_IMAGE, sargs_value("file"));
    // }
    // TODO: load input from command line
    // if (!delay_input) {
    //     if (sargs_exists("input")) {
    //         keybuf_put(sargs_value("input"));
    //     }
    // }

}

static void draw_status_bar(void);

void app_frame(void) {
    state.frame_time_us = clock_frame_time();
    const uint64_t emu_start_time = stm_now();
    state.ticks = vg5000_exec(&state.vg5000, state.frame_time_us);
    state.emu_time_ms = stm_ms(stm_since(emu_start_time));
    draw_status_bar();
    gfx_draw(vg5000_display_info(&state.vg5000));

    // TODO: load file from command line
    //handle_file_loading();
    // TODO: load input from command line
    //send_keybuf_input();
}

/* keyboard input handling */
void app_input(const sapp_event* event) {
}

void app_cleanup(void) {
    vg5000_discard(&state.vg5000);
    #ifdef CHIPS_USE_UI
        ui_vg5000_discard(&state.ui);
        ui_discard();
    #endif
    saudio_shutdown();
    gfx_shutdown();
    sargs_shutdown();
}

static void draw_status_bar(void) {
    prof_push(PROF_EMU, (float)state.emu_time_ms);
    prof_stats_t emu_stats = prof_stats(PROF_EMU);
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    sdtx_canvas(w, h);
    sdtx_color3b(255, 255, 255);
    sdtx_pos(1.0f, (h / 8.0f) - 1.5f);
    sdtx_printf("frame:%.2fms emu:%.2fms (min:%.2fms max:%.2fms) ticks:%d", (float)state.frame_time_us * 0.001f, emu_stats.avg_val, emu_stats.min_val, emu_stats.max_val, state.ticks);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    sargs_setup(&(sargs_desc){ .argc=argc, .argv=argv });
    const chips_display_info_t info = vg5000_display_info(0);
    return (sapp_desc) {
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = 2 * info.screen.width + BORDER_LEFT + BORDER_RIGHT,
        .height = 2 * info.screen.height + BORDER_TOP + BORDER_BOTTOM,
        .window_title = "VG5000µ",
        .icon.sokol_default = true,
        .enable_dragndrop = true,
        .logger.func = slog_func,
    };
}
