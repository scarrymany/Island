# Island HUD implementation

Build a native C++20 / Qt 6.8 Windows music island, following the user's C++ preference. The supplied image, MusicUI and YEET17PCSET inform the layout. No external application code is copied. Windows GSMTC provides metadata and transport commands.

## Deliverables and ownership

1. MediaBridge: C++/WinRT worker, discovery, subscriptions, source selection, bounded artwork, timeline and playback commands.
2. WindowsIntegration: hotkey, startup, Core Audio master volume, topmost, click-through and native backdrop.
3. ConfigStore and SettingsWindow: validated atomic JSON, profiles, themes, import/export and live settings.
4. HudWindow and Layout: frameless island, element visibility and dragging, presets, per-monitor placement, animations and inactivity hiding.
5. Application: tray lifecycle, single-instance IPC, graceful shutdown and diagnostics.
6. UpdateService and GitHub Actions: verified GitHub Releases, native installer, portable archive and release publication.

## Shared interfaces

MediaBridge publishes MediaSnapshot and source ID/name pairs using Qt signals. Commands are previous, play_pause, next and seek in seconds. ConfigStore publishes validated QJsonObject settings. Element coordinates are unscaled HUD-local coordinates; monitor positions are logical screen-local coordinates.

## Verification

- Exercise persistence validation, profile/theme isolation, malformed imports, timelines, source selection, native API wrappers with mocks, and layout bounds.
- Run real Qt widget smoke tests for controls, drag/drop, settings changes, monitor placement, animations and tray shutdown.
- Probe real Windows media session APIs on this machine, record actual state without substituting demo data.
- Build a Windows executable, launch it, inspect screenshots and runtime logs.
- Document exclusive fullscreen limitations, source-provided metadata and master volume scope.
