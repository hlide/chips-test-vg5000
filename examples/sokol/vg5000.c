/*
    vg5000.c

    The VG5000µ is a French home computer from the early 80s
    It uses a Z80 CPU and a EF9245 video chip

    NOT EMULATED:
        - Joystick

    Copyright (c) 2023 Sylvain Glaize
    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.
    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:
        1. The origin of this software must not be misrepresented; you must not
        claim that you wrote the original software. If you use this software in a
        product, an acknowledgment in the product documentation would be
        appreciated but is not required.
        2. Altered source versions must be plainly marked as such, and must not
        be misrepresented as being the original software.
        3. This notice may not be removed or altered from any source
        distribution. 

*/

#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "common.h"
#include "chips/z80.h"
#include "chips/mem.h"
#include "chips/ef9345.h"
#include "chips/beeper.h"
#include "chips/clk.h"
#include "chips/kbd.h"
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
    #include "ui/ui_ef9345.h"
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
        ui_vg5000_t ui;
        vg5000_snapshot_t snapshots[UI_SNAPSHOT_MAX_SLOTS];
    #endif
} state;

#ifdef CHIPS_USE_UI
static void ui_draw_cb(void);
static void ui_boot_cb(vg5000_t* sys);
static void ui_save_snapshot(size_t slot_index);
static bool ui_load_snapshot(size_t slot_index);
static void ui_load_snapshots_from_storage(void);
#define BORDER_TOP (24)
#else
#define BORDER_TOP (8)
#endif
#define BORDER_LEFT (8)
#define BORDER_RIGHT (8)
#define BORDER_BOTTOM (16)

static void push_audio(const float* samples, int num_samples, void* user_data) {
    (void)user_data;
    saudio_push(samples, num_samples);
}

