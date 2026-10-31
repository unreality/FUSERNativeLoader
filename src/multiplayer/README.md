# FUSERMultiplayer

The client side of multiplayer against a fuserd server, as a bundled mod (`mods/FUSERMultiplayer/`, C++ tier). It
hosts nothing: sessions run on the FDS dedicated server. It links `winhttp.lib` and so needs the loader's
`winhttp.dll`; it is not a standalone proxy. `enabled=0` in its `mod.ini` (or a `.disabled` folder) runs the other mods
without it.

The server is the one the game itself talks to: the Live environment's `ClientURL` in `Game.ini`'s
`EnvironmentSettings`. `StoreURL=` in `FuserNative.ini` overrides it for this mod's own requests.

| File | |
|---|---|
| `plugin_entry.cpp` | `fnl_mod_init`: the Mods tab rows, then `host_hook_start` |
| `host_hook.cpp` | everything below except the parts named here; its per-build address table `kVerified` |
| `p2p_tunnel.cpp` | the UDP tunnel agent: STUN, hole punching, the relay, and the song transfers' data path |
| `pak_check.cpp` | `check_song_pak`: the layout check every song pak from another player or a community link passes |
| `community_fetch.cpp` | community song archives (zip, rar4, rar5) through the static libarchive in `lib/libarchive/` |
| `server_content.cpp` | server content: fuserd's items, kept in `Fuser/Content/ServerContent/` |

## What it does

- **The matchmaking join.** Create Private Session, a public match and a session browser join all end in the game's
  matchmaking join; `JoinAddressOverride=<ip:port>` sends it somewhere other than the address fuserd gave (two
  machines behind one NAT without hairpinning).
- **Custom songs in other players' crates.** Twice a second in a multiplayer game, the songs in each performer's crate
  that aren't on this machine are asked of fuserd: from its catalogue, from a community link (the creator's public
  Google Drive; the archive is downloaded and extracted here), or, when fuserd has neither, from a player in the
  session who has the song (a song transfer through the tunnel agent, signalled by fuserd). Viewers' crates are
  never fetched: a viewer never plays. A fetch that fails is tried again after 2 minutes, up to 3 times a session.
  Official DLC never goes this way: the game's own DLC path handles it.
- **Song checks.** A pak from another player or a community link is mounted only if `check_song_pak` passes it: an
  unencrypted, uncompressed v8 pak of at most 64 MB holding only that song's folders and album art, whose packages
  hold only the song classes - no Blueprints. Its `.sig` chunk table is checked too. A sender checks its own file the
  same way before offering it.
- **Sync progress.** "Synchronising N custom songs..." and "Custom songs synchronised" (or how many couldn't be
  fetched) on the event feed, and in the lobby a **Custom Songs Sync** panel above Performers with a row per player
  (profile picture, name, status, bar), from fuserd's sync states. A player without the loader shows no status.
- **The missing-stem skip.** A remote DJ's stem whose song isn't on this machine stalls that DJ's whole queue in the
  stock game. After 1.5 s (longer while a store install that may bring it is running, up to 30 s) the stem is skipped
  so the rest of the queue plays; it simply isn't heard here.
- **The Diamond store.** A song bought there that isn't installed is installed (always permanently), and so are owned
  store songs restored at login; tiles of songs not installed take their title, artist and art from fuserd's
  catalogue.
- **The crate song limit.** A server may cap the songs in the crate a player enters multiplayer or the Main Stage
  with (fuserd `multiplayer.max_crate_songs`, at least 24): a bigger crate fails the game's own crate check, with a
  Crate Error popup naming the limit. The same call reports the loader's version and the exe build to fuserd, at
  login and on each entry to multiplayer, which keeps stock FUSER players apart from loader users.
- **Shared mixes with custom songs.** When a mix's playback loads, its song list comes from fuserd; the songs this
  machine lacks are fetched like crate songs, and the load is held for them (at most 2 minutes) behind a "Custom
  Songs" status popup. Songs that couldn't be fetched are named afterwards. Mixes the game only previews (the
  playback-complete screen's next mixes, the mix browser) aren't checked.
- **Server content.** fuserd's items (projector screens, outfit parts) download at login into
  `Fuser/Content/ServerContent/` and register locked until a Diamond store purchase, an event reward or fuserd's
  claimed assets unlock them. The title screen waits for the login sync, at most `ServerContentWaitSeconds`, before
  the main menu.

**Player-hosted sessions** (fuserd makes the creating player the host when it runs no FDS servers) are a stub: the
log says a local FDS server is needed and the stock join runs, which fails with the game's connection error. The P2P
tunnel's signalling (`P2P=1`) exists for those sessions; fuserd signals P2P for player-hosted sessions only, so with
FDS sessions it has nothing to connect to and the join goes ahead as normal. The tunnel agent itself is in use: song
transfers between players run over it in every session.

## Settings (Settings > Mods > Multiplayer)

