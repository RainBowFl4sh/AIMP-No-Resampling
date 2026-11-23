# Changelog

## 2.8.1

Up to 37 % faster sample rate changes than 2.8 with Voicemeeter (1.64 → 1.03 s of silence, ASIO into Voicemeeter Virtual ASIO) and up to 24 % without Voicemeeter (1.29 → 0.98 s from track start until the music plays again, DirectSound to the DAC; WASAPI exclusive 14 % faster). Measured with the same settings on Windows 11 with a FiiO KA11.

New:
- Switching the plugin off - on the General tab, in AIMP's menu or with the hotkey - puts AIMP, Voicemeeter and the Windows devices back to the rates they had before the plugin changed them, right away (until now only when AIMP closed). A playing track continues at the same position
- Advanced tab: "Back to the start-up rates after a pause of (s)". When nothing plays for this long (stopped or paused, 30 s by default), AIMP, Voicemeeter and the devices go back to the rates from before, so other programs run at your usual rate again; the next track switches as usual. Off by default
- Advanced tab: "Switch before the next track starts" (off by default). Near the end of a track the plugin looks at the next one; if it needs another rate, AIMP stops after the current track, the plugin switches and then starts the next track, so it is never played at the old rate first. For automatic track changes; AIMP's own "stop after track" is left alone. With AIMP's crossfade it looks earlier (before the crossfade would begin); the current track then ends without being mixed into a next track with another rate - the tab and the log say so
- Voicemeeter tab: with "Switch all hardware outputs" ticked, each of A2-A5 can be ticked off on its own (only the outputs of your Voicemeeter version are shown). A slow device you rarely use no longer delays every change. The tab notes that every extra output makes switching slower
- The log shows how long a sample rate change takes: the time stamps have milliseconds, and every change ends with a line "Timing: ..." with each step (stopping playback, devices, Voicemeeter, AIMP's rate, the pause before playback continues) and how long the music was silent

Faster:
- Windows devices behind Voicemeeter (A1-A5) are switched at the same time instead of one after the other - Windows needs about a third of a second per device
- AIMP's rate: no fixed steps any more - every step goes on as soon as AIMP is ready, so the change takes only as long as the PC needs. The music continues as soon as AIMP confirms the new rate; its hidden preferences close in the background
- AIMP's rate changes at the same time as the devices behind it (Windows formats, Voicemeeter's engine) instead of afterwards
- The log shows for every device how long Windows took
- Without a running Voicemeeter, every track was about 0.17 s slower: the plugin tried to connect to Voicemeeter each time. It now does so only while a Voicemeeter program runs

Fixed:
- DirectSound or WASAPI straight to a device that Voicemeeter uses on one of its outputs (A1-A5): the plugin no longer changes that device's Windows format, which made AIMP report "Device is busy". The log names the device once

## 2.8.0

New:
- ASIO, WASAPI exclusive and DirectSound now follow the track without restarting AIMP. AIMP plays these outputs with its own fixed rate (Preferences → Sound Output); the plugin changes that rate for every track the way you would - pick the rate on AIMP's Sound Output page and press Apply - but invisibly in the background and in well under a second, without editing AIMP.ini. The devices behind AIMP come first (Voicemeeter's engine rate and its A1-A5 devices, the Windows device for DirectSound), then AIMP's rate, and then the plugin checks AIMP's setting. Playback goes on (it only stops for a moment when devices behind AIMP change, as in WASAPI shared mode). Always on while the plugin is enabled on the General tab - nothing to set up
- Only rates AIMP offers for the device are used: the plugin reads AIMP's own rate list for the output. If the device cannot play the track's rate, AIMP gets the best rate it offers - a multiple of the track's rate or the next lower rate of the same family (192 → 96 → 48 kHz); with WASAPI exclusive the device is asked as well
- Voicemeeter Virtual ASIO runs at Voicemeeter's engine rate, so AIMP has to use exactly that rate: the plugin checks both after the switch. If AIMP cannot take the new rate, the devices go back to AIMP's rate, so music never plays too fast or too slow
- While AIMP's preferences are open, the plugin does not touch them; AIMP's rate follows as soon as you close them. The program you work in keeps the focus, and AIMP's preferences still open on the page you used last
- Advanced tab: "AIMP rate changes" counts the changes without restart

