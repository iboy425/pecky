# Repository workflow

- Treat `https://github.com/iboy425/pecky` (`origin`) as the canonical remote.
- After each user-requested code or documentation change, run the relevant checks, commit the completed change, and push the current `main` branch to `origin`.
- Keep commits focused and describe the delivered outcome in the commit message.
- Never commit captured participant CSV files, trained model binaries, Python caches, build output, credentials, or the local `硬件资料/` source-material directory.
- If a push cannot complete because authentication or networking is unavailable, preserve the local commit and clearly tell the user that remote synchronization is pending.