| Row | Key | Choices | |
|---|---|---|---|
| Download Songs | `DownloadSongs` | No / Temporarily / **Permanently** | where songs from other players' crates, the store's restored songs and community songs go: nowhere (fetching pauses), this run's session folder (`%LOCALAPPDATA%\Fuser\Saved\FuserNative\SessionSongs\<pid>\`, never mounted at the next launch), or `custom_songs`. Without the key, the older `StoreAutoInstall=0` reads as No |
| Download Mix Songs | `DownloadMixSongs` | No / **Temporarily** / Permanently | the same, for a shared mix's songs |
| P2P Song Download | `P2PSongDownload` | Off / **From Followed** / Always | which players a song fuserd doesn't have is asked of: none, the players you follow, or everyone whose crate holds it. A blocked player is never asked. Greyed out while Download Songs is No |
| Send Songs to Other Players | `SendSongs` | **Off** / On | whether this machine answers other players' requests with its custom songs (including ones it received Temporarily) |
| Upload Speed Limit | `SendSongsKBps` | 256 KB/s - 4 MB/s, default **2 MB/s** | per transfer; greyed out while Send Songs is off |
| Simultaneous Uploads | `SendSongsMax` | 1 - 4, default **2** | greyed out while Send Songs is off |

The download modes are read at most once a second, so a change applies mid-session; the upload rows apply to the
next request, not to transfers under way.

## FuserNative.ini

| Key | Default | |
|---|---|---|
| `StoreURL` | the Live `ClientURL` | fuserd's API base for this mod's requests |
| `JoinAddressOverride` | none | `<ip:port>` a matchmaking join goes to instead |
| `SyncNotices` | 1 | 0: no sync lines on the event feed |
| `SyncProgressBar` | 1 | 0: no Custom Songs Sync panel |
| `MissingCelSkip` | 1 | 0: no missing-stem skip |
| `MissingCelGraceMs` | 1500 | how long a missing stem waits before it is skipped |
| `StoreInstallWaitMs` | 30000 | how long it waits while a store install is running |
| `ServerContentWaitSeconds` | 30 | the title screen's wait for server content; 0 never waits |
| `DiagnosticLogs` | 0 | 1: the clock samples during a set and the state / connection watch after a join, in every build |
| `HostLobbyClockTrace` | 0 | 1: clock samples in the lobby too (with `DiagnosticLogs=1`) |
| `P2P` | 0 | 1: P2P signalling for player-hosted sessions (see above) |
| `P2PJoinTimeoutMs` | 10000 | how long a P2P join waits for the tunnel before the normal join |
| `P2PRelay`, `P2PForceRelay` | 1, 0 | use fuserd's relay when it offers one; only the relay |
| `P2PSkipLan`, `P2PAllAdapters` | 0, 0 | leave out LAN candidates; include adapters without a default gateway |
| `P2PTunnelPort` | 0 (any) | the tunnel socket's UDP port |
| `P2PTunnelListen`, `P2PTunnelHostHere`, `P2PTunnelGamePort`, `HostBeaconPort`, `P2PTunnelPeer`, `P2PLocalPort` | | the tunnel's manual test mode: endpoints from the ini instead of fuserd (host: `P2PTunnelHostHere=1` and `P2PTunnelListen`; joiner: `P2PTunnelPeer`, `P2PLocalPort` 17777 and `JoinAddressOverride=127.0.0.1:<port>`) |

Messages go to `FuserNative.log` (`FuserNative.2.log` for a second instance from the same install), tagged `[host]`,
`[p2p]`, `[songs]` and so on.

## Debug build

The Debug configuration (`FNL_DEBUG`) adds trace hooks a release build never installs: the drop chain and deck trace
(`DeckTrace`, on by default there), the leave trace (`LeaveTrace=1`), and these keys, with the game window focused:

| Key | |
|---|---|
| Ctrl+Shift+J | join `HostAddress` (default `127.0.0.1:7777`) directly, with `JoinOptions` (an FDS needs `?playerSessionId=?gameSessionId=?hypeUserId=?buildConfig=`); unlike a plain `open`, it also loads the lobby music and sets the multiplayer game mode |
| Ctrl+Shift+T | an 8 s ProcessEvent trace |
| Ctrl+Shift+R | a state report |
| Ctrl+Shift+U | install the song `StoreTestInstall=<shortname>` from fuserd |
| Ctrl+Shift+N | a STUN probe (`P2PProbe=1` runs it once after login) |
| Ctrl+Shift+X | the song transfer test between two machines, without fuserd (`P2PTunnelPort`, `XferTestPeer=<ip:port>`, and on the sender `XferTestSend=<path to the pak>`) |

## Other game builds

The game functions this mod calls and hooks are in `kVerified` (`host_hook.cpp`), one row per build; its hooks
install only on a build whose row is complete (`[host] build <name>: multiplayer addresses verified`). See
Supported executables in the repository's `README.md` for adding a build with `tools/derive_mp_build.py`.
