# DeskBuddy Groot

ESP32-C3 Super Mini + 1.3" SH1106 OLED + touch pad, controlled from a phone over Bluetooth LE.

## Files
- `deskbuddy_groot.ino` firmware (libraries: Adafruit GFX, Adafruit SH110X; board ESP32C3 Dev Module, USB CDC On Boot enabled, Partition Scheme "Huge APP" if too big)
- `index.html` phone page. Web Bluetooth and the microphone need **Chrome on Android** and **https** (host it on GitHub Pages) or `localhost`.

## On the OLED
Home screen: the eyes are always alive (blink, look left/right/up). Tap = next face (26 faces), double tap = angry faces, **long press = app menu**.

| App | Controls |
|---|---|
| Dino | touch = jump, hold 1 s = exit |
| Jet (space fighter) | auto-fire. **Hold = fly up, release = fly down.** Hold 2.5 s = exit. After game over: tap = retry, long press = exit |
| Music | tap = choose button (prev / play / next / vol- / vol+), long press = press it, double tap = back |
| Text | last message typed on the phone, pages automatically if long |
| Time / Temp / Weather | tap = menu, double tap = face |

In the menu: tap = next app, long press = open, double tap = back.

## On the phone page
- **Type a message** -> shows on the OLED (optional live typing). ASCII only.
- **Voice chat** -> tap the mic: eyes listen, think, talk. Double tap / long press on the OLED cancels. Built-in replies (time, date, weather, jokes, "open dino", "play music", "be happy"...). Put an endpoint in the box (POST `{text}` -> `{reply}`) to chat with a real AI.
- **Music** -> choose songs from the phone; the OLED Music app controls them.
- **Remote** -> on-screen touch pad + app buttons.

## BLE protocol (Nordic UART)
Phone -> buddy: `T W R D E`, `M0,<chunk>` `M1,<chunk>` `M2` (message), `N0/N1/N2` (song title), `A,<play>,<vol>`, `V,<0..3>`, `K,<0|1>`, `S,<n>`.
Buddy -> phone (notify): `P,p|n|b|+|-` (music), `V,x` (cancel voice).
