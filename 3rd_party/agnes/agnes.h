/*
SPDX-License-Identifier: MIT

agnes
https://http://github.com/kgabis/agnes
Copyright (c) 2022 Krzysztof Gabis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef agnes_h
#define agnes_h

#ifdef __cplusplus
extern "C"
{
#endif

#define AGNES_VERSION_MAJOR 0
#define AGNES_VERSION_MINOR 2
#define AGNES_VERSION_PATCH 0

#define AGNES_VERSION_STRING "0.2.0"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AGNES_SCREEN_WIDTH = 256,
    AGNES_SCREEN_HEIGHT = 240
};

typedef struct {
    bool a;
    bool b;
    bool select;
    bool start;
    bool up;
    bool down;
    bool left;
    bool right;
} agnes_input_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} agnes_color_t;

typedef struct agnes agnes_t;
typedef struct agnes_state agnes_state_t;

// APU callback function types for external APU implementation
typedef void (*agnes_apu_write_func)(void* user_data, uint16_t addr, uint8_t val, uint64_t cpu_cycle);
typedef uint8_t (*agnes_apu_read_func)(void* user_data, uint16_t addr, uint64_t cpu_cycle);

agnes_t* agnes_make(void);
void agnes_destroy(agnes_t *agn);
bool agnes_load_ines_data(agnes_t *agnes, void *data, size_t data_size);
void agnes_set_input(agnes_t *agnes, const agnes_input_t *input_1, const agnes_input_t *input_2);
size_t agnes_state_size(void);
void agnes_dump_state(const agnes_t *agnes, agnes_state_t *out_res);
bool agnes_restore_state(agnes_t *agnes, const agnes_state_t *state);
bool agnes_tick(agnes_t *agnes, bool *out_new_frame);
bool agnes_next_frame(agnes_t *agnes);

agnes_color_t agnes_get_screen_pixel(const agnes_t *agnes, int x, int y);

// APU handler functions
void agnes_set_apu_handler(agnes_t *agnes, 
                           agnes_apu_write_func write_func,
                           agnes_apu_read_func read_func,
                           void *user_data);

// Get current CPU cycle count (for APU synchronization)
uint64_t agnes_get_cpu_cycles(const agnes_t *agnes);

// CPU state structure for external visualization
typedef struct agnes_cpu_state {
    uint8_t a;      // Accumulator
    uint8_t x;      // X register
    uint8_t y;      // Y register
    uint8_t sp;     // Stack pointer
    uint8_t p;      // Status register (flags)
    uint16_t pc;    // Program counter
} agnes_cpu_state_t;

// Get current CPU state
void agnes_get_cpu_state(const agnes_t *agnes, agnes_cpu_state_t *out_state);

#ifdef __cplusplus
}
#endif

#endif /* agnes_h */
