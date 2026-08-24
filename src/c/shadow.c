#include <pebble.h>
#include "config.h"

// --- CONFIGURATION & MACROS ---
#define REDRAW_INTERVAL 30     // Redraw the map shadow every 30 minutes
#define TIME_OFFSET_PERSIST 1  // Storage key for phone time offset
#define LAT_PERSIST 2          // Storage key for saved Latitude
#define LON_PERSIST 3          // Storage key for saved Longitude

// Define screen dimensions, fonts, and layout sizes dynamically based on the watch hardware
#if PBL_DISPLAY_WIDTH > 144
  // Settings for wider screens (like Pebble Time 2)
  #define WIDTH 200
  #define HEIGHT 139
  #define TIME_FONT FONT_KEY_LECO_60_NUMBERS_AM_PM
  #define DATE_FONT FONT_KEY_GOTHIC_28_BOLD
  #define TEXT_OFFSET 10
  #define ODOT_SIZE 5
  #define IDOT_SIZE 2
  #define CROSS_WIDTH 2
  #define ODOT_WIDTH 2
#else
  // Settings for standard 144px screens (like Pebble Time, Steel, Classic)
  #define WIDTH 144
  #define HEIGHT 100
  #define TIME_FONT FONT_KEY_LECO_42_NUMBERS
  #define DATE_FONT FONT_KEY_GOTHIC_18_BOLD
  #define TEXT_OFFSET 3
  #define ODOT_SIZE 3
  #define IDOT_SIZE 1
  #define CROSS_WIDTH 1
  #define ODOT_WIDTH 1
#endif

// --- GLOBAL VARIABLES ---
// Coordinates mapped to pixel locations on the map (-1 means no GPS data yet)
int LOCAL_X = -1;
int LOCAL_Y = -1;
int time_offset = 0; // Difference between watch time and UTC time from phone

// UI Elements
static Window *window;
static TextLayer *time_text_layer;
static TextLayer *date_text_layer;
static TextLayer *bottom_text_layer;
static GBitmap *world_bitmap;
static Layer *canvas;
static GBitmap *image;

// --- UI THEME CHANGER ---
static void flip_color(int sw) {
  // sw = 0 means day (White Background). Otherwise night (Black Background).
  GColor bg_color = (sw == 0) ? GColorWhite : GColorBlack;
  GColor fg_color = (sw == 0) ? GColorBlack : GColorWhite;

  // Apply the chosen theme to all text layers
  text_layer_set_background_color(time_text_layer, bg_color);
  text_layer_set_text_color(time_text_layer, fg_color);
  text_layer_set_background_color(date_text_layer, bg_color);
  text_layer_set_text_color(date_text_layer, fg_color);
  text_layer_set_background_color(bottom_text_layer, bg_color);
  text_layer_set_text_color(bottom_text_layer, fg_color);
}

static bool is_night_at(float lat, float lon, int sun_x, int sun_y) {
  int x_angle = (int)(((lon + 180.0) / 360.0) * TRIG_MAX_ANGLE);
  int y_angle = (int)(-(lat / 360.0) * TRIG_MAX_ANGLE);
  
  float angle = ((float)sin_lookup(sun_y)/(float)TRIG_MAX_RATIO) * ((float)sin_lookup(y_angle)/(float)TRIG_MAX_RATIO);
  angle = angle + ((float)cos_lookup(sun_y)/(float)TRIG_MAX_RATIO) * ((float)cos_lookup(y_angle)/(float)TRIG_MAX_RATIO) * ((float)cos_lookup(sun_x - x_angle)/(float)TRIG_MAX_RATIO);
  
  return (angle < 0);
}

