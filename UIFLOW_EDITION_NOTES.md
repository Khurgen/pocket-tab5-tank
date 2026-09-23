# UIFlow Edition Development Notes

## Goal

Create a Pocket Tank-inspired aquarium for M5Stack Tab5 using UIFlow2 MicroPython. This edition preserves the experiential ideas of the upstream project while using a MicroPython-friendly behavioral system rather than porting the native C/ESP-IDF LLM implementation.

## Runtime contract

- M5Stack Tab5
- UIFlow2 MicroPython
- 1280 x 720 landscape
- `M5.begin()`
- `Widgets.setRotation(3)`
- `m5ui.init()`
- continuous `M5.update()`
- application files under `/flash/pocket_tab5_tank/`
- Run Once until each milestone passes physical acceptance

## Release ladder

- v0.1.x Living Aquarium
- v0.2.x Fish Minds
- v0.3.x Touch
- v0.4.x Persistence
- v0.5.x Growth and Relationships
- v0.6.x Milestones
- v0.7.x Tab5 Hardware Integration

## v0.1.0 design decision

The first aquarium uses only lightweight LVGL/M5UI geometry. There are no PNG sprites. This reduces startup cost and makes the first hardware test about animation stability rather than asset decoding.

Fish are deliberately simple geometric composites: oval body, rounded tail, eye, and highlight. Their motion uses independent velocities, different vertical trajectories, bounded movement, and periodic vertical course changes.

The first acceptance gate is ten minutes of uninterrupted animation on the physical Tab5.
