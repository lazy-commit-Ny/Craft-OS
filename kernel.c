#include <stddef.h>
#include <stdint.h>

#define WIDTH 800
#define HEIGHT 600
#define PIXELS (WIDTH * HEIGHT)
#define VIRTIO_BASE 0x10001000UL
#define VIRTIO_STRIDE 0x1000UL
#define VIRTIO_SLOTS 8
#define VIRTIO_MAGIC 0x74726976U
#define VIRTIO_DEVICE_GPU 16
#define VIRTIO_DEVICE_INPUT 18
#define QUEUE_SIZE 8
#define GPU_RESOURCE_ID 1
#define CURSOR_W 16
#define CURSOR_H 22

static uint32_t scene[PIXELS] __attribute__((aligned(4096)));
static uint32_t framebuffer[PIXELS] __attribute__((aligned(4096)));

static inline void fence_io(void) {
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
}

static uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static volatile uint8_t *const uart = (volatile uint8_t *)0x10000000UL;

static void serial_putc(char c) {
    while ((uart[5] & 0x20U) == 0) {}
    uart[0] = (uint8_t)c;
}

static void serial_write(const char *text) {
    while (*text) serial_putc(*text++);
}

static void serial_hex(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> shift) & 0xFU]);
    }
}

static int serial_getc_nonblock(void) {
    return (uart[5] & 1U) ? uart[0] : -1;
}

struct virtq_desc {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t length;
};

struct virtq_avail {
    volatile uint16_t flags;
    volatile uint16_t index;
    volatile uint16_t ring[QUEUE_SIZE];
    uint16_t used_event;
};

struct virtq_used {
    volatile uint16_t flags;
    volatile uint16_t index;
    struct virtq_used_elem ring[QUEUE_SIZE];
    uint16_t available_event;
};

struct virtio_queue {
    volatile uint32_t *registers;
    struct virtq_desc *descriptors;
    struct virtq_avail *available;
    struct virtq_used *used;
    uint16_t index;
    uint16_t last_used;
};

static struct virtq_desc gpu_descriptors[QUEUE_SIZE] __attribute__((aligned(16)));
static struct virtq_avail gpu_available __attribute__((aligned(2)));
static struct virtq_used gpu_used __attribute__((aligned(4)));
static struct virtio_queue gpu_queue;
static volatile uint32_t *gpu_registers;

static volatile uint32_t *find_virtio_device(uint32_t device_id) {
    for (uint32_t slot = 0; slot < VIRTIO_SLOTS; ++slot) {
        volatile uint32_t *registers =
            (volatile uint32_t *)(VIRTIO_BASE + slot * VIRTIO_STRIDE);
        if (registers[0] == VIRTIO_MAGIC && registers[2] == device_id) {
            return registers;
        }
    }
    return NULL;
}

static int setup_queue(volatile uint32_t *registers, uint32_t queue_index,
                       struct virtio_queue *queue, struct virtq_desc *descriptors,
                       struct virtq_avail *available, struct virtq_used *used) {
    registers[12] = queue_index;
    uint32_t maximum = registers[13];
    if (maximum < QUEUE_SIZE) return 0;

    queue->registers = registers;
    queue->descriptors = descriptors;
    queue->available = available;
    queue->used = used;
    queue->index = 0;
    queue->last_used = 0;

    registers[14] = QUEUE_SIZE;
    registers[32] = (uint32_t)(uintptr_t)descriptors;
    registers[33] = (uint32_t)((uint64_t)(uintptr_t)descriptors >> 32);
    registers[36] = (uint32_t)(uintptr_t)available;
    registers[37] = (uint32_t)((uint64_t)(uintptr_t)available >> 32);
    registers[40] = (uint32_t)(uintptr_t)used;
    registers[41] = (uint32_t)((uint64_t)(uintptr_t)used >> 32);
    fence_io();
    registers[17] = 1;
    return 1;
}

