/* The interface's widgets, decompiled one function at a time.
 *
 * This is a different standard of evidence from the rest of src/app. The
 * screens there were reimplemented from decompiled coordinate tables and from
 * tapping the hardware, and checked by comparing rendered frames against
 * captures - which proves only the states that were captured. What goes in
 * here is read out of the ARM, reimplemented, and run against the original
 * under Unicorn over a spread of inputs. A function without a passing
 * differential test does not belong in this file.
 */
#ifndef DS10_APP_WIDGET_H
#define DS10_APP_WIDGET_H

#include <stdint.h>

/* A knob's value as an angle, from 0x02055b98 - what every screen with a knob
 * calls to give one a value.
 *
 * The unit is 1/0x10000 of a turn, and the result is truncated to signed 16
 * bits because that is the width of the field the original stores it in
 * (+0x82); at the bottom of a 270 degree sweep it comes out negative.
 *
 *   half  = (sweep / 2) * 0x10000 / 360
 *   angle = (0x10000 - half) + 2*half * (value - min) / (max - min)
 *
 * Two things here are easy to get wrong, and this reimplementation had both
 * wrong before the function was read:
 *
 *   the value is NOT clamped. The original clamps separately, into the field
 *   at +0x28, and computes the angle from what it was given - so a value past
 *   the end sweeps the pointer past the end of the scale;
 *   the divide is the compiler's signed helper at 0x0200c158, which truncates
 *   toward zero, not toward minus infinity.
 *
 * `reverse` is the widget's +0xA4, which every screen leaves clear; set, the
 * sweep runs the other way. `sweep` is 270 everywhere in this ROM.
 */
int16_t ds10_widget_knob_angle(int min, int max, int value, int sweep,
                               int reverse);

/* The pieces of that the original also writes into the widget, exposed
 * because they are what a test compares and what a caller drawing a scale
 * would want: the half sweep, and the value clamped into [min, max]. */
int32_t ds10_widget_knob_half(int sweep);
int32_t ds10_widget_clamp(int min, int max, int value);

#endif /* DS10_APP_WIDGET_H */
