#include "lvgl/lvgl.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <signal.h>

/* --- HARDWARE GPIO CONFIGURATION --- */
#define BUTTON_GPIO_NUM  "60"                   // Maps physically to Pin P9.12
#define BUTTON_SYSFS_DIR "/sys/class/gpio/gpio60"

/* --- UART CONFIGURATION --- */
#define UART_PORT "/dev/ttyS2" 
#define BAUD_RATE B9600

static int uart_fd = -1;
static int gpio_fd = -1;                        // File descriptor for button state tracking
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

static lv_obj_t * gauge_arcs[3];
static lv_obj_t * gauge_labels[3];

static lv_obj_t * level_bars[3];
static lv_obj_t * level_labels[3];

static lv_obj_t * digital_texts[3];
const char* titles[3] = {"Temperature (C)", "Humidity (%)", "Gas Level"};

/* --- GRAPH CONFIGURATION (3 Separate Windows) --- */
static lv_obj_t * sensor_charts[3];
static lv_chart_series_t * sensor_series[3];
static const uint32_t chart_colors[3] = {0xFF0000, 0x0000FF, 0x00FF00};

/* ---------------------------------------------------------
 * CLEAN EXIT & SCREEN CLEAR LOGIC
 * --------------------------------------------------------- */
void clear_screen_black(void) {
    if (fbfd > 0) {
        long int screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
        uint8_t *black_buf = (uint8_t *)calloc(1, screensize);
        if (black_buf) {
            lseek(fbfd, 0, SEEK_SET);
            if (write(fbfd, black_buf, screensize) < 0) {
                // Silences warn_unused_result
            }
            free(black_buf);
        }
    }
}

void handle_exit_signal(int sig) {
    printf("\n[INFO] Intercepted shutdown signal (%d). Cleaning up...\n", sig);
    if (uart_fd != -1) {
        close(uart_fd);
    }
    if (gpio_fd != -1) {
        close(gpio_fd);
    }
    clear_screen_black();
    if (fbfd > 0) {
        close(fbfd);
    }
    printf("[INFO] Screen cleared. Safe exit complete.\n");
    exit(0);
}

/* ---------------------------------------------------------
 * HARDWARE SETUP 
 * --------------------------------------------------------- */
int init_button_gpio(void) {
    // 1. Configure the pin muxing to standard GPIO with an internal Pull-Up resistor
    (void)!system("config-pin P9.12 gpio_pu");

    // 2. Export the GPIO interface via Sysfs if not already exported
    if (access(BUTTON_SYSFS_DIR, F_OK) != 0) {
        int export_fd = open("/sys/class/gpio/export", O_WRONLY);
        if (export_fd != -1) {
            if (write(export_fd, BUTTON_GPIO_NUM, sizeof(BUTTON_GPIO_NUM)) < 0) { /* Ignore payload write flags */ }
            close(export_fd);
            usleep(100000); // Small 100ms processing threshold for filesystem generation
        }
    }

    // 3. Set direction explicitly to input
    int dir_fd = open(BUTTON_SYSFS_DIR "/direction", O_WRONLY);
    if (dir_fd == -1) return -1;
    if (write(dir_fd, "in", 2) < 0) { /* Ignore warn_unused_result */ }
    close(dir_fd);

    // 4. Open the data value stream using non-blocking read access
    return open(BUTTON_SYSFS_DIR "/value", O_RDONLY | O_NONBLOCK);
}

int init_uart(const char *device) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) return -1;
    
    struct termios options;
    tcgetattr(fd, &options);
    
    cfsetispeed(&options, BAUD_RATE);
    cfsetospeed(&options, BAUD_RATE);
    
    options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    options.c_cflag |= (CLOCAL | CREAD | CS8);
    
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
        if (write(fbfd, px_map, width * bytes_per_pixel) < 0) {
            // Silences warn_unused_result
        }
        px_map += width * bytes_per_pixel;
    }
    lv_display_flush_ready(disp);
}

/* ---------------------------------------------------------
 * UI VIEW ROTATION ACTION (NOW EXCLUSIVELY TRIGGERED BY BUTTON)
 * --------------------------------------------------------- */