static int initialize_virtio(volatile uint32_t *registers) {
    if (!registers || registers[1] < 2) return 0;

    registers[28] = 0;
    registers[28] = 1;
    registers[28] = 3;

    registers[5] = 0;
    registers[8] = 0;
    registers[5] = 1;
    uint32_t high_features = registers[4];
    if ((high_features & 1U) == 0) {
        registers[28] = 0x80;
        return 0;
    }
    registers[9] = 1;
    registers[8] = 1;
    registers[28] = 11;
    if ((registers[28] & 8U) == 0) {
        registers[28] = 0x80;
        return 0;
    }
    return 1;
}

static int submit_gpu_command(const void *request, uint32_t request_size,
                              void *response, uint32_t response_size) {
    struct virtq_desc *desc = gpu_queue.descriptors;
    desc[0].address = (uintptr_t)request;
    desc[0].length = request_size;
    desc[0].flags = 1;
    desc[0].next = 1;
    desc[1].address = (uintptr_t)response;
    desc[1].length = response_size;
    desc[1].flags = 2;
    desc[1].next = 0;

    uint16_t available_index = gpu_queue.index;
    gpu_queue.available->ring[available_index % QUEUE_SIZE] = 0;
    fence_io();
    gpu_queue.available->index = ++gpu_queue.index;
    fence_io();
    gpu_queue.registers[20] = 0;

    uint32_t timeout = 100000000U;
    while (gpu_queue.used->index == gpu_queue.last_used && timeout--) {}
    if (gpu_queue.used->index == gpu_queue.last_used) return 0;
    ++gpu_queue.last_used;
    fence_io();
    return ((uint32_t *)response)[0] == 0x1100U;
}

struct gpu_header {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint32_t padding;
};

struct gpu_response {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint32_t padding;
};

struct gpu_rect {
    uint32_t x, y, width, height;
};

struct gpu_create_request {
    struct gpu_header header;
    uint32_t resource_id, format, width, height;
};

struct gpu_attach_request {
    struct gpu_header header;
    uint32_t resource_id, entry_count;
    uint64_t address;
    uint32_t length, padding;
};

struct gpu_scanout_request {
    struct gpu_header header;
    struct gpu_rect rectangle;
    uint32_t scanout_id, resource_id;
};

struct gpu_flush_request {
    struct gpu_header header;
    struct gpu_rect rectangle;
    uint32_t resource_id, padding;
};

struct gpu_transfer_request {
    struct gpu_header header;
    struct gpu_rect rectangle;
    uint64_t offset;
    uint32_t resource_id, padding;
};

static void gpu_command_header(struct gpu_header *header, uint32_t type) {
    header->type = type;
    header->flags = 0;
    header->fence_id = 0;
    header->context_id = 0;
    header->padding = 0;
}

static int gpu_update(int x, int y, int width, int height) {
    struct gpu_response response = {0};
    struct gpu_transfer_request transfer = {0};
    gpu_command_header(&transfer.header, 0x0105);
    transfer.rectangle = (struct gpu_rect){(uint32_t)x, (uint32_t)y,
                                           (uint32_t)width, (uint32_t)height};
    transfer.offset = ((uint64_t)y * WIDTH + (uint32_t)x) * sizeof(uint32_t);
    transfer.resource_id = GPU_RESOURCE_ID;
    if (!submit_gpu_command(&transfer, sizeof(transfer), &response, sizeof(response))) {
        serial_write("ERROR: GPU framebuffer transfer response ");
        serial_hex(response.type);
        serial_write("\r\n");
        return 0;
    }

    struct gpu_flush_request flush = {0};
    gpu_command_header(&flush.header, 0x0104);
    flush.rectangle = transfer.rectangle;
    flush.resource_id = GPU_RESOURCE_ID;
    if (!submit_gpu_command(&flush, sizeof(flush), &response, sizeof(response))) {
        serial_write("ERROR: GPU framebuffer flush response ");
        serial_hex(response.type);
        serial_write("\r\n");
        return 0;
    }
    return 1;
}

