# Are.na / 3DS

An [Are.na](https://www.are.na) client for the Nintendo 3DS, modeled on the iPhone app.

## Install

Copy `arena.cia` to the SD card and install it with FBI, or run `arena.3dsx` from the Homebrew Launcher. The app needs Wi-Fi.

## Logging in

1. Open the app; it shows a QR code. Your phone has to be on the same Wi-Fi as the 3DS.
2. Scan it. A small page served by the 3DS opens.
3. Tap **Log in with Are.na** and approve. **Safari will not be able to open the page it lands on.**
4. Copy that page's whole address, go back to the first tab, paste it and tap **Send to 3DS**.

The token is saved to `sdmc:/3ds/arena/token.txt` and doesn't expire.

## Build

Needs Docker. From this folder:

```sh
./build.sh
```

This compiles in the `devkitpro/devkitarm` image and packs the CIA with the bundled `makerom` and `bannertool`, producing `arena.3dsx` and `arena.cia`.

Logging in uses an OAuth app registered at [are.na/oauth/applications](https://www.are.na/oauth/applications) with the redirect URI `https://localhost/callback`. Its Client ID is in `source/config.h`. To build with your own:

```sh
ARENA_CLIENT_ID=... ARENA_REDIRECT_URI=https://localhost/callback ./build.sh
```

An installed app can also be pointed at another OAuth app with `sdmc:/3ds/arena/oauth.txt`, holding `client_id=`, `redirect_uri=` and, if needed, `client_secret=` lines.

## Layout

| | |
|---|---|
| `source/main.c` | Screens, drawing, input, the add sheet |
| `source/api.c` | Are.na API: fetching, parsing, uploads and edits |
| `source/store.c` | Screens' data, loaded on a background thread |
| `source/img.c` | Image loading, JPEG decoding, texture cache |
| `source/auth.c` | Login: PKCE and the page the 3DS serves |
| `source/add.c` | Camera, microphone, SD card files, posting |
| `source/http.c` | libcurl helpers |
| `source/qrcodegen.c` | QR codes, by [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) (MIT) |

## Limits

- **No search.** Are.na's search API is for Premium accounts only. I am cheap. Feel free to add it if you want.
- **No follow.** Are.na's API has no way to follow or unfollow.
- **No video, audio playback or PDFs.**
- **Emoji show as `?`.**