// --- MAP & SHADOW GENERATION ---
static void draw_earth() {
  // 1. Calculate the current time and date progress
  int now = (int)time(NULL) + time_offset;
  float day_of_year; 
  float time_of_day; 
  int leap_years = (int)((float)now / 131487192.0);
  
  day_of_year = now - (((int)((float)now / 31556926.0) * 365 + leap_years) * 86400);
  day_of_year = day_of_year / 86400.0;
  time_of_day = day_of_year - (int)day_of_year;
  day_of_year = day_of_year / 365.0;
  
  // 2. Calculate where the sun is currently shining
  int sun_x = (int)((float)TRIG_MAX_ANGLE * (1.0 - time_of_day));
  int sun_y = -sin_lookup((day_of_year - 0.2164) * TRIG_MAX_ANGLE) * .26 * .25;
  
  // Check the 4 corners of your custom map bounds
  bool tl_night = is_night_at(43.0, -118.0, sun_x, sun_y); // Top-Left
  bool tr_night = is_night_at(43.0, -105.0, sun_x, sun_y); // Top-Right
  bool bl_night = is_night_at(36.0, -118.0, sun_x, sun_y); // Bottom-Left
  bool br_night = is_night_at(36.0, -105.0, sun_x, sun_y); // Bottom-Right

  bool all_night = (tl_night && tr_night && bl_night && br_night);
  bool all_day = (!tl_night && !tr_night && !bl_night && !br_night);

  int x, y;

  // If the terminator line is NOT crossing the map, skip the heavy trig math!
  if (all_night || all_day) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Map is entirely %s. Skipping heavy math.", all_day ? "Day" : "Night");
    
    for(x = 0; x < WIDTH; x++) {
      for(y = 0; y < HEIGHT; y++) {
  
        // Fast application of Day/Night without calculating angles
#ifdef PBL_BW
        int byte = y * gbitmap_get_bytes_per_row(world_bitmap) + (int)(x / 8);
        if (all_night ^ (0x1 & (((char *)gbitmap_get_data(world_bitmap))[byte] >> (7 - x % 8)))) {
          ((char *)gbitmap_get_data(image))[byte] |= (0x1 << (7 - x % 8));
        } else {
          ((char *)gbitmap_get_data(image))[byte] &= ~(0x1 << (7 - x % 8));
        }
#else
        int byte = y * gbitmap_get_bytes_per_row(world_bitmap) + (int)(x / 2);
        if (all_night) { 
          ((char *)gbitmap_get_data(world_bitmap))[byte] = ((char *)gbitmap_get_data(world_bitmap))[(int)(WIDTH*HEIGHT / 2) + byte];
        } else { 
          ((char *)gbitmap_get_data(world_bitmap))[byte] = ((char *)gbitmap_get_data(world_bitmap))[WIDTH*HEIGHT + byte];
        }
#endif
        if(x == LOCAL_X && y == LOCAL_Y){
          flip_color(all_night ? 1 : 0); 
        }
      }
    }
  } 
  else {
    // The sun is actively rising or setting over the map! 
    // Do the heavy per-pixel trig math to draw the curved shadow line.
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Terminator line crossing map. Calculating shadow line.");
    
    for(x = 0; x < WIDTH; x++) {
      // 1. Find the real-world longitude of this X pixel (-118 to -105)
      float pixel_lon = -118.0 + (((float)x / (float)WIDTH) * 13.0);
      
      // 2. Convert longitude to Pebble's angle system (-180...180 maps to 0...TRIG_MAX_ANGLE)
      int x_angle = (int)(((pixel_lon + 180.0) / 360.0) * TRIG_MAX_ANGLE);
      
      for(y = 0; y < HEIGHT; y++) {
        // 3. Find the real-world latitude of this Y pixel (43 to 36)
        float pixel_lat = 43.0 - (((float)y / (float)HEIGHT) * 7.0);
        
        // 4. Convert latitude to Pebble's angle system (90...-90 maps to -TRIG_MAX_ANGLE/4...+TRIG_MAX_ANGLE/4)
        int y_angle = (int)(-(pixel_lat / 360.0) * TRIG_MAX_ANGLE);
            
        // Calculate the shadow line using the spherical law of cosines
        float angle = ((float)sin_lookup(sun_y)/(float)TRIG_MAX_RATIO) * ((float)sin_lookup(y_angle)/(float)TRIG_MAX_RATIO);
        angle = angle + ((float)cos_lookup(sun_y)/(float)TRIG_MAX_RATIO) * ((float)cos_lookup(y_angle)/(float)TRIG_MAX_RATIO) * ((float)cos_lookup(sun_x - x_angle)/(float)TRIG_MAX_RATIO);
        
#ifdef PBL_BW
        // Apply 1-bit Black and White pixel logic (reads bits left-to-right)
        int byte = y * gbitmap_get_bytes_per_row(world_bitmap) + (int)(x / 8);
        if ((angle < 0) ^ (0x1 & (((char *)gbitmap_get_data(world_bitmap))[byte] >> (7 - x % 8)))) {
          ((char *)gbitmap_get_data(image))[byte] = ((char *)gbitmap_get_data(image))[byte] | (0x1 << (7 - x % 8));
        } else {
          ((char *)gbitmap_get_data(image))[byte] = ((char *)gbitmap_get_data(image))[byte] & ~(0x1 << (7 - x % 8));
        }
#else
        // Apply Color logic (grabs the shadow pixel from the bottom half of the source image)
        int byte = y * gbitmap_get_bytes_per_row(world_bitmap) + (int)(x / 2);
        if (angle < 0) { 
          ((char *)gbitmap_get_data(world_bitmap))[byte] = ((char *)gbitmap_get_data(world_bitmap))[(int)(WIDTH*HEIGHT / 2) + byte];
        } else { 
          ((char *)gbitmap_get_data(world_bitmap))[byte] = ((char *)gbitmap_get_data(world_bitmap))[WIDTH*HEIGHT + byte];
        }
#endif
        
        // 5. If we are currently evaluating the pixel where the user is standing, update the theme
        if(x == LOCAL_X && y == LOCAL_Y){
          flip_color(angle < 0 ? 1 : 0); // 1 = Night, 0 = Day
        }
      }
    }
  }
  // Tell the watch to push the updated map to the screen
  layer_mark_dirty(canvas);
}

