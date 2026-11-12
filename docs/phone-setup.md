# Android Phone Alarm Setup (MacroDroid + ntfy)

Configure an Android phone to wake you at night within seconds of a mains outage by repeating a siren on the `Alarm` audio stream that overrides Do Not Disturb (DND), with clean ways to silence it from the full-screen `"Power is OFF"` stop screen, the standalone notification, or the device web page.

## At a glance

- **Loud outage alarm (`power_off`):** Sets `Alarm` volume to `100%`, posts a standalone persistent high-priority notification (`Mains electricity is OFF.`), opens the full-screen `"Power is OFF"` stop screen above the lock screen (waking the screen alongside `Keep Device Awake` for up to `15 min`) with a **`Stop alarm`** button, and loops `Play: Hotline` every `2 s` on the **Alarm** audio stream (capped at `450` cycles / `15 min`).
- **Test notification (`test_alarm`):** Posts a single non-persistent `Power Monitor` / `Power monitor test received` test notification without the siren (normal notification sound only; does not change volume or override DND).
- **Stop paths (`Stop alarm` button on the `"Power is OFF"` screen or standalone notification, `power_on`, `alarm_stop`):** Stops the loop, stops any playing sound, hides the `"Power is OFF"` screen, releases screen keep-awake, and clears the outage notification. Swiping the MacroDroid notification away on Android 14+ does **not** stop the siren.

Reserve static DHCP IP addresses on your router for both the phone and the ESP32 (`power-monitor.local` mDNS did not resolve on the tested Pixel 8). Because MacroDroid's free tier depends on advert-topped "free days" that must not expire before an outage, **MacroDroid Pro** (one-off purchase) is recommended.

| Channel | Direction | Requirements | Behaviour |
|---|---|---|---|
| Local HTTP (`Local Notify URL`) | ESP32 -> Phone (`:8080`) | Phone on home Wi-Fi; fixed IPs for phone and ESP32 | Works without internet; delivers `power_off`, `power_on`, `test_alarm`, and `alarm_stop` to MacroDroid |
| `ntfy.sh` (`Cloud Webhook URL`) | ESP32 -> `ntfy.sh` -> ntfy app | `cloud_enabled` switched On; internet access on ESP32 and phone | Off by default at home; away-from-home backup that alerts independently via ntfy's own insistent `Max priority` notification |

## Step-by-step MacroDroid setup (verified on v6.1.5)

MacroDroid imports `.mdr` backups rather than JSON descriptions, so build the variable, Action Block, and macro directly in the MacroDroid UI.

### 1. Create the global variable

Under **Home -> Global Variables**, create one variable:

| Variable | Type | Initial value | Purpose |
|---|---|---|---|
| `pm_alarm_ringing` | Boolean | `False` | Keeps the siren loop running until any stop path sets it to `False` |

### 2. Create Action Block: `Stop alarm`

Under **Home -> Action Blocks**, create **`Stop alarm`** with these five actions in order:

1. **Variables -> Set Variable:** `pm_alarm_ringing` (Global Boolean) = `False`.
2. **Media -> Play/Stop Sound:** `[Stop Existing Sound]` (`Stop Sounds`).
3. **User Interface -> Hide Custom Scene:** scene name `Power is OFF`, match mode **Matches**.
4. **Device Settings -> Keep Device Awake:** **Disable Keep Awake**.
5. **Notification -> Clear Notifications:** match text *Contains* `Mains electricity is OFF.` for application **MacroDroid** (leave *Ignore ongoing notifications* unchecked).

### 3. Build Macro: `Power Outage Alarm`

#### Triggers (4 total)