static int initialize_gpu(void) {
    gpu_registers = find_virtio_device(VIRTIO_DEVICE_GPU);
    if (!gpu_registers) {
        serial_write("ERROR: virtio-gpu device not found; slots:");
        for (uint32_t slot = 0; slot < VIRTIO_SLOTS; ++slot) {
            volatile uint32_t *registers =
                (volatile uint32_t *)(VIRTIO_BASE + slot * VIRTIO_STRIDE);
            serial_putc(' ');
            serial_hex(registers[0]);
            serial_putc('/');
            serial_hex(registers[2]);
        }
        serial_write("\r\n");
        return 0;
    }
    if (!initialize_virtio(gpu_registers)) {
        serial_write("ERROR: GPU transport negotiation failed, version ");
        serial_hex(gpu_registers[1]);
        serial_write("\r\n");
        return 0;
    }
    if (!setup_queue(gpu_registers, 0, &gpu_queue, gpu_descriptors,
                     &gpu_available, &gpu_used)) {
        serial_write("ERROR: GPU control queue setup failed\r\n");
        return 0;
    }
    gpu_queue.registers[20] = 0;
    gpu_registers[28] = 15;

    struct gpu_response response = {0};
    struct gpu_create_request create = {0};
    gpu_command_header(&create.header, 0x0101);
    create.resource_id = GPU_RESOURCE_ID;
    create.format = 1;
    create.width = WIDTH;
    create.height = HEIGHT;
    if (!submit_gpu_command(&create, sizeof(create), &response, sizeof(response))) {
        serial_write("ERROR: GPU resource creation failed, response ");
        serial_hex(response.type);
        serial_write("\r\n");
        return 0;
    }

    struct gpu_attach_request attach = {0};
    gpu_command_header(&attach.header, 0x0106);
    attach.resource_id = GPU_RESOURCE_ID;
    attach.entry_count = 1;
    attach.address = (uintptr_t)framebuffer;
    attach.length = sizeof(framebuffer);
    if (!submit_gpu_command(&attach, sizeof(attach), &response, sizeof(response))) {
        serial_write("ERROR: GPU framebuffer attachment failed, response ");
        serial_hex(response.type);
        serial_write("\r\n");
        return 0;
    }

    struct gpu_scanout_request scanout = {0};
    gpu_command_header(&scanout.header, 0x0103);
    scanout.rectangle = (struct gpu_rect){0, 0, WIDTH, HEIGHT};
    scanout.resource_id = GPU_RESOURCE_ID;
    if (!submit_gpu_command(&scanout, sizeof(scanout), &response, sizeof(response))) {
        serial_write("ERROR: GPU scanout setup failed, response ");
        serial_hex(response.type);
        serial_write("\r\n");
        return 0;
    }
    return 1;
}

static struct virtq_desc input_descriptors[QUEUE_SIZE] __attribute__((aligned(16)));
static struct virtq_avail input_available __attribute__((aligned(2)));
static struct virtq_used input_used __attribute__((aligned(4)));
static struct virtio_queue input_queue;
static int input_ready;

struct input_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

static struct input_event input_events[QUEUE_SIZE] __attribute__((aligned(16)));
static int mouse_x = WIDTH / 2;
static int mouse_y = HEIGHT / 2;
static int mouse_left_click;
static int mouse_moved;

static int initialize_mouse(void) {
    volatile uint32_t *registers = find_virtio_device(VIRTIO_DEVICE_INPUT);
    if (!initialize_virtio(registers) ||
        !setup_queue(registers, 0, &input_queue, input_descriptors,
                     &input_available, &input_used)) {
        return 0;
    }
    for (uint32_t i = 0; i < QUEUE_SIZE; ++i) {
        input_descriptors[i].address = (uintptr_t)&input_events[i];
        input_descriptors[i].length = sizeof(input_events[i]);
        input_descriptors[i].flags = 2;
        input_descriptors[i].next = 0;
        input_available.ring[i] = (uint16_t)i;
    }
    input_queue.index = QUEUE_SIZE;
    input_available.index = QUEUE_SIZE;
    fence_io();
    input_queue.registers[20] = 0;
    registers[28] = 15;
    input_ready = 1;
    return 1;
}

