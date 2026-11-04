# Migrations

## 2b5a1b31 (04 Nov 2026): Remove Seth automation

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