vg5000_desc_t vg5000_desc() {
    return (vg5000_desc_t) {
        .type = VG5000_TYPE_11,
        .audio = {
            .callback = { .func = push_audio },
            .sample_rate = saudio_sample_rate(),
        },        
        .roms = {
            .vg5000_10 = { .ptr = dump_vg5000_rom_10, .size = sizeof(dump_vg5000_rom_10) },
            .vg5000_11 = { .ptr = dump_vg5000_rom_11, .size = sizeof(dump_vg5000_rom_11) },
            .ef9345_charset = { .ptr = dump_vg5000_charset_rom, .size = sizeof(dump_vg5000_charset_rom) },
        },
        .audible_tape = true,
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
    keybuf_init(&(keybuf_desc_t){ .key_delay_frames = 5 });
    clock_init();
    prof_init();
    fs_init();
    saudio_setup(&(saudio_desc){
        .logger.func = slog_func,
    });
    #ifdef CHIPS_USE_UI
        ui_init(ui_draw_cb);
        ui_vg5000_init(&state.ui, &(ui_vg5000_desc_t){
            .vg5000 = &state.vg5000,
            .boot_cb = ui_boot_cb,
            .dbg_texture = {
                .create_cb = ui_create_texture,
                .update_cb = ui_update_texture,
                .destroy_cb = ui_destroy_texture,
            },
            .snapshot = {
                .load_cb = ui_load_snapshot,
                .save_cb = ui_save_snapshot,
                .empty_slot_screenshot = {
                    .texture = ui_shared_empty_snapshot_texture()
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
    if (sargs_exists("file")) {
        delay_input = true;
        fs_start_load_file(FS_SLOT_IMAGE, sargs_value("file"));
    }
    if (!delay_input) {
        if (sargs_exists("input")) {
            keybuf_put(sargs_value("input"));
        }
    }
}

static void handle_file_loading(void);
static void send_keybuf_input(void);
static void draw_status_bar(void);

void app_frame(void) {
    state.frame_time_us = clock_frame_time();
    const uint64_t emu_start_time = stm_now();
    state.ticks = vg5000_exec(&state.vg5000, state.frame_time_us);
    state.emu_time_ms = stm_ms(stm_since(emu_start_time));
    draw_status_bar();
    gfx_draw(vg5000_display_info(&state.vg5000));

    handle_file_loading();
    send_keybuf_input();
}

/* keyboard input handling */
void app_input(const sapp_event* event) {
    // TODO: accept dropped files also when ImGui grabs input
    // if (event->type == SAPP_EVENTTYPE_FILES_DROPPED) {
    //     fs_start_load_dropped_file(FS_SLOT_IMAGE);
    // }
    #ifdef CHIPS_USE_UI
    if (ui_input(event)) {
        // input was handled by UI
        return;
    }
    #endif
    switch (event->type) {
        int c;
        case SAPP_EVENTTYPE_CHAR:
            c = (int) event->char_code;
            if ((c > 0x20) && (c < 0x7F)) {
                c = toupper(c); // Send all alpha characters as upper case
                vg5000_key_down(&state.vg5000, c);
                vg5000_key_up(&state.vg5000, c);
            }
            break;
        case SAPP_EVENTTYPE_KEY_DOWN:
        case SAPP_EVENTTYPE_KEY_UP:
            switch (event->key_code) {
                case SAPP_KEYCODE_SPACE:        c = 0x20; break;
                case SAPP_KEYCODE_LEFT_ALT:     c = 0x01; break; // Shift+Ctrl -> Accent
                case SAPP_KEYCODE_HOME:         c = 0x02; break; // EFFE
                case SAPP_KEYCODE_TAB:          c = 0x06; break; // INS
                case SAPP_KEYCODE_ESCAPE:       c = 0x07; break;
                case SAPP_KEYCODE_LEFT:         c = 0x08; break;
                case SAPP_KEYCODE_RIGHT:        c = 0x09; break;
                case SAPP_KEYCODE_DOWN:         c = 0x0A; break;
                case SAPP_KEYCODE_UP:           c = 0x0B; break;
                case SAPP_KEYCODE_ENTER:        c = 0x0D; break;
                case SAPP_KEYCODE_BACKSPACE:    c = 0x0C; break;
                case SAPP_KEYCODE_RIGHT_ALT:    c = 0x0E; break; // Caps Lock
                case SAPP_KEYCODE_LEFT_CONTROL: c = 0x0F; break;
                case SAPP_KEYCODE_END: // Triangle Key is special, it's not part of the keyboard matrix
                    if (event->type == SAPP_EVENTTYPE_KEY_DOWN) {
                        vg5000_triangle_key_pressed(&state.vg5000);
                    }
                    c = 0;
                    break; 
                default:                        c = 0; break;
            }
            if (c) {
                if (event->type == SAPP_EVENTTYPE_KEY_DOWN) {
                    vg5000_key_down(&state.vg5000, c);
                }
                else {
                    vg5000_key_up(&state.vg5000, c);
                }
            }
            break;
        default:
            break;
    }
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

uint64_t tape_short_impulse(uint16_t* buffer) {
    buffer[0] = 833;
    buffer[1] = 833;
    return 2;
}

uint64_t tape_long_impulse(uint16_t* buffer) {
    buffer[0] = 1666;
    buffer[1] = 1666;
    return 2;
}

uint64_t tape_end_of_byte(uint16_t* buffer) {
    uint64_t output_position = 0;
    for (size_t i = 0; i < 4; i++) {
        output_position += tape_short_impulse(buffer + output_position);
    }
    output_position += tape_long_impulse(buffer + output_position);
 
    return output_position;
}

bool k7_to_tape_buffer(vg5000_t* sys, chips_range_t k7_data, chips_range_t* tape_buffer) {
    assert(sys && k7_data.ptr && tape_buffer);

    if (k7_data.size < 32) {
        return false;
    }

    bool success = true;

    // Allocate the oupput tape buffer
    const uint32_t total_synchro = 30000;
    tape_buffer->size = (k7_data.size + total_synchro) * 40;
    tape_buffer->ptr = malloc(tape_buffer->size);

    uint32_t output_position = 0;
    uint16_t* tape_buffer_ptr = tape_buffer->ptr;

    // Start with a short silence (which also will but the signal to high at the end)
    tape_buffer_ptr[output_position++] = 17400;

    // Then 30000 impulses of synchronisation
    for (int i = 0; i < 30000; i++) {
        output_position += tape_short_impulse(tape_buffer_ptr + output_position);
    }
    output_position += tape_end_of_byte(tape_buffer_ptr + output_position);

    assert(output_position < tape_buffer->size);

    // Iterate over the 32 first bytes of the input buffer
    const uint8_t* k7_data_ptr = k7_data.ptr;
    for (size_t i = 0; i < 32; i++) {
        uint8_t byte = k7_data_ptr[i];
        for (size_t j = 0; j < 8; j++) {
            if (byte & 0x1) { 
                output_position += tape_short_impulse(tape_buffer_ptr + output_position);
                output_position += tape_short_impulse(tape_buffer_ptr + output_position);
            }
            else {
                output_position += tape_long_impulse(tape_buffer_ptr + output_position);
            }
            byte >>= 1;
        }

        output_position += tape_end_of_byte(tape_buffer_ptr + output_position);
    }

    assert(output_position < tape_buffer->size);

    // Another short silence
    //tape_buffer_ptr[output_position++] = 10000;

    // 7200 impulses of synchronisation
    for (int i = 0; i < 7200; i++) {
        output_position += tape_short_impulse(tape_buffer_ptr + output_position);
    }
    output_position += tape_end_of_byte(tape_buffer_ptr + output_position);

    assert(output_position < tape_buffer->size);

    // TODO: it would be better to get it from the k7 header data
    // Then the rest of the data
    for (size_t i = 32; i < k7_data.size; i++) {
        uint8_t byte = k7_data_ptr[i];
        for (size_t j = 0; j < 8; j++) {
            if (byte & 0x1) { 
                output_position += tape_short_impulse(tape_buffer_ptr + output_position);
                output_position += tape_short_impulse(tape_buffer_ptr + output_position);
            }
            else {
                output_position += tape_long_impulse(tape_buffer_ptr + output_position);
            }
            byte >>= 1;
        }

        // Followed by End of Byte, which is 4 short impulses followed by one long impulse
        for (int j = 0; j < 4; j++) {
            output_position += tape_short_impulse(tape_buffer_ptr + output_position);
        }
        output_position += tape_long_impulse(tape_buffer_ptr + output_position);
    }

    // printf("input data size: %d\n", k7_data.size);
    // printf("initial allocation: %d\n", tape_buffer->size);
    // printf("output_position: %d\n", output_position * 2);
    // printf("percentage used: %d\n", (output_position * 2 * 100) / tape_buffer->size);

    tape_buffer->size = output_position * 2;

    return success;
}

void k7_to_tape_buffer_free(chips_range_t tape_buffer) {
    free(tape_buffer.ptr);
    tape_buffer.ptr = NULL;
}

static void handle_file_loading(void) {
    fs_dowork();
    const uint32_t load_delay_frames = 120;
    if (fs_success(FS_SLOT_IMAGE) && clock_frame_count_60hz() > load_delay_frames) {
        const chips_range_t file_data = fs_data(FS_SLOT_IMAGE);
        bool load_success = false;
        if (fs_ext(FS_SLOT_IMAGE, "k7")) {
            chips_range_t tape_buffer;
            load_success = k7_to_tape_buffer(&state.vg5000, file_data, &tape_buffer);
            if (load_success) {
                load_success = vg5000_insert_tape(&state.vg5000, tape_buffer);
                keybuf_put("CLOAD\n");
            }
            printf("Inserting tape: %s\n", load_success ? "success" : "failure");
            k7_to_tape_buffer_free(tape_buffer);
        }
        else {
            // TODO: implement quickload
            // TODO: implement loading of ROM
            // load_success = kc85_quickload(&state.vg5000, file_data);
        }
        if (load_success) {
            if (clock_frame_count_60hz() > (load_delay_frames + 10)) {
                gfx_flash_success();
            }
            if (sargs_exists("input")) {
                keybuf_put(sargs_value("input"));
            }
        }
        else {
            gfx_flash_error();
        }
        fs_reset(FS_SLOT_IMAGE);
    }
}

static void send_keybuf_input(void) {
    uint8_t key_code;
    if (0 != (key_code = keybuf_get(state.frame_time_us))) {
        vg5000_key_down(&state.vg5000, key_code);
        vg5000_key_up(&state.vg5000, key_code);
    }
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

#if defined(CHIPS_USE_UI)

void ui_draw_cb(void) {
    ui_vg5000_draw(&state.ui);
}

static void ui_boot_cb(vg5000_t* sys) {
    vg5000_desc_t desc = vg5000_desc();
    vg5000_init(sys, &desc);
}

static void ui_update_snapshot_screenshot(size_t slot) {
    ui_snapshot_screenshot_t screenshot = {
        .texture = ui_create_screenshot_texture(vg5000_display_info(&state.snapshots[slot].vg5000))
    };
    ui_snapshot_screenshot_t prev_screenshot = ui_snapshot_set_screenshot(&state.ui.snapshot, slot, screenshot);
    if (prev_screenshot.texture) {
        ui_destroy_texture(prev_screenshot.texture);
    }
}

static void ui_save_snapshot(size_t slot) {
    if (slot < UI_SNAPSHOT_MAX_SLOTS) {
        state.snapshots[slot].version = vg5000_save_snapshot(&state.vg5000, &state.snapshots[slot].vg5000);
        ui_update_snapshot_screenshot(slot);
        fs_save_snapshot("vg5000", slot, (chips_range_t){ .ptr = &state.snapshots[slot], sizeof(vg5000_snapshot_t) });
    }
}

static bool ui_load_snapshot(size_t slot) {
    bool success = false;
    if ((slot < UI_SNAPSHOT_MAX_SLOTS) && (state.ui.snapshot.slots[slot].valid)) {
        success = vg5000_load_snapshot(&state.vg5000, state.snapshots[slot].version, &state.snapshots[slot].vg5000);
    }
    return success;
}

static void ui_fetch_snapshot_callback(const fs_snapshot_response_t* response) {
    assert(response);
    if (response->result != FS_RESULT_SUCCESS) {
        return;
    }
    if (response->data.size != sizeof(vg5000_snapshot_t)) {
        return;
    }
    if (((vg5000_snapshot_t*)response->data.ptr)->version != VG5000_SNAPSHOT_VERSION) {
        return;
    }
    size_t snapshot_slot = response->snapshot_index;
    assert(snapshot_slot < UI_SNAPSHOT_MAX_SLOTS);
    memcpy(&state.snapshots[snapshot_slot], response->data.ptr, response->data.size);
    ui_update_snapshot_screenshot(snapshot_slot);
}

static void ui_load_snapshots_from_storage(void) {
    for (size_t snapshot_slot = 0; snapshot_slot < UI_SNAPSHOT_MAX_SLOTS; snapshot_slot++) {
        fs_start_load_snapshot(FS_SLOT_SNAPSHOTS, "vg5000", snapshot_slot, ui_fetch_snapshot_callback);
    }
}
#endif


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
