# Monologue

A simple webcam recorder for Omarchy. Choose your camera and microphone once, then press **Space** to record. Press again to pause or resume the same take. Stop the take and it opens right away in a built-in editor: trim either end, cut ranges out of the middle, then **Save**.

- Remembers camera and microphone by device ID; missing inputs never silently switch.
- Automatically selects the camera's maximum advertised video resolution, preferring 30 fps at that resolution. Preview preserves the whole frame.
- Live microphone meter with peak hold and clipping indication, including while paused. No microphone playback through your speakers.
- Explicit No audio option for silent recordings.
- Live Omarchy accent syncing, including theme symlink switches. Controls and dialogs follow Hyprland's active corner rounding, including personal overrides. Dark chrome and a yellow fallback follow Omacut; button foregrounds adapt for contrast.
- Built-in editing: split the take into clips on an Omacut-style filmstrip, trim each clip with its handles, see where you paused, and undo. Edits never touch the original.
- H.264 MP4 with AAC audio, retained originals, and atomic Save.

## Build and run

Install a C++17 compiler, `make`, `qt6-base`, `qt6-declarative`, `qt6-multimedia` (Qt 6.8 or newer), `libpulse`, and `ffmpeg`. Microphone capture requires a PulseAudio-compatible server, such as PipeWire-Pulse on Omarchy. Then:

```sh
./bin/build
./build/monologue
```

The save dialog requires `xdg-desktop-portal` and your desktop's portal backend. Monologue uses Qt's FFmpeg multimedia backend for timestamped capture; the binary selects it automatically.

### Arch / Omarchy

To build and install the Arch package (requires `makepkg`; extra arguments are passed to it):

```sh
./bin/install
```

### Fedora

Monologue also runs on Fedora; Arch, Omarchy, and Hyprland are not runtime requirements. Without Omarchy or Hyprland, it uses the default accent and corner rounding. Qt 6.8 or newer is required.

Install the build dependencies. These commands refresh repository metadata and disable optional (weak) dependencies for this transaction only; required dependencies are still installed. This avoids pulling in unrelated integrations for software you already have, such as Ghostty's Vim plugin.

```sh
sudo dnf --refresh --setopt=install_weak_deps=False install \
  gcc-c++ make pkgconf-pkg-config \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtmultimedia-devel \
  pulseaudio-libs-devel
```

For a GNOME Wayland desktop, the runtime integration packages are:

```sh
sudo dnf --refresh --setopt=install_weak_deps=False install \
  qt6-qtwayland pipewire-pulseaudio \
  xdg-desktop-portal xdg-desktop-portal-gnome
```

Use your desktop's portal backend instead of `xdg-desktop-portal-gnome` on other desktops (for example, `xdg-desktop-portal-kde` on KDE).