1. **Local outage:** *Connectivity -> HTTP Server Request*, port `8080`, path `power_off` (`http://<phone-ip>:8080/power_off`; answers `200 OK` to `GET` and `POST`).
2. **Local restore:** *Connectivity -> HTTP Server Request*, path `power_on` (`http://<phone-ip>:8080/power_on`).
3. **Local stop (required):** *Connectivity -> HTTP Server Request*, path `alarm_stop` (`http://<phone-ip>:8080/alarm_stop`). Sent whenever the ESP32 leaves `ARMED` for `DISARMED` or `SNOOZED`, or restarts an active snooze.
4. **Local test (required):** *Connectivity -> HTTP Server Request*, path `test_alarm` (`http://<phone-ip>:8080/test_alarm`). Sent by **Test Alarm** on the device web page (or a `2–5 s` `BOOT` hold when the enclosure is open).
   - *Do not omit `alarm_stop` or `test_alarm`:* MacroDroid drops unconfigured HTTP paths without replying, causing 3 failed retries (`~25 s` block) in the ESP32 notification worker.

#### Actions (22 total)

```text
If (Trigger Fired: HTTP Server Request: http://<phone-ip>:8080/power_off)
    MacroDroid Specific -> Cancel Macro Actions: [This Macro] - other instances only
    Volume -> Volume Change: Alarm = 100%
    Notification -> Display Notification:
        Title: "POWER OUTAGE DETECTED!"
        Text: "Mains electricity is OFF."
        Channel: "Notification action (High priority)"
        Persistent notification: Checked
        Re-show if swiped away: Checked
        Prevent Android 16+ grouping: Checked
        ADD ACTION BUTTON:
            Label: "Stop alarm"
            Run when pressed: Action Block "Stop alarm"
            Clear notification on press: Checked
    Variables -> Set Global Variable (Boolean): pm_alarm_ringing = True
    Device Settings -> Keep Device Awake: Screen On - For Fixed Time (15 minutes)
    User Interface -> Display Custom Scene: "Power is OFF" (private scene)
        Display Mode: Full Screen (Activity)
        Show on lock screen: Checked
        Close scene on back press/swipe: Unchecked
        Close scene on home/task switch: Unchecked
        Close scene on background/non-interactive press: Unchecked
        Block next actions until scene closed: Unchecked
        Background colour: #FF990000
        Text: "Power is OFF" and "Mains power lost. Check your equipment."
        Button: "Stop alarm" -> Action Block "Stop alarm" (Close scene on press: Checked)
    Variables -> Set Local Variable (Integer): pm_loop = 0
    Conditions/Loops -> Repeat actions (While 'condition' repeat):
        Condition: pm_alarm_ringing = True AND pm_loop < 450
        Media -> Play/Stop Sound: Hotline, Audio Stream = Alarm
            (uncheck "Block next actions until complete")
        Wait Before Next Action: 2 seconds (check "Use alarm")
        Variables -> Set Local Variable (Integer): pm_loop = (+1)
    End Loop
    If (pm_alarm_ringing = True)
        Action Block: Stop alarm
    End If

Else If (Trigger Fired: HTTP Server Request: http://<phone-ip>:8080/test_alarm)
    Notification -> Display Notification:
        Title: "Power Monitor"
        Text: "Power monitor test received"
        Persistent notification: Unchecked

Else
    MacroDroid Specific -> Cancel Macro Actions: [This Macro] - other instances only
    Action Block: Stop alarm
End If
```

Notes verified on Pixel 8 (MacroDroid 6.1.5):

