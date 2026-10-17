# Input scripts

What a player does, written down in frames, for `hexlith-boot --input`. It is Hexlith's own format and nothing reads it but `application::InputScript`.

A frame is one vertical blank of the top screen, counted from zero when the process starts, the same count the golden trace uses. hid:USER asks for the input 234 times a second, about four times a frame, and each time it gets the state for the frame it is in.

## Lines

One action per line, after the frame it happens at:

```
# The language selection: the language under the cursor, then yes.
100 tap a
200 tap a
```

A `#` starts a comment that runs to the end of the line, and blank lines are ignored. Fields are separated by spaces or tabs. Frames are whole numbers from zero and may not go backwards from one line to the next, though several lines may share one.

| Action | Arguments | Effect |
| --- | --- | --- |
| `press` | one or more buttons | Holds them from this frame until a `release` |
| `release` | one or more buttons | Lets them go |
| `tap` | one or more buttons, then optionally `for <frames>` | Holds them for that many frames, four if the line does not say |
| `touch` | x and y, then optionally `for <frames>` | Puts the stylus on the bottom screen there, until a `lift` or for that many frames |
| `lift` | none | Takes the stylus off the screen |
| `circle` | x and y | Holds the circle pad at that position until the next `circle` |

The buttons are `a`, `b`, `x`, `y`, `l`, `r`, `start`, `select`, `up`, `down`, `left` and `right`, the last four being the D-pad.

Four frames is the default length because many scenes read their input every other frame, at thirty frames a second, and a press shorter than two of their frames can be missed between two reads.

## Ranges

| Field | Range | Source |
| --- | --- | --- |
| Touch x | 0 to 319 | the bottom screen is 320 pixels wide, 3dbrew LCD |
| Touch y | 0 to 239 | and 240 high |
| Circle x and y | -154 to 154, which is 0x9A | `MAX_CIRCLEPAD_POS` in Azahar's `src/core/hle/service/hid/hid.cpp`, which notes it was measured with a calibrated pad |
| Length | 1 to 1,000,000 frames | |

The circle pad reads as a direction once it is more than 40 units from its centre, the way Azahar's `GetStickDirectionState` decides it, so `circle 154 0` also holds the circle pad's right direction.

## Order

Every line becomes one or two changes: a tap or a touch with a length lets go at its frame plus the length. Changes are applied in order of frame, and at the same frame in the order of the lines they came from. So a tap that ends at a frame where a later line presses the same button leaves it held:

```
10 tap a for 4
14 press a
```

A script is read and checked whole before the run starts. A mistake names its line, and the run does not start.
