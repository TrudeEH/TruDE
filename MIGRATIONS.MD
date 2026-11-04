# Migrations

Temporary upgrade code to retire once all existing installations have migrated.

## 2b5a1b31 (04 Nov 2026): Remove Seth automation

Retirement: once all users have updated, remove `remove_agent_timer()` and its
call in `install.sh`. Keep normal Seth build/install behavior.

Existing installations must stop and remove Seth's old systemd user timer and
service, then rebuild Seth. Running the updated installer performs this cleanup
and rebuild automatically. To migrate manually, run these commands as your normal
user from the repository root:

```sh
systemctl --user disable --now TruDE-agent.timer
systemctl --user stop TruDE-agent.service
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
rm -f -- "$config_dir/systemd/user/TruDE-agent.service" \
    "$config_dir/systemd/user/TruDE-agent.timer" \
    "$config_dir/systemd/user/timers.target.wants/TruDE-agent.timer"
systemctl --user daemon-reload
sh scripts/agent/build.sh
```

The systemctl commands may report missing units if the timer was never installed.
Close and reopen Seth after rebuilding. Automation tools, the Automation tab, and
`--run-due` / `--run-task` are removed. Memory, Settings, and Help now use F3, F4,
and F5 respectively.

Existing settings and chat history are preserved. The old schedule file at
`${XDG_DATA_HOME:-$HOME/.local/share}/TruDE-agent/automations.json` is no longer
read or updated; it can be kept as an archive or deleted manually.

## Remove hypridle from existing installations

- Location: `install.sh`, `install_packages()` and the hypridle entries in
  `link_configs()`'s `legacy_pair` loop.
- Migration: stop/disable the old service, terminate hypridle, uninstall its
  package, and remove installer-owned launcher/configuration symlinks.
- Retirement: remove the hypridle package check and cleanup block and the two
  hypridle legacy pairs once all users have updated.

## Remove obsolete nnn links

- Location: `install.sh`, nnn entries in `link_configs()`'s `legacy_pair` loop.
- Migration: remove installer-owned nnn opener, environment, and plugin symlinks.
- Retirement: remove those three legacy pairs once all users have updated. When
  both nnn and hypridle cleanup are retired, remove the entire legacy loop.

## Disable the replaced Waybar service

- Location: `install.sh`, `main()`'s `systemctl --user disable --now waybar.service`.
- Migration: stop and disable the previous bar when installing the native bar.
- Retirement: remove this command once all users have updated.
  The `waybar` settings key is still actively used and is not part of this cleanup.

## Upgrade older Seth settings

- Location: `scripts/agent/src/store.c`, `load_config()`.
- Migration: convert version 1 settings to version 2 with separate shell and
  memory servers; replace the old stock agent-name/prompt text; add the optional
  disabled 9Router server to existing configurations.
- Retirement: remove the version upgrade, old prompt replacement branches, and
  existing-configuration 9Router insertion once all saved settings have been
  upgraded. Retain current defaults, validation, and normal settings persistence.
