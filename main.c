#include "lvgl/lvgl.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>

/* --- UART CONFIGURATION --- */
#define UART_PORT "/dev/ttyS1" 
#define BAUD_RATE B9600

static int uart_fd = -1;
static int temp_val = 0;
static int hum_val = 0;
static int gas_val = 0;

/* --- DISPLAY CONFIGURATION --- */
static int fbfd = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

/* --- UI GLOBALS --- */
static lv_obj_t * boot_screen;
static lv_obj_t * boot_bar;
static lv_timer_t * boot_timer;
static int boot_progress = 0;

static lv_obj_t * main_tabview;
static lv_timer_t * tab_rotate_timer;

/* Fixed: Added label arrays for ALL pages so numbers update correctly */
static lv_obj_t * gauge_arcs[3];
static lv_obj_t * gauge_labels[3];

static lv_obj_t * level_bars[3];
static lv_obj_t * level_labels[3];

static lv_obj_t * digital_texts[3];
const char* titles[3] = {"Temperature (C)", "Humidity (%)", "Gas Level"};

/* ---------------------------------------------------------
 * HARDWARE SETUP 
 * --------------------------------------------------------- */
int init_uart(const char *device) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) return -1;
    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, BAUD_RATE);
    cfsetospeed(&options, BAUD_RATE);
    options.c_cflag |= (CLOCAL | CREAD | CS8);
    options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;
    tcsetattr(fd, TCSANOW, &options);
    return fd;
}

void my_fb_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    if(fbfd <= 0) { lv_display_flush_ready(disp); return; }
    int32_t width = lv_area_get_width(area);
    int bytes_per_pixel = vinfo.bits_per_pixel / 8;

    for(int y = area->y1; y <= area->y2; y++) {
        long int location = (y + vinfo.yoffset) * finfo.line_length + (area->x1 + vinfo.xoffset) * bytes_per_pixel;
        lseek(fbfd, location, SEEK_SET);
        write(fbfd, px_map, width * bytes_per_pixel);
        px_map += width * bytes_per_pixel;
    }
    lv_display_flush_ready(disp);
}

/* ---------------------------------------------------------
 * PAGE 1: GAUGES
 * --------------------------------------------------------- */