Changed:
- The experimental AIMP restart of earlier versions is now the legacy method on the ASIO / Exclusive tab ("Restart AIMP instead"). It is off by default, also after an update from a version where the old option was ticked; while it is ticked, it replaces the new method
- The message after an update no longer says that AIMP was restarted (it is also shown when the plugin file was copied by hand)

## 2.7.0

New:
- Quick switch: AIMP's main menu and the menu of the tray icon have a "Prevent Resampling" entry with "Enabled" (switches the plugin on or off without opening the settings), "Audio chain..." and "Settings...". The same is available for hotkeys under Preferences → Player → Hot keys, group "Prevent Resampling": "Switch on / off" and "Show the audio chain". No keys are set by default, so nothing clashes with AIMP's own hotkeys - pick the ones you like. AIMP shows "Prevent Resampling is on" or "... is off" for two seconds, and switching on checks the playing track right away
- Highest sample rate (General tab): No limit (default), 384, 192, 96 or 48 kHz - for DACs, USB or Bluetooth devices and Voicemeeter set-ups that should not run at very high rates. Higher rates are lowered within their family (192 → 96, 176.4 → 88.2 kHz) and AIMP converts such tracks to that rate; the log says "limited to ... (highest rate setting)" and the audio chain shows the conversion. Windows only: AIMP for Linux plays every track at its own rate, so the list is greyed out there
- Exceptions (General tab): "Do not switch for lossy formats" (MP3, AAC, Ogg Vorbis, Opus, WMA, Musepack ... - recognised by AIMP's codec name, so FLAC or ALAC in an .ogg or .m4a container still switches) and a list of folders or playlists that are never switched, e.g. "D:\Audiobooks; Podcasts". A full path also covers its subfolders, a name matches every folder and playlist of that name; "Folder..." adds a folder with the folder dialog. These tracks play at the current rate, and the log says why
- Bit depth (Output tab, off by default): "Bit depth follows the track too" sets the Windows format of the device to the track's bit depth as well (16, 24 or 32 bit) in WASAPI shared mode, if the driver supports it - otherwise the bit depth stays as it is. Not used with Voicemeeter (it mixes in 32-bit float) and not for lossy formats
- Bit-perfect status and audio chain (Output tab): the tab now scrolls. Below the settings a sign and one line, centred, show whether the playing track reaches your device bit-perfect - and if not, why (e.g. "Not bit-perfect: AIMP equalizer" or "AIMP resamples 192 kHz → 96 kHz"). Below it follows the audio chain: the whole way of the playing track from the file to the device you hear, step by step, each with its own symbol and details - the track (file, format, channels, bit rate, sample rate, bit depth), AIMP's sound processing (equalizer with its strongest band, sound effects, ReplayGain / normalization, crossfade, gapless, resampling), AIMP's volume, AIMP's output (device, type, rate and who set it), Windows (default format, mixing, volume, audio enhancements) or PipeWire (graph rate), Voicemeeter (engine rate), the device itself and the result. The sign is green for bit-perfect, yellow when the rate is kept but something changes the samples ("No resampling, but not bit-perfect: ...") or only hints apply, red when the track is resampled anywhere. Values that change the signal are red, hints yellow - drawn by the plugin itself, so a coloured skin or AIMP's accent colour does not tint them. The first step already shows at the bottom edge of the tab, so it is clear there is more below; it follows the playing track while the tab is open
- Audio chain window (in the menus or by hotkey): the same status and chain in a window of its own, looking exactly like on the Output tab. It is as high as the chain (up to the height of the screen), so nothing needs scrolling, and it updates itself - every two seconds while a track plays and at once when a track starts, pauses or stops - so it only has a Close button. It does not block AIMP: you can keep using the player (and the plugin's menu and hotkeys) while it is open; opening it again brings it to the front. It grows and shrinks with the chain (another track, other settings), and after an AIMP restart by the plugin it comes back at the same place

Improved:
- Output changes are noticed right away: another output device in AIMP, a new Windows default device (e.g. headphones plugged in) or another device on Voicemeeter's A1 - the playing track is checked again and switched if needed, instead of only with the next track. A device that keeps changing is checked at most three times per track
- Linux: before each switch the plugin reads PipeWire's forced rate (clock.force-rate) instead of trusting the rate it set last. If PipeWire was restarted or another program changed it (e.g. while the plugin was switched off), the rate is set again
- "Restart AIMP now" (bit-perfect mode) and the restart for ASIO / exclusive: after the restart the settings open again on the same tab and an open audio chain window comes back, so you are back where you were
- Voicemeeter is only mentioned where it is installed: without it, the Voicemeeter tab is hidden (on Linux as well), and the warning on the General tab, the statistics and the audio chain do not mention it. Where it is installed, the audio chain always shows it - as "Not involved" when AIMP does not play into it
- Settings page: the timing options (automatic timing, settle time) are now on the Advanced tab and the Voicemeeter note about its equalizer and effects is on the Voicemeeter tab, to make room for the new options. The explanation of the AIMP output list on the Output tab is now its tooltip
- Smaller plugin: the built-in translations and the changelog are stored gzip-compressed and only unpacked when needed, and the plugin is built for size. The Windows DLLs are about 30 % smaller (64 bit 897 → 613 KB, 32 bit 1009 → 723 KB), the Linux plugin about 40 % (604 → 352 KB)
- Tests: the build test on GitHub also runs the plugin with a simulated Voicemeeter under Wine (engine rate, highest rate setting, output change on A1, restoring the engine rate when AIMP closes)

Fixed:
- Restoring when AIMP closes: after a restart by the plugin (ASIO / exclusive, Restart AIMP now), the original rates were not restored if the plugin was disabled in the restarted AIMP or no track was played there - Windows and Voicemeeter stayed at the last track's rate. They are now always restored, whether the plugin is enabled or not
- AIMP 4.70: closing AIMP's preferences showed "Invalid pointer operation", and the preferences could not be opened again until AIMP was restarted

## 2.6.1

New:
- About tab: a "Support this plugin" link (PayPal donation, the same as for the Discord Rich Presence plugin), quietly at the right edge next to the author line
- Portable AIMP: the settings file is now kept in AIMP's profile folder next to AIMP.ini. For an installed AIMP this is %APPDATA%\AIMP as before (Linux: ~/.config/AIMP), nothing changes there. A portable AIMP keeps the plugin's settings in its own Profile folder instead of %APPDATA%\AIMP on the computer it runs on. Settings from an older version are taken over once when the profile folder has none yet (the old file stays for an installed AIMP that may still use it)

Fixed:
- Update check: the GitHub repository will be renamed from AIMP-No-Resmapling to AIMP-No-Resampling. The update check asks under the new name first and under the old one as long as the repository still has it, and accepts packages from both, so updates keep working through the plugin before and after the rename
- Voicemeeter with an ASIO device on A1: the plugin sets Voicemeeter's "ASIO follows the engine rate" option (Option.ASIOsr) so the ASIO driver follows the track, but it never set it back. It now remembers the previous value and restores it when AIMP closes (also after restarts by the plugin, which hand it over to the new AIMP)
- Internet radio and other streams (http://, https://, mms:// and other URLs) are never switched any more. Switching stops and restarts playback, which for a live stream means reconnecting - and with the restart option for ASIO / WASAPI exclusive / DirectSound a whole AIMP restart on every station change. The log names only the station's server, never the path, a listening key or a password. Local files work as before
- Log at start-up: "AIMP output at start-up: Windows Default" was logged even when another output was selected - AIMP opens its output only after loading the plugins. The log now shows the output from AIMP's settings; the output really used is logged with every track as before
- Bit-perfect mode: it also worked while the plugin was disabled on the General tab, and ticking it or its options still changed AIMP's settings. The mode and its options (volume to 100 %, no gapless playback, "Restart AIMP now") are now greyed out as long as "Enable Prevent Resampling" is not ticked, with a note that the mode only works while the plugin is enabled. A disabled plugin leaves AIMP's sound settings alone: if the mode was on, AIMP gets its previous settings back (ReplayGain, normalization, loudness compensation and voice remover with the next start of AIMP). Enabling the plugin again switches the mode back on

## 2.6.0

New:
- Bit-perfect mode (Output tab): one tick switches off everything in AIMP that changes the signal - crossfade and fading on track changes, equalizer, sound effects (chorus, echo, flanger, reverb, bass, stereo enhancer, pitch, speed, tempo, balance), ReplayGain, peak normalization, loudness compensation and voice remover. Two more options appear once it is ticked: AIMP's volume to 100 % and no gapless playback (AIMP's "add pause between tracks", 500 ms). Unticking brings the previous values back; they are kept in the plugin's settings file, so this also works after AIMP was restarted. ReplayGain, normalization, loudness compensation and voice remover are only read by AIMP at start-up: they are written into AIMP.ini after AIMP has closed and take effect with the next start ("Restart AIMP now" on Windows does it right away). A note reminds you not to use Voicemeeter's equalizer or effects
- Languages: the settings page, the statistics, the update messages and the message after an update are available in English, German, Russian and Ukrainian. "Automatic" (the default) follows AIMP's language; another language can be picked on the Advanced tab and is shown right away. Every other AIMP language uses English. The log stays English
- The Statistics tab is now called Advanced. Besides the statistics and the latest log entries it has the language list and a "Settings file" row: Export... saves the plugin's settings to a file of your choice, Import... loads such a file (shown and saved at once), Open folder opens the folder of the settings file and the log. The settings file row on the General tab is gone (it is on the Advanced tab now)
- After an update (automatic or installed by hand) a short message tells you that Prevent Resampling was updated, to which version, and that the changes are on the About tab. AIMP restarts to load a new plugin version, and without the message it was not clear why. Shown once per new version, not after a new installation

Security:
- Updates: the package and its checksum are only downloaded from this repository's GitHub releases, and the package name is only accepted as a plain file name (letters, digits, ".", "-", "_"). A manipulated release answer can no longer make the plugin download from another server or save a file outside the settings folder
- The restart helper (RestartAimp, started through rundll32) only writes a plain number as AIMP's output rate into AIMP.ini
- Device formats are checked before they are handed to Windows: a damaged [Session] entry in the settings file, or an implausible format reported by a driver, is ignored instead of being passed on
- Release tags with extremely long numbers no longer overflow in the version comparison

Improved:
- Switching on Windows: the current format of each device is read once per track instead of several times (each read opened the Windows device list again)
- Settings file and AIMP.ini: case-insensitive comparisons without temporary strings; simpler key detection in AIMP.ini

Fixed:
- ASIO / WASAPI exclusive / DirectSound with the restart option: a maximized AIMP came back maximized, but with another normal size and position - visible as soon as the window was no longer maximized; on Windows 10 with AIMP 6 it could get smaller with every restart (80 % each time when AIMP works with 120 DPI). The plugin in the new AIMP now gives its windows their place back itself, in the window's own DPI units: maximized, minimized or normal as before, with exactly the same normal size and position. AIMP's own button for leaving the maximized state uses a size AIMP keeps itself, and after a start AIMP takes it from its skin file without its interface scaling (x 96/120 at 125 %); the plugin now sets each window to its normal size once (still invisible) before it maximizes it, so AIMP keeps the right size
- ASIO / WASAPI exclusive / DirectSound with the restart option: an AIMP in the background (behind other windows or minimized) came to the front after each restart and took the focus, e.g. from the file manager you were working in. AIMP now comes back where it was: at the same place in the window order, without taking the focus; if Windows activates it anyway, the focus goes back to the window that had it. An AIMP in front stays in front
- ASIO / WASAPI exclusive / DirectSound with the restart option: after several restarts the still image ("seamless" restart) could stop hiding the restart and AIMP was no longer visible. The new AIMP window now goes directly below its still image before the image fades out, and the still image stays until AIMP confirms that its windows are back in place (before, the helper only waited for any AIMP window). AIMP also waits longer for the still image before it closes (slow helper start, e.g. while a virus scanner checks it). The new windows are not drawn until they are in their place (no window in its normal size for a moment, also not when AIMP was minimized), and when AIMP was in front the still image stays on top of the window Windows brings to the front while the old AIMP closes. The still image also takes the focus until the new AIMP is there, so Windows no longer brings another window (e.g. Explorer or the browser) to the front in between
- Voicemeeter started after AIMP was only noticed after restarting AIMP ("installed, not running"). The Remote API session does not see a Voicemeeter that starts later, so the plugin now logs in again while Voicemeeter is not reachable (at most every 3 seconds). If the current track goes to Voicemeeter, the plugin waits for it and switches as soon as Voicemeeter runs - no new track and no AIMP restart needed
- ASIO / WASAPI exclusive / DirectSound with the restart option: a maximized AIMP window came back in normal size after the restart. The restart helper now remembers which AIMP windows were maximized and maximizes them again if AIMP does not do it itself, before the still image fades out
- Output tab: the first entry of "Windows device" was cut off ("(automatic: from AIMP's output, otherwise the W..."). It now reads "Automatic (from AIMP's output)" like the other automatic entries; the note below the list explains the fallback to the Windows default device. The layout test now also checks that such entries fit
- A log file larger than 4 GB was not started anew (only the lower 32 bits of the size were checked)
- AIMP.ini: the UTF-8 byte order mark is now recognised by all three bytes, not only by the first one

## 2.5.4

New:
- Updates: after AIMP has installed a downloaded update, the plugin notices the new files and restarts AIMP by itself (AIMP only loads new plugin versions at start-up). The current track continues at the same position and the sample rates stay as they are
- Downloads for every platform: the .aimppack contains Windows 32-bit, Windows 64-bit and Linux; releases also offer separate win32, win64 and linux-x64 ZIPs for manual installation (plugin file directly in the PreventResampling folder - AIMP does not load it from an x64 sub-folder). The package follows the layout documented in the AIMP SDK (Linux library in x64)
- Built against the AIMP SDK of AIMP 6.00 Beta 7, now part of the repository (sdk/)
- Linux: the Voicemeeter tab shows all options greyed out with a "Linux detected - this tab is disabled" note
- Install and update only with the .aimppack - copying the DLL over an installed version fails while AIMP is running, because Windows does not allow overwriting a loaded DLL. The README explains this
- Official support for AIMP 4 (4.70 or newer) besides AIMP 5 and 6. Tested in real AIMP builds: 6.00 Beta 7 (32/64 bit, Windows and native Linux), 5.40 (32/64 bit) and 4.70, each installed through the .aimppack and with Windows 7, 8, 8.1, 10 and 11 emulated by Wine - switching, Voicemeeter, restoring on exit, the AIMP restart and the settings page all work in every version. AIMP 3 is not supported (no settings pages and no thread service in its plugin interface); the log now says which AIMP service is missing

Fixed:
- ASIO / WASAPI exclusive / DirectSound with the restart option: when the plugin closed AIMP to apply a new rate, "Restore the original sample rates when AIMP closes" also ran - Windows and Voicemeeter went back to the old rate and the restarted AIMP had to switch everything again. A restart by the plugin now keeps the new rates. The rates from before the session are handed to the restarted AIMP (settings file, section [Session]), so closing AIMP yourself still restores them
- Linux: AIMP for Linux resamples every track to its own fixed rate (Sound Output -> Parameters), just like on Windows. The plugin set PipeWire to the track's rate, so a 96 kHz track was resampled twice (96 -> 44.1 kHz in AIMP, 44.1 -> 96 kHz in PipeWire). PipeWire now follows AIMP's rate instead; following each track will need an AIMP restart on Linux (planned). Found while testing the native AIMP 6.00 Beta 7 for Linux, which installs the .aimppack correctly
- AIMP 4: the plugin did not know AIMP's output device (AIMP 4 does not offer it to plugins), so ASIO / exclusive / DirectSound and Voicemeeter were not detected. It is now read from AIMP's settings; the output list on the Output tab shows it greyed out
- AIMP 4: after switching or after a restart by the plugin the track started from the beginning instead of continuing at the same position (AIMP 4 cannot start a track at a position). The plugin now seeks to the position as soon as AIMP has opened the track
- Update check: a failed check (e.g. GitHub not reachable) was shown as "You have the latest version" after the next start and only repeated after the full interval. The About tab now says that the last check failed, and the check is repeated at the next AIMP start (at most once an hour). Error messages from AIMP are logged on one line

## 2.5.3

Fixed:
- The track AIMP restores at start-up was played at the wrong sample rate until another track (or the same one again) was opened. AIMP loads it before the plugin receives any events; the plugin now checks the playing track whenever playback starts and, if the rate differs, switches and continues at the same position
- Enabling the plugin while music is playing now switches to the current track's rate right away

## 2.5.2

Fixed:
- Voicemeeter was not set back to its previous sample rate when AIMP closed ("Restore the original sample rates when AIMP closes"). The rate before the session is now taken from the running engine instead of Option.sr (which can report stale values), and the plugin waits until Voicemeeter really runs at that rate before it logs out
- An output on WDM (e.g. A2) could turn red in Voicemeeter when AIMP closed: after restoring the Windows formats the plugin now restarts the Voicemeeter engine itself

New:
- Output tab: "Bit-perfect playback" notes - what has to be switched off (crossfade, equalizer / DSP, normalisation / ReplayGain, volume below 100 %), that gapless only works between tracks with the same rate, and that with effects on the plugin only prevents resampling
- Voicemeeter tab: note that it is only relevant for Voicemeeter users (Windows only), and that "Auto restart audio engine (all devices)" has to be ticked in Voicemeeter - the Voicemeeter API offers no way to set it from the plugin
- Linux: a Voicemeeter tab that explains why it has no function there (support for Linux mixers such as Pulsemeeter may follow)

## 2.5.1

Fixed:
- About tab: controls were shifted or cut off. All tabs now use the same fixed grid as the Discord Rich Presence plugin (465 x 420, anchored top-left), so nothing drifts or gets cut off when the dialog is resized
- Changelog and statistics are wrapped to the width of their box - no more horizontal scrolling

Changed:
- Settings pages streamlined in the style of the Discord Rich Presence plugin: "label: control" rows, compact info texts; the header line with name and author on the General tab is gone (it is on the About tab)
- About tab: bigger title, sharper avatar, "Install <version>" button and a "What's new?" link when an update is available
- General tab: "Open folder" link to the settings file and log

## 2.5.0

New:
- About tab like the Discord Rich Presence plugin: author, links to GitHub (page, releases, report a problem) and the complete changelog - the installed version always on top
- Update check: looks for a new release on GitHub - at every AIMP start, once a day, once a week or once a month (or switched off). New versions are downloaded, checked (SHA-256) and opened in AIMP, which installs them; a new version is opened automatically only once. "Check now" and "Install" buttons on the About tab
- Settings live in their own file `%APPDATA%\AIMP\PreventResampling.ini` (Linux: `~/.config/AIMP/PreventResampling.ini`), written with all defaults on the first start

Fixed:
- The plugin was enabled after installation if settings of an older version were left in AIMP's configuration. A new installation now always starts disabled; the old entries are removed
- Music started playing by itself when AIMP started with a paused or stopped track. The plugin now only switches while AIMP is really playing; a track that is loaded without playing is handled when you press play

## 2.4.0

- Everything in English: plugin UI, log, README
- Disabled after installation, with a warning to use the plugin only while listening to music exclusively
- AIMP restart is opt-in and limited to the outputs that need it (ASIO, WASAPI exclusive event/push, DirectSound) - never for WASAPI shared. New "ASIO / Exclusive" tab with a clear disclaimer
- Seamless restart: a still image of AIMP covers the restart, so AIMP seems to stay open
- Voicemeeter: all hardware outputs A1-A5 (option) get the right Windows format, not only A1; the engine is only restarted when something actually changed
- Faster switching with hardware-based timing (Fast / Normal / Conservative profile); Voicemeeter is polled instead of waited for
- DirectSound is a separate output type; tabs cleaned up and scrollable

## 2.3.0

- WASAPI exclusive and ASIO: AIMP always plays with its own rate (AIMPSoundOut\DeviceFreq) and reads it only at start-up. New option to restart AIMP with the new rate via a small helper; the track continues at the same position

## 2.2.0

- Voicemeeter hardware outputs are detected even if Voicemeeter names them differently than Windows; their Windows default format is switched too
- WASAPI fix: a wrong byte in IID_IMMDeviceEnumerator prevented the plugin from finding any audio device
- "Test format" button on the Statistics tab

## 2.1.0

- Renamed to Prevent Resampling, version and author on the settings page, gear button in the plugin list
- Voicemeeter switched only once (stale Option.sr read-back) - fixed
- The Windows default format is set with the correct float mix format and verified afterwards
- The AIMP output device on the plugin page no longer overrides AIMP's own selection

## 2.0.0

- Native settings page in AIMP with statistics; Windows 32/64 bit and Linux builds
- WASAPI shared/exclusive, ASIO, Voicemeeter (engine rate and A1 device) and PipeWire support
