# Pocket Tab5 Tank
# UIFlow2 / MicroPython
# v0.1.0 - Living Aquarium
#
# Acceptance target:
# - 1280 x 720 landscape
# - two independently moving fish
# - continuous animation for at least 10 minutes
# - no touch, persistence, audio, or AI behavior yet

import time
import M5
from M5 import *
import m5ui
import lvgl as lv

APP_NAME = "Pocket Tab5 Tank"
APP_VERSION = "0.1.0"

SCREEN_W = 1280
SCREEN_H = 720

WATER_TOP = 0
WATER_BOTTOM = 625

COLOR_WATER = 0x082B45
COLOR_WATER_2 = 0x0B3A59
COLOR_SAND = 0xC9A66B
COLOR_SAND_DARK = 0x9B7A46
COLOR_PLANT = 0x2F7D4A
COLOR_PLANT_LIGHT = 0x4FA867
COLOR_ROCK = 0x59636B
COLOR_ROCK_LIGHT = 0x707D86
COLOR_BUBBLE = 0xA9E7FF
COLOR_WHITE = 0xFFFFFF
COLOR_BLACK = 0x000000

current_page = None
ui_objects = []
fish_list = []
bubble_list = []

last_frame_ms = 0
frame_counter = 0
fps_window_ms = 0
fps_value = 0


def now_ms():
    try:
        return time.ticks_ms()
    except:
        return int(time.time() * 1000)


def diff_ms(a, b):
    try:
        return time.ticks_diff(a, b)
    except:
        return a - b


def add_obj(obj):
    ui_objects.append(obj)
    return obj


def style_obj(obj, bg, radius=0, border=0):
    obj.set_style_bg_color(lv.color_hex(bg), 0)
    obj.set_style_bg_opa(255, 0)
    obj.set_style_border_width(border, 0)
    obj.set_style_radius(radius, 0)
    try:
        obj.set_style_pad_all(0, 0)
    except:
        pass


def make_rect(parent, x, y, w, h, color, radius=0):
    obj = lv.obj(parent)
    obj.set_pos(x, y)
    obj.set_size(w, h)
    style_obj(obj, color, radius=radius, border=0)
    return add_obj(obj)