// --- CANVAS RENDERING ---
static void draw_watch(struct Layer *layer, GContext *ctx) {
  // Center the map dynamically in case the screen is wider than the map image
  GRect bounds = layer_get_unobstructed_bounds(layer);
  int map_offset_x = (bounds.size.w - WIDTH) / 2;
  int map_offset_y = 0; 
  
  // Draw the actual generated map
  graphics_draw_bitmap_in_rect(ctx, image, GRect(map_offset_x, map_offset_y, WIDTH, HEIGHT));
  
  // Only draw the crosshair if we have successfully received GPS coordinates
  if (LOCAL_X >= 0 && LOCAL_Y >= 0) {
    // Shift the crosshair to match the map's centered offset
    int cross_x = LOCAL_X + map_offset_x;
    int cross_y = LOCAL_Y + map_offset_y;
    int gap = ODOT_SIZE;

    #if defined(PBL_COLOR)
      // Standard color crosshair
      graphics_context_set_stroke_width(ctx, CROSS_WIDTH);
      graphics_context_set_stroke_color(ctx, GColorLightGray);
      
      graphics_draw_line(ctx, GPoint(map_offset_x, cross_y), GPoint(cross_x - gap, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x + gap, cross_y), GPoint(map_offset_x + WIDTH, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x, map_offset_y), GPoint(cross_x, cross_y - gap));
      graphics_draw_line(ctx, GPoint(cross_x, cross_y + gap), GPoint(cross_x, map_offset_y + HEIGHT));

      graphics_context_set_stroke_width(ctx, ODOT_WIDTH);
      graphics_draw_circle(ctx, GPoint(cross_x, cross_y), ODOT_SIZE);
      graphics_context_set_fill_color(ctx, GColorRed);
      graphics_fill_circle(ctx, GPoint(cross_x, cross_y), IDOT_SIZE);
    #else 
      // High-contrast "Halo" crosshair for Black & White screens
      // Step A: Draw a thick white line as the background outline
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 3);
      
      graphics_draw_line(ctx, GPoint(map_offset_x, cross_y), GPoint(cross_x - gap, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x + gap, cross_y), GPoint(map_offset_x + WIDTH, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x, map_offset_y), GPoint(cross_x, cross_y - gap));
      graphics_draw_line(ctx, GPoint(cross_x, cross_y + gap), GPoint(cross_x, map_offset_y + HEIGHT));
      
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, GPoint(cross_x, cross_y), 4); 

      // Step B: Draw a thin black line right down the middle
      graphics_context_set_stroke_color(ctx, GColorBlack);
      graphics_context_set_stroke_width(ctx, 1);
      
      graphics_draw_line(ctx, GPoint(map_offset_x, cross_y), GPoint(cross_x - gap, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x + gap, cross_y), GPoint(map_offset_x + WIDTH, cross_y));
      graphics_draw_line(ctx, GPoint(cross_x, map_offset_y), GPoint(cross_x, cross_y - gap));
      graphics_draw_line(ctx, GPoint(cross_x, cross_y + gap), GPoint(cross_x, map_offset_y + HEIGHT));

      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_draw_circle(ctx, GPoint(cross_x, cross_y), 3);
      graphics_fill_circle(ctx, GPoint(cross_x, cross_y), 1);
    #endif
  }
}

