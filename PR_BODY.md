## Privacy & security hardening (secure defaults, all configurable)

Hi! I'm a user and I like the project. Before installing it I went through the source. These are optional hardening changes. Feel free to cherry-pick only the parts you want.

### What changes

**1. Ignore password fields (default on)**
While focus is on a password field, keystrokes are not buffered, analysed, logged or retyped. Detection works in two layers:
- A WinEvent `EVENT_OBJECT_FOCUS` hook only records the focus target and wakes a background worker (MTA thread). The worker checks UI Automation `IsPassword`, MSAA `STATE_SYSTEM_PROTECTED` and Win32 `ES_PASSWORD`. No cross-process call runs on the keyboard-hook thread, so a hung app can't stall input.
- Fail closed: from a focus change until the worker's verdict (typically < 50 ms) keys are treated as protected. A generation counter means a verdict computed before the latest focus change can never turn protection off, and a check where no API could answer keeps the previous verdict. UIA timeouts are capped at 1 s.
- Re-checks also run after clicks and window switches. In testing, focus events from the shell's `ForegroundStaging` window during window switches otherwise cleared the flag while the password field was still focused.
- Transitions are logged as `SECURE-FIELD: ON/OFF`, never with text.
- Verified with the debug log (current version): Firefox and Edge (password field → `SECURE-FIELD: ON`, text field → no ON) and the Windows credential dialog (`Get-Credential`). Chrome and WinForms were verified on the earlier version. TODO before opening: re-check Chrome/WinForms and an Electron app (Discord).

**2. Redact typed text in the debug log (default on)**
`ws2s()` in `main.cpp` is used only for logging. It now writes `<len=N>` instead of the text. You can turn it off when you need to debug detection.

**3. Updater hardening**
- Downloads use HTTPS only, and only from an allowlist of GitHub hosts. The final host is checked again after redirects.
- `WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP` replaces `..._ALWAYS`.
- The download must return HTTP 200 (before, an error page could be saved as the installer).
- The downloaded installer is checked against the SHA-256 `digest` that the GitHub API publishes for the asset. If it doesn't match, or there is no digest, the file is deleted and not run. Note: this protects against corruption, redirect tampering and a wrong asset. It does not protect against a compromised GitHub account. Code signing would cover that.
- Only this repo's release assets are accepted (`https://github.com/kertser/KeyboardSwitcher/releases/download/...`), and `tag_name` is restricted to version characters before it goes into a file name.
- The installer is saved to a random subdirectory of `%TEMP%` and held open share-read from before hashing until it is launched, so it can't be swapped between verification and execution.
- The startup update check is optional (default on).

**4. Settings & diagnostics**
- A new `%APPDATA%\KeyboardSwitcher\settings.json`. It is separate from `user_prefs.json`, which gets wiped on every version change.
- A tray submenu, **Privacy & Security**, to toggle the options. Turning off password protection asks for confirmation.
- `require_verified_updates` can only be changed in the JSON file, on purpose.
- A new `--debug-log` flag turns the debug log on at startup. If the exe folder (Program Files) isn't writable, the log falls back to `%APPDATA%\KeyboardSwitcher`. Before this, logging silently did nothing for installed copies. The log is now thread-safe (the updater logs from background threads).
- `settings.json` is written atomically (temp file + rename) and each key is type-checked on load.

**5. Public CI builds (`.github/workflows/build.yml`)**
- Builds the ZIP and the NSIS installer on `windows-2022` with MinGW and publishes `SHA256SUMS.txt`.
- ONNX Runtime 1.24.3 comes from Microsoft's official NuGet package. The workflow fails if `onnxruntime.dll` isn't Microsoft-signed. FYI: the DLLs in your v1.7.0 release are byte-identical to the NuGet ones, which is good.

### Files
- New: `cpp/include/Security.h`, `cpp/src/Security.cpp`, `.github/workflows/build.yml`
- Changed: `cpp/src/main.cpp`, `cpp/CMakeLists.txt` (adds `oleacc ole32 oleaut32 bcrypt uuid`), `cpp/resources/resource.h`, `README.md` (new "Privacy & Security" section)

### Tested
- CI build is green with no compiler warnings.
- The portable build starts, and `settings.json` is created with secure defaults.
- Windows Defender scan: clean.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