def make_circle(parent, x, y, diameter, color):
    return make_rect(parent, x, y, diameter, diameter, color, radius=diameter // 2)


def make_label(parent, text, x, y, color=COLOR_WHITE, font=None):
    kwargs = {
        "x": x,
        "y": y,
        "text_c": color,
        "bg_c": COLOR_WATER,
        "bg_opa": 0,
        "parent": parent,
    }
    if font is not None:
        kwargs["font"] = font
    label = m5ui.M5Label(str(text), **kwargs)
    return add_obj(label)


def get_font(size):
    try:
        return getattr(lv, "font_montserrat_" + str(size))
    except:
        try:
            return lv.font_montserrat_14
        except:
            return None


FONT_SMALL = get_font(14)
FONT_MEDIUM = get_font(20)


def build_background(parent):
    # Main water field
    make_rect(parent, 0, 0, SCREEN_W, SCREEN_H, COLOR_WATER, radius=0)

    # Slightly lighter lower-water band for visual depth
    make_rect(parent, 0, 430, SCREEN_W, 195, COLOR_WATER_2, radius=0)

    # Sand floor
    make_rect(parent, 0, 625, SCREEN_W, 95, COLOR_SAND, radius=0)
    make_rect(parent, 0, 625, SCREEN_W, 8, COLOR_SAND_DARK, radius=0)

    # Left plant bed
    plant_x = (80, 112, 145, 176, 205)
    plant_h = (150, 205, 175, 235, 165)
    for i in range(len(plant_x)):
        x = plant_x[i]
        h = plant_h[i]
        color = COLOR_PLANT if i % 2 == 0 else COLOR_PLANT_LIGHT
        make_rect(parent, x, 625 - h, 18, h, color, radius=9)

    # Right plant bed
    plant_x2 = (1010, 1040, 1072, 1105, 1138, 1170)
    plant_h2 = (185, 225, 150, 245, 195, 165)
    for i in range(len(plant_x2)):
        x = plant_x2[i]
        h = plant_h2[i]
        color = COLOR_PLANT_LIGHT if i % 2 == 0 else COLOR_PLANT
        make_rect(parent, x, 625 - h, 18, h, color, radius=9)

    # Central reef / rocks
    make_rect(parent, 490, 570, 150, 58, COLOR_ROCK, radius=28)
    make_rect(parent, 575, 540, 180, 88, COLOR_ROCK_LIGHT, radius=40)
    make_rect(parent, 700, 580, 120, 48, COLOR_ROCK, radius=24)

    # Bubble-column base
    make_rect(parent, 900, 594, 70, 32, COLOR_ROCK, radius=14)

    # Small title/version, deliberately unobtrusive
    make_label(parent, "Pocket Tab5 Tank  v" + APP_VERSION, 22, 18, color=0x8AC9E8, font=FONT_SMALL)


def create_fish(parent, name, x, y, vx, vy, body_color, tail_color):
    root = lv.obj(parent)
    root.set_pos(int(x), int(y))
    root.set_size(100, 52)
    root.set_style_bg_opa(0, 0)
    root.set_style_border_width(0, 0)
    try:
        root.set_style_pad_all(0, 0)
    except:
        pass
    add_obj(root)

    body = make_rect(root, 18, 6, 70, 40, body_color, radius=20)
    tail = make_rect(root, 0, 13, 28, 27, tail_color, radius=10)
    eye = make_circle(root, 68, 16, 8, COLOR_BLACK)
    highlight = make_circle(root, 70, 17, 3, COLOR_WHITE)

    fish = {
        "name": name,
        "root": root,
        "body": body,
        "tail": tail,
        "eye": eye,
        "highlight": highlight,
        "x": float(x),
        "y": float(y),
        "vx": float(vx),
        "vy": float(vy),
        "w": 100,
        "h": 52,
        "dir": 1 if vx >= 0 else -1,
        "turn_timer": now_ms(),
    }
    orient_fish(fish)
    fish_list.append(fish)
    return fish


def orient_fish(fish):
    # The fish is assembled from simple LVGL shapes. Repositioning the tail
    # and eye is enough to make direction changes obvious without images.
    if fish["vx"] >= 0:
        fish["dir"] = 1
        fish["body"].set_pos(18, 6)
        fish["tail"].set_pos(0, 13)
        fish["eye"].set_pos(68, 16)
        fish["highlight"].set_pos(70, 17)
    else:
        fish["dir"] = -1
        fish["body"].set_pos(8, 6)
        fish["tail"].set_pos(72, 13)
        fish["eye"].set_pos(24, 16)
        fish["highlight"].set_pos(26, 17)


def create_bubble(parent, x, y, size, speed):
    bubble = make_circle(parent, int(x), int(y), int(size), COLOR_BUBBLE)
    try:
        bubble.set_style_bg_opa(90, 0)
    except:
        pass

    data = {
        "obj": bubble,
        "x": float(x),
        "y": float(y),
        "size": int(size),
        "speed": float(speed),
    }
    bubble_list.append(data)
    return data


def update_fish(fish, dt):
    fish["x"] += fish["vx"] * dt
    fish["y"] += fish["vy"] * dt

    # Aquarium bounds. Fish stay out of the sand and away from the top edge.
    min_x = 18
    max_x = SCREEN_W - fish["w"] - 18
    min_y = 70
    max_y = WATER_BOTTOM - fish["h"] - 25

    changed_direction = False

    if fish["x"] <= min_x:
        fish["x"] = min_x
        fish["vx"] = abs(fish["vx"])
        changed_direction = True
    elif fish["x"] >= max_x:
        fish["x"] = max_x
        fish["vx"] = -abs(fish["vx"])
        changed_direction = True

    if fish["y"] <= min_y:
        fish["y"] = min_y
        fish["vy"] = abs(fish["vy"])
    elif fish["y"] >= max_y:
        fish["y"] = max_y
        fish["vy"] = -abs(fish["vy"])

    # Gentle deterministic wandering. Every few seconds each fish changes its
    # vertical bias independently. No random-module dependency is required.
    t = now_ms()
    interval = 2800 if fish["name"] == "Ada" else 3900
    if diff_ms(t, fish["turn_timer"]) >= interval:
        fish["turn_timer"] = t
        if fish["name"] == "Ada":
            fish["vy"] = -fish["vy"] * 0.85
            if abs(fish["vy"]) < 18:
                fish["vy"] = 22 if fish["dir"] > 0 else -22
        else:
            fish["vy"] = -fish["vy"] * 1.05
            if abs(fish["vy"]) < 14:
                fish["vy"] = -18 if fish["dir"] > 0 else 18

    if changed_direction:
        orient_fish(fish)

    fish["root"].set_pos(int(fish["x"]), int(fish["y"]))


def update_bubbles(dt):
    for bubble in bubble_list:
        bubble["y"] -= bubble["speed"] * dt
        if bubble["y"] < 45:
            bubble["y"] = 590
        bubble["obj"].set_pos(int(bubble["x"]), int(bubble["y"]))


def setup_aquarium():
    global current_page
    global last_frame_ms, fps_window_ms, frame_counter, fps_value

    print(APP_NAME + " v" + APP_VERSION + ": setup start")

    current_page = m5ui.M5Page(bg_c=COLOR_WATER)
    current_page.screen_load()

    build_background(current_page)

    # Two independently moving fish with distinct speeds and trajectories.
    create_fish(
        current_page,
        "Ada",
        260,
        210,
        92,
        24,
        0xF28C45,
        0xD86432,
    )

    create_fish(
        current_page,
        "Darwin",
        830,
        390,
        -68,
        -17,
        0x48B8A0,
        0x2A8A78,
    )

    # Animated bubble column.
    create_bubble(current_page, 915, 520, 16, 38)
    create_bubble(current_page, 936, 430, 11, 47)
    create_bubble(current_page, 922, 330, 14, 34)
    create_bubble(current_page, 946, 240, 9, 52)

    last_frame_ms = now_ms()
    fps_window_ms = last_frame_ms
    frame_counter = 0
    fps_value = 0

    print(APP_NAME + " v" + APP_VERSION + ": living aquarium ready")
    print("Acceptance test: leave running for at least 10 minutes")


def setup():
    M5.begin()
    Widgets.setRotation(3)
    m5ui.init()
    setup_aquarium()


def loop():
    global last_frame_ms, frame_counter, fps_window_ms, fps_value

    M5.update()

    t = now_ms()
    elapsed = diff_ms(t, last_frame_ms)

    # Cap simulation steps so a temporary pause does not launch objects across
    # the tank when execution resumes.
    if elapsed < 0:
        elapsed = 0
    if elapsed > 100:
        elapsed = 100

    # Aim for roughly 30 simulation updates per second.
    if elapsed >= 33:
        dt = elapsed / 1000.0
        last_frame_ms = t

        for fish in fish_list:
            update_fish(fish, dt)

        update_bubbles(dt)

        frame_counter += 1

    if diff_ms(t, fps_window_ms) >= 5000:
        span = diff_ms(t, fps_window_ms)
        if span > 0:
            fps_value = int((frame_counter * 1000) / span)
        print("aquarium alive | approx updates/sec:", fps_value,
              "| Ada:", int(fish_list[0]["x"]), int(fish_list[0]["y"]),
              "| Darwin:", int(fish_list[1]["x"]), int(fish_list[1]["y"]))
        fps_window_ms = t
        frame_counter = 0

    try:
        time.sleep_ms(5)
    except:
        time.sleep(0.005)


if __name__ == "__main__":
    try:
        setup()
        while True:
            loop()
    except (Exception, KeyboardInterrupt) as e:
        try:
            m5ui.deinit()
        except:
            pass
        try:
            from utility import print_error_msg
            print_error_msg(e)
        except:
            print("Pocket Tab5 Tank error:", e)
