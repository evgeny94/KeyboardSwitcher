## Privacy & security hardening (secure defaults, all configurable)

Hi! I'm a user and I like the project. Before installing it I went through the source. These are optional hardening changes. Feel free to cherry-pick only the parts you want.

### What changes

**1. Ignore password fields (default on)**
While focus is on a password field, keystrokes are not buffered, analysed, logged or retyped. Detection works in two layers:
- **Fast path:** a WinEvent `EVENT_OBJECT_FOCUS` hook checks MSAA `STATE_SYSTEM_PROTECTED` and Win32 `ES_PASSWORD`. It can only turn protection ON, so there is no delay before the first keystroke.
- **Re-check:** a background UI Automation worker (MTA thread) reads `IsPassword` after every focus change, click and window switch. It is the only path that turns protection OFF. This matters because in testing, focus events from the shell's `ForegroundStaging` window during window switches cleared the flag while the password field was still focused. UIA calls never run on the keyboard-hook thread, so a hung app can't stall the hook.
- Transitions are logged as `SECURE-FIELD: ON/OFF`, never with text.
- Verified with the debug log in Chrome (password ↔ text input) and WinForms (`UseSystemPasswordChar`).

**2. Redact typed text in the debug log (default on)**
`ws2s()` in `main.cpp` is used only for logging. It now writes `<len=N>` instead of the text. You can turn it off when you need to debug detection.

**3. Updater hardening**
- Downloads use HTTPS only, and only from an allowlist of GitHub hosts. The final host is checked again after redirects.
- `WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP` replaces `..._ALWAYS`.
- The download must return HTTP 200 (before, an error page could be saved as the installer).
- The downloaded installer is checked against the SHA-256 `digest` that the GitHub API publishes for the asset. If it doesn't match, or there is no digest, the file is deleted and not run. Note: this protects against corruption, redirect tampering and a wrong asset. It does not protect against a compromised GitHub account. Code signing would cover that.
- The startup update check is optional (default on).

**4. Settings & diagnostics**
- A new `%APPDATA%\KeyboardSwitcher\settings.json`. It is separate from `user_prefs.json`, which gets wiped on every version change.
- A tray submenu, **Privacy & Security**, to toggle the options. Turning off password protection asks for confirmation.
- `require_verified_updates` can only be changed in the JSON file, on purpose.
- A new `--debug-log` flag turns the debug log on at startup. If the exe folder (Program Files) isn't writable, the log falls back to `%APPDATA%\KeyboardSwitcher`. Before this, logging silently did nothing for installed copies.

**5. Reproducible public builds (`.github/workflows/build.yml`)**
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