// --- TICK HANDLER (Fires every minute) ---
static void handle_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
  static char time_text[] = "00:00";
  static char date_text[] = "00 Mth | 00 Day";

  // Format and update Date
  strftime(date_text, sizeof(date_text), "%m %b | %d %a", tick_time);
  text_layer_set_text(date_text_layer, date_text);

  // Format and update Time
  strftime(time_text, sizeof(time_text), "%I:%M", tick_time);
  text_layer_set_text(time_text_layer, time_text);
  
  // Only trigger the heavy map calculation if we've hit our redraw interval
  if (tick_time->tm_min % REDRAW_INTERVAL == 0) {
    draw_earth();
  }
}

// --- PHONE COMMUNICATION ---
static void app_message_inbox_received(DictionaryIterator *iterator, void *context) {
  // 1. Sync Time Offset from Phone (UTC)
  Tuple *t = dict_find(iterator, 0);
  if (t) {
    int unixtime = t->value->int32;
    int now = (int)time(NULL);
    time_offset = unixtime - now;
    persist_write_int(TIME_OFFSET_PERSIST, time_offset); 
  }

  // 2. Receive GPS Location from Phone
  Tuple *lat_tuple = dict_find(iterator, 1);
  Tuple *lon_tuple = dict_find(iterator, 2);
  
  if (lat_tuple && lon_tuple) {
    int32_t lat_val = lat_tuple->value->int32;
    int32_t lon_val = lon_tuple->value->int32;

    // Save to persistent memory so it survives watch reboots
    persist_write_int(LAT_PERSIST, lat_val);
    persist_write_int(LON_PERSIST, lon_val);

    // Reconstruct the floating point numbers (JS sends them as scaled integers)
    float lat = (float)lat_tuple->value->int32 / 10000.0;
    float lon = (float)lon_tuple->value->int32 / 10000.0;

    // Translate global Latitude/Longitude to local X/Y screen pixels
    LOCAL_X = (int)(((lon + 118.0) / 13.0) * WIDTH);
    LOCAL_Y = (int)(((43.0 - lat) / 7.0) * HEIGHT);
    
    APP_LOG(APP_LOG_LEVEL_DEBUG, "New Location Received: X:%d, Y:%d", LOCAL_X, LOCAL_Y);
  }

  // Force map to update immediately with the new data
  draw_earth();
}