void advance_dashboard_page(void) {
    if (main_tabview) {
        uint16_t current = lv_tabview_get_tab_active(main_tabview);
        current++;
        if (current > 3) current = 0;  // Loop back to index 0 after the 4th tab
        
        lv_tabview_set_active(main_tabview, current, LV_ANIM_ON);
    }
}

/* ---------------------------------------------------------
 * PAGE 1: GAUGES
 * --------------------------------------------------------- */
void create_gauge_page(lv_obj_t * parent) {
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
 * PAGE 4: SEPARATE 3-WINDOW REAL-TIME GRAPH
 * --------------------------------------------------------- */
void create_chart_page(lv_obj_t * parent) {
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int box_w = (screen_w / 3) - 15; 
    int box_h = box_w + 40;

    for(int i = 0; i < 3; i++) {
        lv_obj_t * box = lv_obj_create(parent);
        lv_obj_set_size(box, box_w, box_h);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(box, 5, 0);

        lv_obj_t * title = lv_label_create(box);
        lv_label_set_text(title, titles[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);

        sensor_charts[i] = lv_chart_create(box);
        lv_obj_set_size(sensor_charts[i], box_w - 65, box_h - 95);
        lv_obj_align(sensor_charts[i], LV_ALIGN_BOTTOM_RIGHT, -5, -30);
        lv_chart_set_type(sensor_charts[i], LV_CHART_TYPE_LINE);
        lv_obj_set_style_bg_color(sensor_charts[i], lv_color_hex(0x111111), 0);

        lv_chart_set_axis_min_value(sensor_charts[i], LV_CHART_AXIS_PRIMARY_Y, 0);
        lv_chart_set_axis_max_value(sensor_charts[i], LV_CHART_AXIS_PRIMARY_Y, 100);
        lv_chart_set_point_count(sensor_charts[i], 20);
        lv_chart_set_update_mode(sensor_charts[i], LV_CHART_UPDATE_MODE_SHIFT);
        lv_obj_set_style_pad_all(sensor_charts[i], 0, 0);

        // Vertical Y Scale
        lv_obj_t * scale_y = lv_scale_create(box);
        lv_obj_set_size(scale_y, 35, box_h - 95);
        lv_obj_align_to(scale_y, sensor_charts[i], LV_ALIGN_OUT_LEFT_MID, -2, 0);
        lv_scale_set_mode(scale_y, LV_SCALE_MODE_VERTICAL_LEFT);
        lv_scale_set_range(scale_y, 0, 100);
        lv_scale_set_total_tick_count(scale_y, 5);
        lv_scale_set_major_tick_every(scale_y, 1);
        lv_obj_set_style_text_color(scale_y, lv_color_white(), 0);
        lv_obj_set_style_line_color(scale_y, lv_color_hex(0x777777), 0);

        // Horizontal X Scale
        lv_obj_t * scale_x = lv_scale_create(box);
        lv_obj_set_size(scale_x, box_w - 65, 25);
        lv_obj_align_to(scale_x, sensor_charts[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        lv_scale_set_mode(scale_x, LV_SCALE_MODE_HORIZONTAL_BOTTOM);
        lv_scale_set_total_tick_count(scale_x, 4);
        lv_scale_set_major_tick_every(scale_x, 1);
        
        static const char * scale_text_x[] = {"-15s", "-10s", "-5s", "Now", NULL};
        lv_scale_set_text_src(scale_x, scale_text_x);
        lv_obj_set_style_text_color(scale_x, lv_color_white(), 0);
        lv_obj_set_style_line_color(scale_x, lv_color_hex(0x777777), 0);

        sensor_series[i] = lv_chart_add_series(sensor_charts[i], lv_color_hex(chart_colors[i]), LV_CHART_AXIS_PRIMARY_Y);
    }
}

/* ---------------------------------------------------------
 * DATA SAMPLING TIMER
 * --------------------------------------------------------- */
static void sensor_update_timer_cb(lv_timer_t * timer) {
    if (uart_fd != -1) {
        static char rx_buffer[128];
        static int rx_index = 0;
        char single_char;

        while (read(uart_fd, &single_char, 1) > 0) {
            if (single_char == '\n') {
                rx_buffer[rx_index] = '\0';
                sscanf(rx_buffer, "T:%d H:%d G:%d", &temp_val, &hum_val, &gas_val);
                rx_index = 0; 
            } else if (single_char != '\r') { 
                rx_buffer[rx_index] = single_char;
                rx_index++;
                if (rx_index >= sizeof(rx_buffer)) rx_index = 0; 
            }
        }
    }

    int vals[3] = {temp_val, hum_val, gas_val};

    for(int i = 0; i < 3; i++) {
        lv_color_t c = (vals[i] < 50) ? lv_color_hex(0x00FF00) : (vals[i] < 80) ? lv_color_hex(0xFFFF00) : lv_color_hex(0xFF0000);

        if(gauge_arcs[i]) {
            lv_arc_set_value(gauge_arcs[i], vals[i]);
            lv_obj_set_style_arc_color(gauge_arcs[i], c, LV_PART_INDICATOR);
        }
        if(gauge_labels[i]) lv_label_set_text_fmt(gauge_labels[i], "%d", vals[i]);
        
        if(level_bars[i]) {
            lv_bar_set_value(level_bars[i], vals[i], LV_ANIM_ON);
            lv_obj_set_style_bg_color(level_bars[i], c, LV_PART_INDICATOR);
        }
        if(level_labels[i]) lv_label_set_text_fmt(level_labels[i], "%d", vals[i]);
        
        if(digital_texts[i]) {
            lv_label_set_text_fmt(digital_texts[i], "%d", vals[i]);
            lv_obj_set_style_text_color(digital_texts[i], c, 0);
        }

        if(sensor_charts[i] && sensor_series[i]) {
            lv_chart_set_next_value(sensor_charts[i], sensor_series[i], vals[i]);
            lv_chart_refresh(sensor_charts[i]);
        }
    }
}

/* --- NON-BLOCKING HARDWARE BUTTON SAMPLING TIMER --- */
static void hardware_button_timer_cb(lv_timer_t * timer) {
    if (gpio_fd < 0 || boot_screen != NULL) return; // Block rotation during boot loading phase

    static int last_state = 1; // Pulled High (1) via internal configuration
    char state_char;

    lseek(gpio_fd, 0, SEEK_SET);
    if (read(gpio_fd, &state_char, 1) > 0) {
        int current_state = (state_char == '0') ? 0 : 1;

        // Detect Falling Edge (1 -> 0 indicates a distinct physical click down)
        if (current_state == 0 && last_state == 1) {
            advance_dashboard_page(); // Trigger slide change instantly
        }
        last_state = current_state; 
    }
}

static void load_main_dashboard(void) {
    main_tabview = lv_tabview_create(lv_screen_active());
    lv_obj_set_style_bg_color(main_tabview, lv_color_black(), 0);

    lv_obj_t * tab1 = lv_tabview_add_tab(main_tabview, "Digital");
    lv_obj_t * tab2 = lv_tabview_add_tab(main_tabview, "Levels");
    lv_obj_t * tab3 = lv_tabview_add_tab(main_tabview, "Gauges"); 
    lv_obj_t * tab4 = lv_tabview_add_tab(main_tabview, "Graph"); 


    create_text_page(tab1);
    create_bar_page(tab2);
    create_gauge_page(tab3);
    create_chart_page(tab4); 

    // Only updating the sensors on a timer now; the screen will never skip automatically anymore
    lv_timer_create(sensor_update_timer_cb, 500, NULL);       
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
        boot_screen = NULL; 
        load_main_dashboard();
    }
}

static void create_boot_screen(void) {
    boot_screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(boot_screen, vinfo.xres, vinfo.yres);
    lv_obj_set_style_bg_color(boot_screen, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(boot_screen, LV_OBJ_FLAG_SCROLLABLE);

    const char * name = " UART Communication ";
    
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
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);

    printf("Configuring UART2 pins...\n");
    (void)!system("config-pin P9.21 uart");
    (void)!system("config-pin P9.22 uart");

    printf("Configuring Input Button GPIO on P9.12...\n");
    gpio_fd = init_button_gpio();
    if (gpio_fd < 0) {
        printf("[WARNING] Failed to bind to Button Hardware Line. App running without input hardware controls.\n");
    }

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

    // Checked every 50ms for low-overhead physical debouncing
    lv_timer_create(hardware_button_timer_cb, 50, NULL);

    while(1) {
        lv_timer_handler();
        usleep(5000); 
        lv_tick_inc(5); 
    }
    return 0;
}