- **No overlapping loops:** Both `power_off` and `Else` (`power_on` / `alarm_stop`) start with `Cancel Macro Actions` (`other instances only`), preventing a second `power_off` from starting a parallel siren loop and preventing an in-flight `power_off` startup from racing ahead of `Stop alarm`.
- **Standalone notification (`Prevent Android 16+ grouping`):** Checking `Prevent Android 16+ grouping` keeps `Mains electricity is OFF.` separate from MacroDroid's persistent service notification so the `Stop alarm` button is directly visible without expanding a group.
- **Full-screen stop scene and keep-awake:** Using `Full Screen (Activity)` with `Show on lock screen` checked and all `Close scene on ...` options unchecked wakes the screen together with `Keep Device Awake` (`15 min`) and displays `"Power is OFF"` above the lock screen (no separate `Screen On` action is needed; Android 16 rejects MacroDroid's `TURN_SCREEN_ON`). Leaving `Block next actions until scene closed` unchecked lets the siren loop start immediately, `Keep Device Awake` (`15 min`) matches the `450 x 2 s` loop cap, and the post-loop `If pm_alarm_ringing = True` check cleans up the scene and wake lock if the 15-minute cap expires unanswered.
- **15-minute safety cap:** MacroDroid's `While 'condition' repeat` has no built-in iteration limit, so `pm_loop < 450` (`450 x 2 s = 900 s = 15 min`) prevents an unattended siren from ringing indefinitely.
- **Exact-alarm permission:** Checking **Use alarm** on `Wait Before Next Action` prompts for Android's **Alarms & reminders** (`SCHEDULE_EXACT_ALARM`) permission. Grant it to MacroDroid so the `2 s` loop stays reliable while the phone is asleep.
- **Action button binding:** MacroDroid 6.1.5 has no separate *Notification Button* trigger; bind `Stop alarm` directly inside **Display Notification -> ADD ACTION BUTTON -> Run when pressed**.

### 4. Override Do Not Disturb on the notification channel

Keep the phone's global DND mode untouched. Instead, open Android **Settings -> Apps -> MacroDroid -> Notifications -> `Notification action (High priority)`** (channel `action_notification_high_priority`) and enable **Override Do Not Disturb** (with High/Urgent importance). The **Alarm** audio stream also bypasses DND by Android policy.

## Away-from-home backup (`ntfy.sh`)

At home, the phone on the same Wi-Fi is alerted directly via **Local Notify URL**, so leave **Internet alerts (ntfy)** switched **Off** in the ESP32 **Device Settings** card. Switch it **On** on the device page only when you are away from home and need remote alerts over cellular; your saved URL stays stored when switched off.

1. Install **ntfy** (`io.heckel.ntfy`) from Google Play or F-Droid.
2. Generate a random, unguessable topic name with no personal details (for example `pm-` followed by 16 random lowercase letters and digits). Anyone who knows the topic can publish to it.
3. Subscribe to that topic in **ntfy**, open its subscription settings, and enable **Instant delivery** (*Instant delivery in doze mode*) so pushes arrive immediately without FCM batching.
4. In ntfy **Settings -> Notifications**, enable **Keep alerting for highest priority**, and in Android notification channel settings for **ntfy -> `Max priority` (`ntfy-max`)** enable **Override Do Not Disturb** so `Priority: urgent` pushes ring continuously until you dismiss the notification.
5. Firmware payload behaviour on `ntfy.sh` (independent of MacroDroid):
   - `POWER_OFF`: Title `Power Outage Alert`, Priority `urgent`, body `Event: POWER_OFF\nDevice: <name>\nDuration: <n>s`.
   - `POWER_ON` (sent only when `alert_mode = 1`): Title `Power Restored`, Priority `default`, body `Event: POWER_ON\nDevice: <name>\nDuration: <n>s` (informational push).
   - `TEST_ALARM`: Title `Power Monitor Test`, Priority `default`, body `Event: TEST_ALARM\nDevice: <name>\nDuration: 0s`.

## Link the ESP32 and verify

In the ESP32 web dashboard (**Device Settings**):

1. Set **Local Notify URL (Phone)** to `http://<phone-ip>:8080/{event}` (`{event}` expands to `power_off`, `power_on`, `test_alarm`, or `alarm_stop`).
2. Set **Cloud Webhook URL** to `https://ntfy.sh/<your-random-topic>` and keep **Internet alerts (ntfy)** **Off** while at home (switch it **On** only when away from home).
3. Choose **Alert Tracking Policy**: `0` (`Outage only (no internet alert on power return)`, default) or `1` (`Both`, if you also want an informational `POWER_ON` push via `ntfy` when away).
4. Tap **Save Device Settings**, then press **Test Alarm** – verify the status card shows `Phone: OK (test ...)` (and `Cloud: off` while Internet alerts are off) and `"Power monitor test received"` pops up on the phone.

## Night reliability checklist (battery and permissions)

Both MacroDroid and ntfy display in-app warnings when battery optimization is active, which can kill the HTTP listener or delay pushes overnight.

- **Battery usage (required):** Place both **MacroDroid** and **ntfy** on the battery-optimisation whitelist with background usage allowed (*Settings -> Apps -> MacroDroid* and *Settings -> Apps -> ntfy -> App battery usage* -> **Unrestricted**), and turn off *Pause app activity if unused*.
  - *Samsung One UI:* Add both apps to **Never auto sleeping apps** and disable *Put unused apps to sleep*.
  - *Xiaomi MIUI / HyperOS:* Enable **Autostart**, set *Battery Saver* to **No restrictions**, and lock MacroDroid in Recent Apps.
- **Keep the phone on its charger overnight** so Android Doze does not throttle background sockets or timers.
- **MacroDroid permissions:**
  - *Accessibility service* (*Settings -> Accessibility -> MacroDroid*) – required by `Display Custom Scene` to show `"Power is OFF"` over the lock screen and system apps without warnings.
  - *Display over other apps* (`SYSTEM_ALERT_WINDOW`, *Settings -> Apps -> Special app access -> Display over other apps -> MacroDroid*) – allows MacroDroid to launch the full-screen `"Power is OFF"` activity (`Full Screen (Activity)`) from the background over the lock screen.
  - *Alarms & reminders* (`SCHEDULE_EXACT_ALARM`) – for `Wait 2 seconds (Use alarm)`.
  - *Post notifications* (Android 13+).
  - *Notification access* (`BIND_NOTIFICATION_LISTENER_SERVICE`, *Settings -> Apps -> Special app access -> Notification read, reply & control -> MacroDroid*) – needed only so `Clear Notifications` in `Stop alarm` can remove MacroDroid's own outage notification.
  - *Local network permission* (Android 16+, if prompted).
  - Keep MacroDroid's persistent foreground service notification enabled inside MacroDroid settings (MacroDroid listens on its local HTTP server; no macro trigger reads notifications from other apps).

## How to stop the siren

Silence the MacroDroid siren in any of these ways (for an independent `ntfy` alert, dismiss or swipe away the `ntfy` notification). Note that **swiping the MacroDroid notification away does not stop the siren on Android 14+**:

- **On the `"Power is OFF"` screen (main stop control):** Tap the large **`Stop alarm`** button on the full-screen `"Power is OFF"` screen (displays over the lock screen as well as when unlocked; `Back` and outside taps do not close it). The ESP32 remains `ARMED`.
- **From the notification shade:** Pull down the shade and tap **`Stop alarm`** directly on the standalone `POWER OUTAGE DETECTED!` / `Mains electricity is OFF.` notification – no group expansion is needed. The ESP32 remains `ARMED`.
- **From the device web page:** Open `http://<esp32-ip>/` and tap **Disarm** or **Snooze** (or call `POST /api/disarm` / `/api/snooze`). Leaving `ARMED` (or restarting a snooze) sends `alarm_stop` to `Local Notify URL`, running `Stop alarm`. (The ESP32's physical `BOOT` button sits inside the closed enclosure and is not reachable in normal use.)
- **Automatically on power return:** Once mains has stayed on for the restore cooldown (`5 s` by default), the ESP32 sends `power_on` locally to `Local Notify URL`, running `Stop alarm`.

## Testing from a PC

Use [`tools/mobile_alarm_sim.py`](../tools/mobile_alarm_sim.py) or `curl` to test each phone trigger without unplugging mains power:

```bash
# 1. Test notification (no siren) and full local siren cycle
python tools/mobile_alarm_sim.py http test_alarm --target "http://<phone-ip>:8080/{event}"
python tools/mobile_alarm_sim.py http power_off --target "http://<phone-ip>:8080/{event}"
curl -X POST http://<phone-ip>:8080/alarm_stop
python tools/mobile_alarm_sim.py http power_on --target "http://<phone-ip>:8080/{event}"

# 2. Cloud ntfy backup push (urgent outage alert and informational restore push)
python tools/mobile_alarm_sim.py ntfy power_off --topic <your-random-topic>
python tools/mobile_alarm_sim.py ntfy power_on --topic <your-random-topic>
```