void create_gauge_page(lv_obj_t * parent) {
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    /* Strict ROW layout (NO WRAP) prevents overlapping glitch */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int box_w = (screen_w / 3) - 30; /* Safer width */
    int box_h = box_w + 40;

    for(int i = 0; i < 3; i++) {
        lv_obj_t * box = lv_obj_create(parent);
        lv_obj_set_size(box, box_w, box_h);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(box, 0, 0); /* 0 Padding stops arcs from being cut off */

        lv_obj_t * title = lv_label_create(box);
        lv_label_set_text(title, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);

        gauge_arcs[i] = lv_arc_create(box);
        lv_obj_set_size(gauge_arcs[i], box_w - 40, box_w - 40);
        lv_arc_set_rotation(gauge_arcs[i], 135);
        lv_arc_set_bg_angles(gauge_arcs[i], 0, 270);
        lv_arc_set_range(gauge_arcs[i], 0, 100);
        lv_obj_align(gauge_arcs[i], LV_ALIGN_BOTTOM_MID, 0, -15);

        gauge_labels[i] = lv_label_create(box);
        lv_label_set_text(gauge_labels[i], "0");
        lv_obj_set_style_text_color(gauge_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(gauge_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_align_to(gauge_labels[i], gauge_arcs[i], LV_ALIGN_CENTER, 0, 0);
    }
}

/* ---------------------------------------------------------
 * PAGE 2: LEVEL BARS
 * --------------------------------------------------------- */
void create_bar_page(lv_obj_t * parent) {
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int box_w = (screen_w / 3) - 30;
    int box_h = box_w + 40;

    for(int i = 0; i < 3; i++) {
        lv_obj_t * box = lv_obj_create(parent);
        lv_obj_set_size(box, box_w, box_h);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(box, 0, 0);

        lv_obj_t * title = lv_label_create(box);
        lv_label_set_text(title, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);

        level_bars[i] = lv_bar_create(box);
        lv_obj_set_size(level_bars[i], 60, box_h - 110);
        lv_bar_set_range(level_bars[i], 0, 100);
        lv_obj_align(level_bars[i], LV_ALIGN_BOTTOM_MID, 0, -45);

        level_labels[i] = lv_label_create(box);
        lv_label_set_text(level_labels[i], "0");
        lv_obj_set_style_text_color(level_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(level_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_align_to(level_labels[i], level_bars[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
}

/* ---------------------------------------------------------
 * PAGE 3: DIGITAL TEXT
 * --------------------------------------------------------- */
void create_text_page(lv_obj_t * parent) {
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int box_w = (screen_w / 3) - 30;
    int box_h = box_w + 40;

    for(int i = 0; i < 3; i++) {
        lv_obj_t * box = lv_obj_create(parent);
        lv_obj_set_size(box, box_w, box_h);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(box, 0, 0);

        lv_obj_t * title = lv_label_create(box);
        lv_label_set_text(title, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);

        digital_texts[i] = lv_label_create(box);
        lv_label_set_text(digital_texts[i], "0");
        lv_obj_set_style_text_font(digital_texts[i], &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(digital_texts[i], lv_color_white(), 0);
        lv_obj_align(digital_texts[i], LV_ALIGN_CENTER, 0, 15);
    }
}

/* ---------------------------------------------------------
 * DATA & AUTO-ROTATION TIMERS
 * --------------------------------------------------------- */
static void sensor_update_timer_cb(lv_timer_t * timer) {
    /* Read UART */
    if (uart_fd != -1) {
        char rx_buffer[128];
        int rx_length = read(uart_fd, (void*)rx_buffer, sizeof(rx_buffer) - 1);
        if (rx_length > 0) {
            rx_buffer[rx_length] = '\0';
            sscanf(rx_buffer, "T:%d H:%d G:%d", &temp_val, &hum_val, &gas_val);
        }
    }

    /* FOR TESTING: Randomize values if UART is not connected */
    if (uart_fd == -1) {
        temp_val = rand() % 100; hum_val = rand() % 100; gas_val = rand() % 100;
    }

    int vals[3] = {temp_val, hum_val, gas_val};

    for(int i = 0; i < 3; i++) {
        lv_color_t c = (vals[i] < 50) ? lv_color_hex(0x00FF00) : (vals[i] < 80) ? lv_color_hex(0xFFFF00) : lv_color_hex(0xFF0000);

        /* Update Arcs */
        if(gauge_arcs[i]) {
            lv_arc_set_value(gauge_arcs[i], vals[i]);
            lv_obj_set_style_arc_color(gauge_arcs[i], c, LV_PART_INDICATOR);
        }
        if(gauge_labels[i]) lv_label_set_text_fmt(gauge_labels[i], "%d", vals[i]);
        
        /* Update Bars */
        if(level_bars[i]) {
            lv_bar_set_value(level_bars[i], vals[i], LV_ANIM_ON);
            lv_obj_set_style_bg_color(level_bars[i], c, LV_PART_INDICATOR);
        }
        if(level_labels[i]) lv_label_set_text_fmt(level_labels[i], "%d", vals[i]);
        
        /* Update Texts */
        if(digital_texts[i]) {
            lv_label_set_text_fmt(digital_texts[i], "%d", vals[i]);
            lv_obj_set_style_text_color(digital_texts[i], c, 0);
        }
    }
}

static void tab_rotate_timer_cb(lv_timer_t * timer) {
    if(main_tabview) {
        uint16_t current = lv_tabview_get_tab_active(main_tabview);
        current++;
        if(current > 2) current = 0; 
        lv_tabview_set_active(main_tabview, current, LV_ANIM_ON);
    }
}

static void load_main_dashboard(void) {
    main_tabview = lv_tabview_create(lv_screen_active());
    lv_obj_set_style_bg_color(main_tabview, lv_color_black(), 0);

    lv_obj_t * tab1 = lv_tabview_add_tab(main_tabview, "Gauges");
    lv_obj_t * tab2 = lv_tabview_add_tab(main_tabview, "Levels");
    lv_obj_t * tab3 = lv_tabview_add_tab(main_tabview, "Digital");

    create_gauge_page(tab1);
    create_bar_page(tab2);
    create_text_page(tab3);

    lv_timer_create(sensor_update_timer_cb, 500, NULL);       
    tab_rotate_timer = lv_timer_create(tab_rotate_timer_cb, 5000, NULL); 
}

/* ---------------------------------------------------------
 * STAGE 1: BOOT SCREEN LOGIC
 * --------------------------------------------------------- */
static void boot_timer_cb(lv_timer_t * timer) {
    boot_progress += 2; 
    if (boot_progress <= 100) {
        lv_bar_set_value(boot_bar, boot_progress, LV_ANIM_ON);
    } else {
        lv_timer_del(boot_timer);
        lv_obj_del(boot_screen); 
        load_main_dashboard();
    }
}

static void create_boot_screen(void) {
    boot_screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(boot_screen, vinfo.xres, vinfo.yres);
    lv_obj_set_style_bg_color(boot_screen, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(boot_screen, LV_OBJ_FLAG_SCROLLABLE);

    const char * name = "JUST DO ELECTRONICS";
    
    lv_obj_t * text_shadow = lv_label_create(boot_screen);
    lv_label_set_text(text_shadow, name);
    lv_obj_set_style_text_color(text_shadow, lv_color_hex(0x0055AA), 0);
    lv_obj_align(text_shadow, LV_ALIGN_CENTER, 3, -37);

    lv_obj_t * text_top = lv_label_create(boot_screen);
    lv_label_set_text(text_top, name);
    lv_obj_set_style_text_color(text_top, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(text_top, LV_ALIGN_CENTER, 0, -40);

    boot_bar = lv_bar_create(boot_screen);
    lv_obj_set_size(boot_bar, 400, 20);
    lv_obj_align(boot_bar, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_range(boot_bar, 0, 100);
    lv_obj_set_style_bg_color(boot_bar, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);

    boot_timer = lv_timer_create(boot_timer_cb, 50, NULL);
}

/* ---------------------------------------------------------
 * MAIN
 * --------------------------------------------------------- */
int main(void) {
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) return 1;
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);

    uart_fd = init_uart(UART_PORT);

    lv_init();
    lv_display_t * disp = lv_display_create(vinfo.xres, vinfo.yres);
    void * buf1 = malloc(vinfo.xres * 40 * 2);
    lv_display_set_buffers(disp, buf1, NULL, vinfo.xres * 40 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_fb_flush);

    create_boot_screen();

    while(1) {
        lv_timer_handler();
        usleep(5000); 
        lv_tick_inc(5); 
    }
    return 0;
}