# GIFs for the release firmware

CI puts these into every firmware it builds (with the same encoder as the web constructor):

- **`standby.gif`**: the standby screensaver (*Settings → System → GIF screensaver*).
- **`boot.gif`** (optional, not present by default): the boot animation (*Settings → System → Boot GIF*).

Replace or add a file and push. To try a GIF on your own iron without touching the repository, use the web
constructor instead: [`tools/hs02-gif-studio`](../tools/hs02-gif-studio/README.md).