static void process_input_event(const struct input_event *event) {
    if (event->type == 2 && event->code == 0) {
        mouse_x += event->value * 2;
        mouse_moved = 1;
    } else if (event->type == 2 && event->code == 1) {
        mouse_y += event->value * 2;
        mouse_moved = 1;
    } else if (event->type == 1 && event->code == 0x110) {
        mouse_left_click = event->value != 0;
    }

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x > WIDTH - CURSOR_W) mouse_x = WIDTH - CURSOR_W;
    if (mouse_y > HEIGHT - CURSOR_H) mouse_y = HEIGHT - CURSOR_H;
}

static int poll_mouse(void) {
    if (!input_ready) return 0;
    int changed = 0;
    while (input_queue.used->index != input_queue.last_used) {
        fence_io();
        uint16_t used_slot = input_queue.last_used % QUEUE_SIZE;
        uint32_t descriptor_id = input_queue.used->ring[used_slot].id;
        if (descriptor_id >= QUEUE_SIZE) {
            ++input_queue.last_used;
            continue;
        }
        process_input_event(&input_events[descriptor_id]);
        input_available.ring[input_queue.index % QUEUE_SIZE] = (uint16_t)descriptor_id;
        ++input_queue.index;
        input_available.index = input_queue.index;
        ++input_queue.last_used;
        fence_io();
        input_queue.registers[20] = 0;
        changed = 1;
    }
    return changed;
}

static void put_pixel(int x, int y, uint32_t value) {
    if ((unsigned)x < WIDTH && (unsigned)y < HEIGHT) {
        scene[y * WIDTH + x] = value;
    }
}

