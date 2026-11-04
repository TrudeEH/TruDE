Commit after completing each request, so you or the user can rollback if necessary, unless the user says otherwise.
Do not create or edit a README file unless the user asks.
Assume the base system TruDE is meant for is a minimal debian installation.
Always use `sh` for scripting instead of `bash`.
Never add tests to the repository. If you need to test your changes do it in a temporary way and don't commit scripts used for testing.
Never add reports, benchmarks, or any other temporary information to the repo, unless the user asks for it to be included.
MIGRATIONS.MD tracks temporary migration code kept for existing installations, such as checks that remove previously installed packages, obsolete files or services, or convert old settings. For each such change, record the code location, what it migrates, and which code can be removed once all users have updated. This is a retirement checklist for upgrade compatibility code, not a general upgrade guide or a list of new dependencies, features, or build instructions. Keep entries while the migration code exists; remove them when that code is retired. Permanent setup and ongoing compatibility behavior do not belong in this file.

## Visual design

Use the libadwaita dark palette across all system UI. Use `#222226` for the window background, `#38383c` for raised surfaces, white for primary text, and `#aaaaaa` for muted text. The accent is light orange `#ffbe6f`, with `#ffa348` for stronger hover states and `#222226` for text on the accent.