You also need `ffmpeg` and `ffprobe` with H.264/AAC encoding support. Edited exports explicitly use `libx264`, so Fedora's restricted `ffmpeg-free` alone is not sufficient. If needed, follow [RPM Fusion's multimedia instructions](https://rpmfusion.org/Howto/Multimedia) to install full codec support. Qt's FFmpeg backend must also have H.264/AAC encoders available for recording. Check the command-line encoders with:

```sh
ffmpeg -hide_banner -encoders | grep -E 'libx264|aac'
```

Then run the installer **as your normal user, without sudo**:

```sh
./bin/install
~/.local/bin/monologue
```

On Fedora, the script builds the application and installs its binary into `~/.local/bin`, plus a desktop launcher, icon, and license into `${XDG_DATA_HOME:-$HOME/.local/share}`. The launcher uses an absolute executable path, so it does not rely on the desktop session's `PATH`. Add `~/.local/bin` to your shell's `PATH` if you want to launch it simply as `monologue`.

The installer does not install dependencies, enable repositories, or create an RPM. Re-running it rebuilds and replaces the user-local installation. Fedora installation takes no arguments; Arch's `makepkg` options do not apply.

The build and test scripts select `pkg-config` from Qt's host prefix when available. This avoids Homebrew's `pkg-config` shadowing Fedora's version and incorrectly reporting that installed Qt development packages are missing. You can explicitly select tools with the `QMAKE` and `PKG_CONFIG` environment variables, for example:

```sh
PKG_CONFIG=/usr/bin/pkg-config ./bin/install
```

## Shortcuts

| Key | Action |
| --- | --- |
| Space | Record / pause / resume; play / pause while editing |
| Enter or Ctrl+Enter | Stop the take and edit it |
| Esc | Confirm discarding the clip and start over |
| Q | Quit (asks first if the take is still recording or unsaved) |
| ? | Keyboard help |

The shutter button records and stops; the round button beside it pauses and resumes.

While editing, the timeline shows your take as one or more clips, each framed with a handle at both ends. Whatever falls outside every clip is cut.

- **Double-click a clip** to split it there. Splits catch on pause marks.
- **Drag a handle** to trim that clip. Dragging the handles at a split apart removes the part between them.
- **Double-click a gap**, or the line between two touching clips, to join them again.
- **Hover a clip** and click its **×** to remove it.

Handles catch on pause marks, neighbouring clips, and the playhead. Playback plays the clips in order and skips the gaps.

| Key | Action |
| --- | --- |
| Left / Right | Move the playhead 1 second (Shift: 5 s, Alt: 0.2 s) |
| [ / ] | Jump to the previous / next pause or clip edge |
| S | Split the clip at the playhead |
| X, Delete | Remove the clip under the playhead; in a gap, restore it |
| Ctrl+Space / Alt+Space | Move the start / end of the clip under the playhead to the playhead |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Z | Zoom to the clip under the playhead; again to zoom out |
| Ctrl+S | Save |

## Storage and settings

Takes are recorded into the application's XDG data directory, normally `~/.local/share/omacom/monologue/recordings/`, one folder per take. Saving an unedited clip copies the original without re-encoding; an edited clip is re-encoded from its clips (H.264 CRF 18, AAC) into a hidden temporary file beside the destination, then renamed into place. Saving into Monologue's own recordings folder is refused.

A take lives only as long as you're working on it: starting a new recording or quitting deletes it. If you haven't saved it since your last change, Monologue asks first, and offers to save. If Monologue is killed or crashes with a take open, the next launch reopens that take in the editor if it was last written less than an hour ago (its edits and pause marks are not kept); an older one is deleted. A take that can't be played back is deleted. Each take holds a lock while in use, so a second Monologue window never mistakes it for an interrupted one.

Device IDs and the last save directory live in the application's Qt settings, normally `~/.config/omacom/monologue.conf`. The theme is read from `~/.local/state/omarchy/current/theme/colors.toml`, as in Omacut. No Omarchy config is changed.

A disconnected source or encoder error stops the take and preserves its files. An unfinalized MP4 after a crash or power loss may not be playable. Maximum resolution is never silently reduced: if the device or encoder cannot sustain it, Monologue reports the problem.

## Development and validation

The test runner requires Python 3 in addition to the build dependencies.

```sh
./bin/test
```

Installer regression tests can also run independently, with only Python 3 and standard shell tools:

```sh
python3 tests/install_tests.py
```

They use temporary homes, mocked build tools, and fixture distribution metadata; they do not build the application, install packages, or change your real home directory.

Tests cover format ranking, pause timing, PCM levels, atomic saving, the edit model, undo, reopening an interrupted take, take deletion and locking, filmstrip generation, edited exports (duration and content on both sides of a cut), the first frame showing on entering the editor, theme file changes and symlink swaps, actual H.264/AAC encoding and decoding, and native QML keyboard/layout behavior with simulated sources. They run offscreen and do not activate a camera or microphone. The capture-independent backend uses an injected file picker for save and recovery tests. The QML tests write inspection screenshots to `/tmp/monologue-ui-*.png`.

The recording engine forwards native camera frames and one shared microphone PCM stream to timestamped Qt inputs. Microphone timestamps account for the audio server's measured capture latency, including buffered samples. The writer removes paused intervals by capture time and accepts delayed samples from before Pause or Finish. Finish briefly waits for those samples before closing the recording. Qt's audio encoder counts samples, so the writer pads genuine capture gaps and trims overlaps to maintain the common timeline. Preview and metering remain live while paused. Finish remuxes the MP4 without re-encoding to normalize packet durations and put playback metadata at the front of the file.

A synchronization regression test encodes and decodes matching flashes and audio clicks with delayed delivery, including events just before Pause and Finish. It checks event alignment within 10 ms, rather than only comparing stream durations.

Run `./bin/test-camera` explicitly for a short real camera/microphone recording with a pause and resume. It uses separate `monologue-camera-check` settings and storage, and retains its output for inspection. It is never run by `bin/test`.

A real-camera check is still needed for each device/backend combination, particularly maximum-resolution throughput and synchronization over long takes. Short 1920 × 1080 and 3840 × 2160 hardware checks passed on this machine; they do not establish physical lip sync or sustained throughput.

## License

MIT. The portal file picker, theme-watching approach, and filmstrip trimming interface derive from [Omacut](../omacut), copyright David Heinemeier Hansson; attribution is retained in [LICENSE](LICENSE).