// --- WINDOW BUILDER ---
static void window_load(Window *window) {
  // Define default boot colors
#ifdef BLACK_ON_WHITE
  GColor background_color = GColorBlack;
  GColor foreground_color = GColorWhite;
#else
  GColor background_color = GColorWhite;
  GColor foreground_color = GColorBlack;
#endif
  window_set_background_color(window, background_color);
  
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_unobstructed_bounds(window_layer);

  // Calculate the remaining screen space below the map
  int text_area_start = HEIGHT - TEXT_OFFSET;
  int remaining_height = bounds.size.h - text_area_start;
  
  // Divide remaining space: 60% for time, 40% for date
  int time_height = (remaining_height * 60) / 100;
  int date_height = remaining_height - time_height;

  // Build Time Text
  time_text_layer = text_layer_create(GRect(0, text_area_start, bounds.size.w, time_height));
  text_layer_set_background_color(time_text_layer, background_color);
  text_layer_set_text_color(time_text_layer, foreground_color);
  text_layer_set_font(time_text_layer, fonts_get_system_font(TIME_FONT));
  text_layer_set_text(time_text_layer, "");
  text_layer_set_text_alignment(time_text_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(time_text_layer));

  // Build Bottom Block (Acts as a solid background block behind the date)
  bottom_text_layer = text_layer_create(GRect(0, text_area_start + time_height, bounds.size.w, date_height));
  text_layer_set_background_color(bottom_text_layer, background_color);
  text_layer_set_text_color(bottom_text_layer, foreground_color);
  layer_add_child(window_layer, text_layer_get_layer(bottom_text_layer));

  // Build Date Text
  date_text_layer = text_layer_create(GRect(0, text_area_start + time_height, bounds.size.w, date_height));
  text_layer_set_background_color(date_text_layer, background_color);
  text_layer_set_text_color(date_text_layer, foreground_color);
  text_layer_set_font(date_text_layer, fonts_get_system_font(DATE_FONT));
  text_layer_set_text(date_text_layer, "");
  text_layer_set_text_alignment(date_text_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(date_text_layer));

  // Build Canvas (Sits over the whole screen, update_proc handles the actual drawing)
  canvas = layer_create(bounds);
  layer_set_update_proc(canvas, draw_watch);
  layer_add_child(window_layer, canvas);

  // Initialize the image memory correctly based on platform
#ifdef PBL_BW
  image = gbitmap_create_with_resource(RESOURCE_ID_WORLD);
#else
  image = gbitmap_create_as_sub_bitmap(world_bitmap, GRect(0, 0, WIDTH, HEIGHT));
#endif

  // Do the first map generation so it isn't blank on boot
  draw_earth();
}

static void window_unload(Window *window) {
  // Free all UI elements from memory
  text_layer_destroy(time_text_layer);
  text_layer_destroy(date_text_layer);
  text_layer_destroy(bottom_text_layer);
  layer_destroy(canvas);
  gbitmap_destroy(image);
}

// --- APP LIFECYCLE ---
static void init(void) {
  // 1. Recover saved time offset from storage
  if (persist_exists(TIME_OFFSET_PERSIST)) {
    time_offset = persist_read_int(TIME_OFFSET_PERSIST);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "loaded offset %d", time_offset);
  }
  
  // 2. Recover saved location from storage (so crosshair shows instantly)
  if (persist_exists(LAT_PERSIST) && persist_exists(LON_PERSIST)) {
    int32_t lat_val = persist_read_int(LAT_PERSIST);
    int32_t lon_val = persist_read_int(LON_PERSIST);

    float lat = (float)lat_val / 10000.0;
    float lon = (float)lon_val / 10000.0;

    LOCAL_X = (int)(((lon + 118.0) / 13.0) * WIDTH);
    LOCAL_Y = (int)(((43.0 - lat) / 7.0) * HEIGHT);
    
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Loaded saved location: X:%d, Y:%d", LOCAL_X, LOCAL_Y);
  }
  
  // Load the core map resource
  world_bitmap = gbitmap_create_with_resource(RESOURCE_ID_WORLD);
  
  // Setup Window Handlers
  window = window_create();
  window_set_window_handlers(window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(window, true);

  // Start the minute ticker
  tick_timer_service_subscribe(MINUTE_UNIT, handle_minute_tick);
  
  // Force a manual tick right away to populate the time/date
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  handle_minute_tick(tick_time, MINUTE_UNIT);

  // Setup JS communication
  app_message_register_inbox_received(app_message_inbox_received);
  app_message_open(128, 128);
}

static void deinit(void) {
  // Clean up services and window
  tick_timer_service_unsubscribe();
  window_destroy(window);
  gbitmap_destroy(world_bitmap);
}

int main(void) {
  init();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", window);
  app_event_loop();
  deinit();
}