static void fill_rect(int x, int y, int width, int height, uint32_t value) {
    if (width <= 0 || height <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > WIDTH ? WIDTH : x + width;
    int y1 = y + height > HEIGHT ? HEIGHT : y + height;
    for (int row = y0; row < y1; ++row) {
        uint32_t *pixel = &scene[row * WIDTH + x0];
        for (int col = x0; col < x1; ++col) *pixel++ = value;
    }
}

static void fill_round_rect(int x, int y, int width, int height, int radius,
                            uint32_t value) {
    if (width <= 0 || height <= 0) return;
    if (radius * 2 > width) radius = width / 2;
    if (radius * 2 > height) radius = height / 2;
    for (int row = 0; row < height; ++row) {
        int inset = 0;
        if (row < radius) {
            int dy = radius - row;
            while (inset < radius && (2 * inset + 1) * (2 * inset + 1) +
                   (2 * dy - 1) * (2 * dy - 1) > 4 * radius * radius) ++inset;
        } else if (row >= height - radius) {
            int dy = row - (height - radius - 1);
            while (inset < radius && (2 * inset + 1) * (2 * inset + 1) +
                   (2 * dy - 1) * (2 * dy - 1) > 4 * radius * radius) ++inset;
        }
        fill_rect(x + inset, y + row, width - inset * 2, 1, value);
    }
}

static const uint8_t letter_font[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {15,16,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

static const uint8_t digit_font[10][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};

static uint8_t glyph_row(char character, int row) {
    if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    if (character >= 'A' && character <= 'Z') return letter_font[character - 'A'][row];
    if (character >= '0' && character <= '9') return digit_font[character - '0'][row];
    switch (character) {
        case '-': return row == 3 ? 14 : 0;
        case '.': return row == 6 ? 4 : 0;
        case ':': return row == 2 || row == 5 ? 4 : 0;
        case '/': return (uint8_t)(1U << (row < 5 ? row : 4));
        case '[': return row == 0 || row == 6 ? 14 : 8;
        case ']': return row == 0 || row == 6 ? 14 : 2;
        case '?': { static const uint8_t g[7] = {14,17,1,2,4,0,4}; return g[row]; }
        case '!': return row < 5 ? 4 : (row == 6 ? 4 : 0);
        case '>': return row == 2 ? 8 : row == 3 ? 4 : row == 4 ? 8 : 0;
        case '_': return row == 6 ? 31 : 0;
        default: return 0;
    }
}

static void draw_char(int x, int y, char character, uint32_t value, int scale) {
    if (character == ' ') return;
    for (int row = 0; row < 7; ++row) {
        uint8_t bits = glyph_row(character, row);
        for (int col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, value);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint32_t value, int scale) {
    int advance = 6 * scale;
    for (int i = 0; text[i]; ++i) draw_char(x + i * advance, y, text[i], value, scale);
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t value) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        put_pixel(x0, y0, value);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

enum app_id { APP_HOME, APP_FILES, APP_TERMINAL, APP_CALCULATOR, APP_SETTINGS, APP_NETWORK, APP_COUNT };

static const char *const app_names[APP_COUNT] = {
    "HOME", "FILES", "TERMINAL", "CALCULATOR", "SETTINGS", "NETWORK"
};
static const uint32_t app_colors[APP_COUNT] = {
    0xFF3B82F6, 0xFF22C55E, 0xFF111827, 0xFF8B5CF6, 0xFF64748B, 0xFF0EA5E9
};
static int active_app = APP_HOME;
static int window_open = 1;
static int previous_click;
static int cursor_drawn;
static int cursor_x;
static int cursor_y;

static void draw_wallpaper(void) {
    for (int y = 0; y < HEIGHT; ++y) {
        uint32_t *pixel = &scene[y * WIDTH];
        for (int x = 0; x < WIDTH; ++x) {
            int wave = (x * 3 + y * 2 + (x * y) / 70) & 255;
            uint8_t red = (uint8_t)(14 + wave / 8 + (HEIGHT - y) / 50);
            uint8_t green = (uint8_t)(30 + wave / 4 + x / 35);
            uint8_t blue = (uint8_t)(76 + wave / 2 + (HEIGHT - y) / 15);
            *pixel++ = color(red, green, blue);
        }
    }
    for (int i = 0; i < 9; ++i) {
        int base_y = 210 + i * 27;
        uint32_t shade = color((uint8_t)(30 + i * 7), (uint8_t)(85 + i * 9),
                               (uint8_t)(190 + i * 5));
        for (int x = 0; x < WIDTH; ++x) {
            int curve = base_y + (x - 400) * (x - 400) / (180 + i * 18);
            if ((unsigned)curve < HEIGHT) {
                fill_rect(x, curve, 1, 2, shade);
            }
        }
    }
}

static void draw_app_icon(int center_x, int center_y, int app, int size) {
    fill_round_rect(center_x - size / 2 + 2, center_y - size / 2 + 4,
                    size, size, size / 5, color(19, 26, 43));
    fill_round_rect(center_x - size / 2, center_y - size / 2,
                    size, size, size / 5, app_colors[app]);
    if (app == APP_FILES) {
        fill_round_rect(center_x - size / 3, center_y - size / 5,
                        size * 2 / 3, size / 2, 4, color(240, 198, 76));
        fill_rect(center_x - size / 3, center_y - size / 5, size / 3, 4,
                  color(252, 218, 112));
    } else if (app == APP_TERMINAL) {
        draw_line(center_x - size / 4, center_y - 4, center_x - size / 10, center_y,
                  color(230, 235, 248));
        draw_line(center_x - size / 10, center_y, center_x - size / 4, center_y + 4,
                  color(230, 235, 248));
        fill_rect(center_x, center_y + 4, size / 4, 2, color(230, 235, 248));
    } else if (app == APP_CALCULATOR) {
        fill_round_rect(center_x - size / 3, center_y - size / 3,
                        size * 2 / 3, size * 2 / 3, 3, color(248, 248, 255));
        for (int i = 0; i < 3; ++i) {
            fill_round_rect(center_x - size / 5 + i * size / 6, center_y,
                            size / 12, size / 12, 2, app_colors[app]);
        }
    } else if (app == APP_SETTINGS) {
        for (int angle = 0; angle < 8; ++angle) {
            int dx = (angle & 1) ? size / 4 : -size / 4;
            int dy = (angle & 2) ? size / 4 : -size / 4;
            fill_rect(center_x + dx - 2, center_y + dy - 2, 5, 5,
                      color(245, 248, 255));
        }
        fill_round_rect(center_x - size / 6, center_y - size / 6,
                        size / 3, size / 3, size / 6, color(245, 248, 255));
    } else if (app == APP_NETWORK) {
        draw_line(center_x - size / 4, center_y + 3, center_x, center_y - size / 5,
                  color(248, 252, 255));
        draw_line(center_x, center_y - size / 5, center_x + size / 4, center_y + 3,
                  color(248, 252, 255));
        fill_round_rect(center_x - 3, center_y + 5, 6, 6, 3, color(248, 252, 255));
    } else {
        fill_round_rect(center_x - size / 4, center_y - size / 4,
                        size / 2, size / 2, size / 4, color(248, 252, 255));
    }
}

static void draw_app_content(void) {
    uint32_t ink = color(34, 43, 61);
    uint32_t muted = color(104, 116, 138);
    switch (active_app) {
        case APP_HOME:
            draw_text(96, 165, "WELCOME TO CRAFT OS", ink, 3);
            draw_text(96, 220, "RISC-V 64 DESKTOP", muted, 2);
            draw_text(96, 276, "CHOOSE AN APP FROM THE DOCK", ink, 2);
            draw_app_icon(165, 380, APP_FILES, 64);
            draw_app_icon(315, 380, APP_TERMINAL, 64);
            draw_app_icon(465, 380, APP_CALCULATOR, 64);
            draw_app_icon(615, 380, APP_SETTINGS, 64);
            break;
        case APP_FILES:
            draw_text(96, 165, "YOUR FILES", ink, 3);
            draw_text(96, 215, "HOME", muted, 2);
            for (int i = 0; i < 4; ++i) {
                int x = 104 + i * 140;
                fill_round_rect(x, 270, 104, 114, 12, color(240, 244, 250));
                draw_app_icon(x + 52, 318, APP_FILES, 46);
                draw_text(x + 12, 358, i == 0 ? "DOCS" : i == 1 ? "IMAGES" :
                          i == 2 ? "PROJECTS" : "TRASH", ink, 1);
            }
            draw_text(96, 435, "FILESYSTEM READY", muted, 2);
            break;
        case APP_TERMINAL:
            draw_text(96, 165, "TERMINAL", ink, 3);
            fill_round_rect(96, 220, 608, 224, 10, color(20, 26, 38));
            draw_text(120, 246, "CRAFT OS RISC-V64 SHELL", color(114, 219, 167), 2);
            draw_text(120, 290, "KERNEL  VIRTIO GPU READY", color(226, 232, 243), 2);
            draw_text(120, 328, input_ready ? "INPUT   VIRTIO MOUSE READY" :
                      "INPUT   MOUSE UNAVAILABLE", color(226, 232, 243), 2);
            draw_text(120, 386, "SERIAL: TYPE 1-6 TO OPEN APPS", color(248, 201, 111), 1);
            break;
        case APP_CALCULATOR:
            draw_text(96, 165, "CALCULATOR", ink, 3);
            fill_round_rect(220, 218, 360, 66, 12, color(240, 244, 250));
            draw_text(250, 240, "READY", ink, 3);
            {
                static const char *const keys[9] = {
                    "1", "2", "3", "4", "5", "6", "7", "8", "9"
                };
            for (int i = 0; i < 12; ++i) {
                int x = 220 + (i % 4) * 92;
                int y = 300 + (i / 4) * 48;
                fill_round_rect(x, y, 76, 38, 8, i == 11 ? app_colors[APP_CALCULATOR] :
                                color(232, 237, 245));
                draw_text(x + 32, y + 13, i < 9 ? keys[i] : i == 9 ? "0" : "=",
                          i == 11 ? color(255,255,255) : ink, 1);
            }
            }
            break;
        case APP_SETTINGS:
            draw_text(96, 165, "SYSTEM SETTINGS", ink, 3);
            draw_text(104, 228, "DISPLAY", muted, 2);
            draw_text(104, 277, "800 X 600   TRUE COLOR", ink, 2);
            draw_text(104, 336, "INPUT", muted, 2);
            draw_text(104, 385, input_ready ? "VIRTIO MOUSE   READY" :
                      "VIRTIO MOUSE   UNAVAILABLE", ink, 2);
            draw_text(104, 435, "ARCHITECTURE   RISCV64", ink, 2);
            break;
        case APP_NETWORK:
            draw_text(96, 165, "NETWORK", ink, 3);
            draw_app_icon(154, 273, APP_NETWORK, 68);
            draw_text(210, 254, "VIRTIO NETWORK", ink, 2);
            draw_text(210, 296, "NETWORK DRIVER NOT CONFIGURED", muted, 2);
            draw_text(104, 370, "VIRTIO-NET SUPPORT IS NOT INCLUDED", muted, 2);
            break;
        default:
            break;
    }
}

static void draw_desktop(void) {
    draw_wallpaper();
    fill_rect(0, 0, WIDTH, 38, color(246, 248, 252));
    fill_rect(0, 37, WIDTH, 1, color(222, 227, 236));
    draw_text(18, 12, "CRAFT", color(32, 42, 61), 1);
    draw_text(90, 12, app_names[active_app], color(52, 63, 83), 1);
    draw_text(610, 12, "RISCV64", color(83, 96, 118), 1);
    draw_text(700, 12, "VIRTIO", color(83, 96, 118), 1);

    if (window_open) {
        fill_round_rect(72, 76, 660, 426, 18, color(8, 15, 29));
        fill_round_rect(64, 68, 660, 426, 18, color(252, 253, 255));
        fill_round_rect(64, 68, 660, 46, 18, color(239, 242, 248));
        fill_rect(64, 96, 660, 18, color(239, 242, 248));
        fill_round_rect(82, 84, 15, 15, 7, color(255, 95, 86));
        fill_round_rect(106, 84, 15, 15, 7, color(255, 189, 46));
        fill_round_rect(130, 84, 15, 15, 7, color(39, 201, 63));
        draw_text(310, 87, app_names[active_app], color(72, 83, 102), 1);
        draw_app_content();
    }

    fill_round_rect(140, 525, 520, 64, 18, color(235, 239, 248));
    for (int app = 0; app < APP_COUNT; ++app) {
        int center_x = 190 + app * 78;
        draw_app_icon(center_x, 554, app, 44);
        if (window_open && active_app == app) {
            fill_round_rect(center_x - 3, 583, 6, 3, 2, color(58, 71, 94));
        }
    }
}

static void draw_cursor(void) {
    for (int row = 0; row < CURSOR_H; ++row) {
        int width = row < 15 ? row / 2 + 1 : (CURSOR_H - row) / 2 + 5;
        for (int col = 0; col < width; ++col) {
            int edge = col == 0 || col == width - 1 || row == CURSOR_H - 1;
            framebuffer[(mouse_y + row) * WIDTH + mouse_x + col] =
                edge ? color(12, 19, 31) : color(255, 255, 255);
        }
    }
}

static void copy_rect_to_framebuffer(int x, int y, int width, int height) {
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > WIDTH) width = WIDTH - x;
    if (y + height > HEIGHT) height = HEIGHT - y;
    if (width <= 0 || height <= 0) return;
    for (int row = y; row < y + height; ++row) {
        uint32_t *source = &scene[row * WIDTH + x];
        uint32_t *target = &framebuffer[row * WIDTH + x];
        for (int col = 0; col < width; ++col) target[col] = source[col];
    }
}

static void redraw_screen(void) {
    draw_desktop();
    for (int i = 0; i < PIXELS; ++i) framebuffer[i] = scene[i];
    draw_cursor();
    if (!gpu_update(0, 0, WIDTH, HEIGHT)) serial_write("ERROR: GPU FRAME UPDATE FAILED\r\n");
    cursor_x = mouse_x;
    cursor_y = mouse_y;
    cursor_drawn = 1;
}

static void update_cursor(void) {
    int left = cursor_drawn && cursor_x < mouse_x ? cursor_x : mouse_x;
    int top = cursor_drawn && cursor_y < mouse_y ? cursor_y : mouse_y;
    int right_old = cursor_drawn ? cursor_x + CURSOR_W : mouse_x + CURSOR_W;
    int bottom_old = cursor_drawn ? cursor_y + CURSOR_H : mouse_y + CURSOR_H;
    int right_new = mouse_x + CURSOR_W;
    int bottom_new = mouse_y + CURSOR_H;
    int right = right_old > right_new ? right_old : right_new;
    int bottom = bottom_old > bottom_new ? bottom_old : bottom_new;

    if (cursor_drawn) copy_rect_to_framebuffer(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
    copy_rect_to_framebuffer(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
    draw_cursor();
    copy_rect_to_framebuffer(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
    if (!gpu_update(left, top, right - left, bottom - top)) {
        serial_write("ERROR: CURSOR UPDATE FAILED\r\n");
    }
    cursor_x = mouse_x;
    cursor_y = mouse_y;
    cursor_drawn = 1;
}

static void handle_click(void) {
    if (mouse_y >= 525 && mouse_y <= 590 && mouse_x >= 150 && mouse_x < 650) {
        int app = (mouse_x - 151) / 78;
        if (app >= 0 && app < APP_COUNT) {
            active_app = app;
            window_open = 1;
        }
    } else if (window_open && mouse_y >= 80 && mouse_y <= 110 &&
               mouse_x >= 78 && mouse_x <= 105) {
        window_open = 0;
    }
    redraw_screen();
}

static void handle_serial_key(int key) {
    if (key >= '1' && key <= '6') {
        active_app = key - '1';
        window_open = 1;
        redraw_screen();
    } else if (key == 27) {
        window_open = 0;
        redraw_screen();
    } else if (key == '\r' || key == '\n') {
        window_open = !window_open;
        redraw_screen();
    }
}

void kernel_main(void) {
    serial_write("\r\nCraft OS: RISC-V 64 booted.\r\n");
    if (!initialize_gpu()) {
        serial_write("ERROR: virtio-gpu initialization failed; check QEMU devices.\r\n");
        for (;;) {
            int key = serial_getc_nonblock();
            if (key >= 0) serial_putc((char)key);
        }
    }
    serial_write("virtio-gpu: 800x600 framebuffer ready.\r\n");
    if (!initialize_mouse()) {
        serial_write("WARNING: virtio mouse not found; desktop is keyboard accessible.\r\n");
    } else {
        serial_write("virtio-mouse: ready.\r\n");
    }

    redraw_screen();
    for (;;) {
        int input_changed = poll_mouse();
        if (input_changed && mouse_left_click && !previous_click) {
            handle_click();
        } else if (input_changed && mouse_moved) {
            update_cursor();
        }
        if (input_changed) {
            previous_click = mouse_left_click;
            mouse_moved = 0;
        }

        int key = serial_getc_nonblock();
        if (key >= 0) handle_serial_key(key);
    }
}
